/*
 * xhci.c -- драйвер хост-контроллера xHCI (USB 3.x / 2.0) для ArtyomX64.
 *
 * Адаптация простого xHCI-драйвера OpenBSD (sys/dev/usb/xhci.c и
 * xhcireg.h, Copyright (c) 2014-2015 Martin Pieuchot, ISC license)
 * под устройство ядра ArtyomX64:
 *
 *   - вместо bus_space/bus_dma -- прямые volatile-обращения к MMIO
 *     (BAR0 через HHDM-отображение Limine: vmm_phys_to_virt()) и
 *     физически непрерывные страницы из PMM;
 *   - вместо usbd_xfer/usbdi -- синхронные вызовы в стиле uhci.c:
 *     xhci_control_transfer()/xhci_transfer();
 *   - вместо прерываний -- polling Event Ring (в ядре включён только
 *     legacy PIC для PS/2, MSI не настроен; UHCI-драйвер работает
 *     так же);
 *   - вместо таймеров -- калиброванные busy-циклы delay_ms()
 *     (sleep() ядре недоступен: IRQ0 замаскирован).
 *
 * Сохранены ключевые приёмы OpenBSD-драйвера:
 *   - последовательность сброса: остановка (USBCMD=0, ждать HCH),
 *     затем HCRST, ждать сброса бита HCRST и флага CNR (xhci_reset);
 *   - Address Device в два шага: сначала BSR=1 (устройство остаётся
 *     на адресе 0, состояние DEFAULT -- некоторые Low/Full-Speed
 *     устройства не принимают SET_ADDRESS до чтения дескриптора),
 *     после GET_DESCRIPTOR(8) и SET_ADDRESS -- BSR=0 с настоящим
 *     bMaxPacketSize0 (xhci_context_setup / xhci_setaddr);
 *   - конструирование control-TD: Setup TRB (IDT, TRT=IN/OUT) ->
 *     Data TRB (DIR_IN|ISP, IOC для IN) -> Status TRB (IOC,
 *     DIR_IN для OUT/zero-length) (xhci_device_ctrl_start);
 *   - размеры контекстов зависят от HCCPARAMS1.CSZ (32 или 64 байта);
 *   - разбор расширенных возможностей (xECP id 2) для разделения
 *     портов на USB2/USB3 протоколы;
 *   - warm reset (WPR) для USB3-портов, обычный PR для USB2.
 *
 * Осознанные ограничения (уровень "простого стека" из uhci.c):
 *   - transfer ring на 64 TRB без multi-TRB TD (данные <= 4KiB за раз);
 *   - потоков (streams) нет, burst = 0, isochronous нет;
 *   - USB-хабы не энумерируются, только корневые порты;
 *   - usb_transfer() для IN-направления настраивает конечную точку
 *     как interrupt IN (этого достаточно для boot-мыши), для OUT --
 *     как bulk OUT;
 *   - без очистки кэша: на x86-64 DMA от PCI когерентен.
 */

#include "xhci.h"
#include "../pci.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"

/* HHDM-смещение Limine (определено в boot/limine_boot.c). */
extern u64int limine_hhdm_offset;

/* ==================================================================
 * Регистры (по образцу OpenBSD xhcireg.h).
 * ================================================================== */

/* Capability-регистры (от начала BAR0). */
#define XC_CAPLENGTH   0x00
#define XC_HCIVERSION  0x02
#define XC_HCSPARAMS1  0x04
#define XC_HCSPARAMS2  0x08
#define XC_HCCPARAMS1  0x10
#define XC_DBOFF       0x14
#define XC_RTSOFF      0x18

#define XHCI_HCS1_N_PORTS(x)     (((x) >> 24) & 0xff)
#define XHCI_HCS1_DEVSLOT_MAX(x) ((x) & 0xff)
#define XHCI_HCS2_SPB_MAX(x)     ((((x) >> 16) & 0x3e0) | (((x) >> 27) & 0x1f))
#define XHCI_HCC_CSZ(x)          (((x) >> 2) & 0x1)
#define XHCI_HCC_PPC(x)          (((x) >> 3) & 0x1)
#define XHCI_HCC_XECP(x)         (((x) >> 16) & 0xffff)

/* Operational-регистры (от базового операционного блока). */
#define XOP_USBCMD     0x00
#define XOP_USBSTS     0x04
#define XOP_PAGESIZE   0x08
#define XOP_DNCTRL     0x14
#define XOP_CRCR       0x18
#define XOP_DCBAAP     0x30
#define XOP_CONFIG     0x38
#define XOP_PORTSC(n)  (0x3F0u + (n) * 0x10u)   /* n -- номер порта с 1 */

#define XHCI_CMD_RS    0x00000001u
#define XHCI_CMD_HCRST 0x00000002u
#define XHCI_CMD_INTE  0x00000004u
#define XHCI_CMD_HSEE  0x00000008u

#define XHCI_STS_HCH   0x00000001u
#define XHCI_STS_HSE   0x00000004u
#define XHCI_STS_EINT  0x00000008u
#define XHCI_STS_PCD   0x00000010u
#define XHCI_STS_CNR   0x00000800u
#define XHCI_STS_HCE   0x00001000u

/* Порты (PORTSC). */
#define XHCI_PS_CCS        0x00000001u
#define XHCI_PS_PED        0x00000002u
#define XHCI_PS_PR         0x00000010u
#define XHCI_PS_GET_PLS(x) (((x) >> 5) & 0xf)
#define XHCI_PS_PP         0x00000200u
#define XHCI_PS_SPEED(x)   (((x) >> 10) & 0xf)
#define XHCI_PS_LWS        0x00010000u
#define XHCI_PS_CSC        0x00020000u
#define XHCI_PS_PEC        0x00040000u
#define XHCI_PS_WRC        0x00080000u
#define XHCI_PS_OCC        0x00100000u
#define XHCI_PS_PRC        0x00200000u
#define XHCI_PS_PLC        0x00400000u
#define XHCI_PS_CEC        0x00800000u
#define XHCI_PS_WPR        0x80000000u
#define XHCI_PS_CLEAR      0x80ff01ffu  /* биты изменения (W1C) */

/* Кодировки скорости в PORTSC (совпадают со значением Slot Context). */
#define XHCI_SPEED_FULL  1
#define XHCI_SPEED_LOW   2
#define XHCI_SPEED_HIGH  3
#define XHCI_SPEED_SUPER 4

/* Runtime-блок: регистры interrupter 0 (смещения от RTSOFF). */
#define XR_IMAN    0x20
#define XR_IMOD    0x24
#define XR_ERSTSZ  0x28
#define XR_ERSTBA  0x30
#define XR_ERDP    0x38
#define XHCI_IMAN_INTR_PEND 0x00000001u
#define XHCI_IMAN_INTR_ENA  0x00000002u

/* TRB -- Transfer Request Block (16 байт, выравнивание 16). */
typedef struct {
    volatile u64int param;    /* параметр (указатель/данные) */
    volatile u32int status;   /* длина + код завершения */
    volatile u32int flags;    /* тип + управляющие биты + cycle */
} __attribute__((packed, aligned(16))) xhci_trb_t;

/* Биты flags (по OpenBSD). */
#define XHCI_TRB_CYCLE  (1u << 0)
#define XHCI_TRB_ENT    (1u << 1)
#define XHCI_TRB_ISP    (1u << 2)
#define XHCI_TRB_CHAIN  (1u << 4)
#define XHCI_TRB_IOC    (1u << 5)
#define XHCI_TRB_IDT    (1u << 6)
#define XHCI_TRB_BSR    (1u << 9)
#define XHCI_TRB_DIR_IN (1u << 16)
#define XHCI_TRB_TRT_OUT (2u << 16)
#define XHCI_TRB_TRT_IN  (3u << 16)

#define XHCI_TRB_TYPE(x)   (((x) >> 10) & 0x3fu)
#define XHCI_TRB_SET_TYPE(t) ((t) << 10)
#define XHCI_TRB_LEN(x)    ((x) & 0x1ffff)
#define XHCI_TRB_REMAIN(x) ((x) & 0xffffff)
#define XHCI_TRB_GET_CODE(x) (((x) >> 24) & 0xff)
#define XHCI_TRB_SET_SLOT(x) (((x) & 0xff) << 24)
#define XHCI_TRB_GET_SLOT(x) (((x) >> 24) & 0xff)
#define XHCI_TRB_GET_EP(x)   (((x) >> 16) & 0x1f)

/* Типы TRB передачи/команд/событий. */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define CMD_ENABLE_SLOT   9
#define CMD_DISABLE_SLOT 10
#define CMD_ADDRESS_DEV  11
#define CMD_CONFIG_EP    12
#define CMD_EVAL_CTX     13
#define EVT_XFER         32
#define EVT_CMD_COMPLETE 33
#define EVT_PORT_CHANGE  34

/* Коды завершения. */
#define CODE_SUCCESS     1
#define CODE_SHORT_XFER 13
#define CODE_EV_FULL    21

/* ==================================================================
 * Конфигурация "простого" стека.
 * ================================================================== */

#define XHCI_MAX_SLOTS   16     /* с каким числом слотов мы работаем  */
#define XHCI_MAX_DEV     16     /* размер таблицы устройств           */
#define XHCI_RING_N      64     /* TRB в кольце (>= 16 по спецификации) */
#define XHCI_MAX_PORTS   32

/* ==================================================================
 * Кольца и устройства.
 * ================================================================== */

typedef struct {
    u64int  phys;               /* физический адрес массива TRB (страница) */
    volatile xhci_trb_t *trb;   /* виртуальный адрес (HHDM)                */
    int     enq;                /* индекс следующего TRB для записи        */
    u32int  cycle;              /* текущий cycle bit производителя         */

    /*
     * Очередь событий передачи, пришедших от контроллера
     * (Transfer Event: TRB, код, остаток).  Небольшая, т.к. у
     * control-IN за одну передачу приходят два события: Data и
     * Status, и их нельзя терять между опросами.
     */
    struct {
        volatile u64int trb_phys;
        volatile u32int code;
        volatile u32int remain;
    } ev_q[8];
    volatile int ev_head, ev_tail;

    int     active;             /* ring_init() выполнен, события ждём     */
    int     pending;            /* число отправленных TRB без события     */
    u64int  last_trb_phys;      /* последний поставленный Normal TRB      */

    /* Буфер данных (физически непрерывный, для DMA):                */
    u64int  bounce_phys;
    volatile u8int *bounce;

    int     configured;         /* EP добавлен в device context            */
} xhci_ring_t;

typedef struct {
    int         used;
    u8int       addr;           /* USB-адрес, под которым нас знает usb_core */
    u8int       slot;           /* слот, выданный контроллером               */
    u8int       port;           /* номер корневого порта (с 1)               */
    u8int       speed;          /* XHCI_SPEED_*                              */
    u64int      dc_phys;        /* страница output device context            */
    u64int      ictx_phys;      /* страница input context                    */
    xhci_ring_t ep0;            /* кольцо управления (EP0, dci 1)            */
    xhci_ring_t ep[XHCI_RING_N / 2];  /* dci 2..33 -> ep[dci-2]              */
    u16int      ep0_mps;        /* текущий MPS EP0                           */
} xhci_dev_t;

static xhci_dev_t xhci_devs[XHCI_MAX_DEV];
static xhci_dev_t *cur_enum_dev;    /* устройство в фазе адреса 0        */
static u16int      pending_mps;     /* MPS, ожидает применения (addr 0)  */

/* ==================================================================
 * Состояние контроллера.
 * ================================================================== */

static volatile u8int *mmio;        /* виртуальный адрес BAR0            */
static u32int op_base;              /* CAPLENGTH                         */
static u32int db_base;              /* DBOFF                             */
static u32int rt_base;              /* RTSOFF                            */
static int    max_ports;
static int    max_slots;            /* MaxSlotsEn, которое мы записали   */
static int    ctx_size;             /* 32 или 64 (HCCPARAMS1.CSZ)        */

static u64int dcbaa_phys;           /* Device Context Base Address Array */
static volatile u64int *dcbaa;

static xhci_ring_t cmd_ring;
static xhci_ring_t evt_ring;
static u64int       erst_phys;      /* страница Segment Table (ERST)     */
static int          evt_enq;        /* индекс следующего события         */
static u32int       evt_cycle;      /* ожидаемый cycle bit событий       */

/* Последнее событие передачи (разбирается event_drain). */
static struct {
    volatile u64int trb_phys;
    volatile u32int code;
    volatile u32int remain;
    volatile int    fresh;
} last_xfer_evt;

/* Код последнего Transfer Event (для диагностики таймаутов; 0 = не было). */
static volatile u32int last_xfer_code;

/* Последнее завершение команды. */
static struct {
    volatile int    ready;
    volatile u32int code;
    volatile u64int param;
    volatile u32int control;    /* control-dword события: биты 31:24 = Slot ID */
} last_cmd_evt;

/* Страница-буфер для control-передач (всегда синхронные). */
static u64int       ctl_bounce_phys;
static volatile u8int *ctl_bounce;

/* Диапазоны портов USB3 (из xECP id 2); если нет -- все порты USB2. */
static int usb3_port_base, usb3_port_count;

static int controller_ok = 0;

/* ==================================================================
 * Отладочный вывод в COM1 (как в uhci.c/usb_core.c).
 * ================================================================== */

static void dbg_putc(char c) { outb(0x3F8, (u8int)c); }
static void dbg_puts(const char *s) { while (*s) dbg_putc(*s++); }
static void dbg_hex32(u32int v)
{
    const char *hexd = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) dbg_putc(hexd[(v >> i) & 0xF]);
}
static void dbg_hex64(u64int v)
{
    dbg_hex32((u32int)(v >> 32));
    dbg_hex32((u32int)v);
}
static void dbg_dec(int v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { dbg_putc('0'); return; }
    while (v > 0) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) dbg_putc(buf[i]);
}

/* ==================================================================
 * Вспомогательные функции: физ<->вирт, задержки, барьеры, MMIO.
 * ================================================================== */

/* Виртуальный адрес по физическому (физ -> kernel HHDM). */
static void *p2v(u64int phys)
{
    return (void *)(uintptr_t)(phys + limine_hhdm_offset);
}

static void short_delay(u32int loops)
{
    while (loops--) __asm__ volatile("pause");
}

/*
 * Задержка ~ms из busy-циклов.  Калибровка та же, что в uhci.c:
 * 100000 итераций pause ~= 1 мс (uhci.c: 5,000,000 ~= 50 мс).
 */
static void delay_ms(u32int ms)
{
    while (ms--) short_delay(100000);
}

/* Полный барьер памяти: гарантирует порядок "записал TRB -> позвонил". */
static inline void xhci_barrier(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/* Чтение/запись 32-битных регистров от начала BAR0. */
static inline u32int xr32(u32int off)
{
    return *(volatile u32int *)(mmio + off);
}
static inline void xr32w(u32int off, u32int v)
{
    *(volatile u32int *)(mmio + off) = v;
}

static inline u32int op_r(u32int off)      { return xr32(op_base + off); }
static inline void   op_w(u32int off, u32int v) { xr32w(op_base + off, v); }
static inline u32int rt_r(u32int off)      { return xr32(rt_base + off); }
static inline void   rt_w(u32int off, u32int v) { xr32w(rt_base + off, v); }

/*
 * Doorbell: слот 0 -- кольцо команд, 1..N -- слоты устройств.
 *
 * ВАЖНО: Doorbell-регистры идут с шагом 4 байта (XHCI_DB(n) = DBOFF +
 * 4*n, как в OpenBSD xhcireg.h).  Значение регистра: цель (номер EP,
 * 1..31) в битах 15:0, Stream ID -- в битах 31:16 (XHCI_DB_SET_SID).
 * Раньше шаг был slot*8: звонок для слота 1 попадал в Doorbell N2
 * (слот 2), контроллер молча игнорировал его -- Transfer Event не
 * приходил вообще ("control transfer timeout"), тогда как команды
 * (слот 0, смещение 0) продолжали работать и маскировали баг.
 */
static void xhci_doorbell(int slot, int endpoint)
{
    xr32w(db_base + (u32int)(slot * 4), (u32int)endpoint);
    xhci_barrier();
}

/* ==================================================================
 * Аллокация страниц из PMM (физически непрерывные, 4KiB-выровненные
 * -- этого достаточно для всех выравниваний xHCI: 64/16/1).
 * ================================================================== */

static u64int dma_page(void)
{
    u64int phys = pmm_alloc_page();
    if (phys == 0)
        return 0;
    u8int *v = (u8int *)p2v(phys);
    memset(v, 0, 4096);
    return phys;
}

/* ==================================================================
 * Кольца TRB.
 * ================================================================== */

static int ring_init(xhci_ring_t *r)
{
    u64int phys = dma_page();
    if (phys == 0)
        return -1;

    r->phys = phys;
    r->trb  = (volatile xhci_trb_t *)p2v(phys);
    r->enq  = 0;
    r->cycle = 1;               /* RCS = 1 при старте                 */
    for (int i = 0; i < 8; i++) {
        r->ev_q[i].trb_phys = 0;
        r->ev_q[i].code = 0;
        r->ev_q[i].remain = 0;
    }
    r->ev_head = 0;
    r->ev_tail = 0;
    r->active = 1;
    r->pending = 0;
    r->last_trb_phys = 0;
    r->bounce_phys = 0;
    r->bounce = 0;
    r->configured = 0;

    /*
     * Последний TRB кольца -- Link TRB с Toggle Cycle: он перенаправляет
     * потребителя на начало кольца и переворачивает ожидаемый cycle bit.
     * Записывается один раз; cycle-бит в нём обновляется при каждом
     * обходе (см. ring_put).
     */
    r->trb[XHCI_RING_N - 1].param = r->phys;
    r->trb[XHCI_RING_N - 1].status = 0;
    r->trb[XHCI_RING_N - 1].flags =
        XHCI_TRB_SET_TYPE(TRB_LINK) | XHCI_TRB_CYCLE | (1u << 1) /* TC */;

    return 0;
}

/* Записать TRB в кольцо; вернёт физический адрес записанного TRB. */
static u64int ring_put(xhci_ring_t *r, u64int param, u32int status,
                       u32int flags)
{
    volatile xhci_trb_t *t = &r->trb[r->enq];
    u64int tphys = r->phys + (u64int)r->enq * sizeof(xhci_trb_t);

    if (r->enq == XHCI_RING_N - 1) {
        /*
         * Слот занят Link TRB'ом: обновляем его cycle до текущего
         * и переворачиваем наш cycle bit -- начался новый обход.
         */
        t->flags = XHCI_TRB_SET_TYPE(TRB_LINK) | r->cycle | (1u << 1);
        xhci_barrier();
        r->enq = 0;
        r->cycle ^= 1u;
        t = &r->trb[0];
        tphys = r->phys;
    }

    t->param = param;
    t->status = status;
    t->flags = flags | r->cycle;
    xhci_barrier();

    r->enq++;
    return tphys;
}

/* ==================================================================
 * Разбор Event Ring (polling -- вместо прерываний OpenBSD).
 * ================================================================== */

static void xhci_push_event(void);      /* тело -- ниже */

static void xhci_event_drain(void)
{
    int processed = 0;

    for (;;) {
        volatile xhci_trb_t *e = &evt_ring.trb[evt_enq];
        u32int flags = e->flags;

        if ((flags & XHCI_TRB_CYCLE) != evt_cycle)
            break;                      /* пустых событий больше нет */

        u32int type = XHCI_TRB_TYPE(flags);
        u64int param = e->param;
        u32int status = e->status;

        if (type == EVT_XFER) {
            last_xfer_evt.trb_phys = param;
            last_xfer_evt.code = XHCI_TRB_GET_CODE(status);
            last_xfer_evt.remain = XHCI_TRB_REMAIN(status);
            last_xfer_code = last_xfer_evt.code;
            last_xfer_evt.fresh = 1;
            xhci_push_event();       /* разложить по очередям колец */
        } else if (type == EVT_CMD_COMPLETE) {
            last_cmd_evt.code = XHCI_TRB_GET_CODE(status);
            last_cmd_evt.param = param;
            last_cmd_evt.control = flags;   /* Slot ID в битах 31:24 */
            last_cmd_evt.ready = 1;
        } else if (type == EVT_PORT_CHANGE) {
            /*
             * Событие смены состояния порта: в polling-режиме мы всё
             * равно читаем PORTSC напрямую, поэтому просто гасим
             * биты изменения (W1C), чтобы событие повторилось.
             */
            int port = (int)((param >> 24) & 0xff);
            if (port >= 1 && port <= max_ports) {
                u32int v = op_r(XOP_PORTSC(port));
                op_w(XOP_PORTSC(port), v | XHCI_PS_CLEAR);
            }
        }
        /* остальные типы событий молча пропускаем */

        evt_enq++;
        if (evt_enq == XHCI_RING_N) {
            evt_enq = 0;
            evt_cycle ^= 1u;
        }
        processed++;
    }

    if (processed) {
        /*
         * Продвинуть ERDP на следующую (ещё не разобранную) запись.
         * Запись ERDP с битом EHB (bit 3) сбрасывает "Event Handler
         * Busy" -- в polling-режиме это только гигиена.
         */
        u64int erdp = evt_ring.phys + (u64int)evt_enq * sizeof(xhci_trb_t);
        rt_w(XR_ERDP, (u32int)erdp | 0x8u);
        rt_w(XR_ERDP + 4, (u32int)(erdp >> 32));
        xhci_barrier();
    }
}

/*
 * Положить событие передачи в очередь того кольца, которому
 * принадлежит завершившийся TRB (поиск по диапазону адресов).
 */
static void xhci_push_event(void)
{
    if (!last_xfer_evt.fresh)
        return;

    last_xfer_evt.fresh = 0;

    u64int p = last_xfer_evt.trb_phys;

    for (int i = 0; i < XHCI_MAX_DEV; i++) {
        xhci_dev_t *d = &xhci_devs[i];
        if (!d->used)
            continue;

        /* Кандидаты: EP0 + все не-EP0 кольца устройства. */
        for (int c = 0; c < 1 + (int)(sizeof(d->ep) / sizeof(d->ep[0])); c++) {
            xhci_ring_t *r = (c == 0) ? &d->ep0 : &d->ep[c - 1];

            if (!r->active)
                continue;
            if (p < r->phys || p >= r->phys + XHCI_RING_N * sizeof(xhci_trb_t))
                continue;

            int next = (r->ev_tail + 1) & 7;
            if (next == r->ev_head) {
                dbg_puts("XHCI: event queue overflow, event dropped\n");
                return;
            }
            r->ev_q[r->ev_tail].trb_phys = p;
            r->ev_q[r->ev_tail].code = last_xfer_evt.code;
            r->ev_q[r->ev_tail].remain = last_xfer_evt.remain;
            r->ev_tail = next;
            return;
        }
    }
    /* События про неизвестные TRB -- норма (например, отменённые). */
}

/* ==================================================================
 * Кольцо команд: отправка команды и ожидание Command Complete.
 * ================================================================== */

static u32int xhci_cmd(u32int type, u64int param, u32int flags)
{
    last_cmd_evt.ready = 0;

    ring_put(&cmd_ring, param, 0,
             XHCI_TRB_SET_TYPE(type) | flags);
    xhci_doorbell(0, 0);

    for (int t = 0; t < 500; t++) {         /* ~500 мс busy-ожидание */
        xhci_event_drain();
        if (last_cmd_evt.ready)
            return last_cmd_evt.code;
        delay_ms(1);
    }

    dbg_puts("XHCI: command timeout (type=");
    dbg_dec(type);
    dbg_puts(")\n");
    return 0;                               /* 0 = нет ответа */
}

/* ==================================================================
 * Контексты устройств (вдохновлено xhci_context_setup() OpenBSD).
 *
 * Input Context -- страница: Input Control Context, Slot Context и
 * 32 Endpoint Context'ов, по ctx_size (32 или 64 байта) каждый.
 * Output Device Context -- отдельная страница на устройство.
 * ================================================================== */

/* Указатель на контекст номер idx внутри input/output страницы. */
static volatile u8int *ictx_ctx(u64int page_phys, int idx)
{
    /* idx 0 -- Input Control Context, 1 -- Slot, 2..33 -- EP dci 1..32 */
    return (volatile u8int *)p2v(page_phys) + (u64int)idx * ctx_size;
}

static void ctx_w32(volatile u8int *ctx, int off, u32int v)
{
    *(volatile u32int *)(ctx + off) = v;
}

/* Контекст конечной точки: EP type, MPS, кольцо, avg TRB len, interval. */
static void ctx_fill_ep(volatile u8int *ctx, int ep_type, u16int mps,
                        u64int ring_phys, u16int avg_trb_len, u8int interval)
{
    for (int i = 0; i < ctx_size; i++)
        ctx[i] = 0;

    ctx_w32(ctx, 0, (u32int)interval << 16);/* Interval (2^(n-1)*125мкс для HS/SS) */
    /* CERR=3, EPTYPE, MPS -- как xhci_context_setup() OpenBSD */
    ctx_w32(ctx, 4, (3u << 1) | ((u32int)ep_type << 3) |
                     ((u32int)mps << 16));
    *(volatile u64int *)(ctx + 8) = ring_phys | 1u;   /* DCS = 1 */
    ctx_w32(ctx, 16, avg_trb_len);          /* MAX_ESIT = 0 */
}

/* Контекст слота: скорость, корневой порт, число контекстов. */
static void ctx_fill_slot(volatile u8int *ctx, xhci_dev_t *d, int ctx_entries)
{
    for (int i = 0; i < ctx_size; i++)
        ctx[i] = 0;

    /* route = 0 (корневой порт), speed, ctx_entries -- как OpenBSD */
    ctx_w32(ctx, 0, ((u32int)d->speed << 20) |
                     ((u32int)ctx_entries << 27));
    ctx_w32(ctx, 4, ((u32int)d->port << 16));  /* RHPORT */
    ctx_w32(ctx, 8, 0);                        /* TT */
    ctx_w32(ctx, 12, 0);                       /* state 0 */
}

/*
 * Address Device: перевод устройства в DEFAULT (bsr=1) или ADDRESSED
 * (bsr=0) -- аналог xhci_cmd_set_address() OpenBSD.  mps -- MPS EP0.
 */
static int xhci_address_device(xhci_dev_t *d, int bsr, u16int mps)
{
    volatile u8int *icc = ictx_ctx(d->ictx_phys, 0);
    volatile u8int *slotc = ictx_ctx(d->ictx_phys, 1);
    volatile u8int *ep0c = ictx_ctx(d->ictx_phys, 2);

    /* Add flags: слот (dci 0) + EP0 (dci 1); drop = 0. */
    ctx_w32(icc, 0, 0);
    ctx_w32(icc, 4, (1u << 0) | (1u << 1));

    ctx_fill_slot(slotc, d, 1);

    /*
     * Начальный MPS EP0: Low/Full = 8 (bMaxPacketSize0 ещё неизвестен),
     * High = 64, Super = 512 -- фиксированы спецификацией USB.
     */
    d->ep0_mps = mps;
    ctx_fill_ep(ep0c, 4 /* Control: 1=Isoch OUT, 2=Bulk OUT, 3=Int OUT,
                           4=Control, 5=Isoch IN, 6=Bulk IN, 7=Int IN */,
                mps, d->ep0.phys, 8, 0);

    u32int code = xhci_cmd(CMD_ADDRESS_DEV, d->ictx_phys,
                           XHCI_TRB_SET_SLOT(d->slot) |
                           (bsr ? XHCI_TRB_BSR : 0));
    if (code != CODE_SUCCESS && code != CODE_SHORT_XFER) {
        dbg_puts("XHCI: Address Device failed (slot ");
        dbg_dec(d->slot);
        dbg_puts(" code ");
        dbg_dec(code);
        dbg_puts(")\n");
        return -1;
    }
    return 0;
}

/* Configure Endpoint: добавить конечную точку dci в device context. */
static int xhci_configure_ep(xhci_dev_t *d, int dci, int ep_type, u16int mps,
                             xhci_ring_t *ring)
{
    volatile u8int *icc = ictx_ctx(d->ictx_phys, 0);
    volatile u8int *slotc = ictx_ctx(d->ictx_phys, 1);
    volatile u8int *epc = ictx_ctx(d->ictx_phys, dci + 1);

    /* Drop = 0; add: слот + новая EP (как xhci_context_setup). */
    ctx_w32(icc, 0, 0);
    ctx_w32(icc, 4, (1u << 0) | (1u << dci));

    ctx_fill_slot(slotc, d, dci);           /* ctx_entries = dci */
    ctx_fill_ep(epc, ep_type, mps, ring->phys, 8, 4);

    u32int code = xhci_cmd(CMD_CONFIG_EP, d->ictx_phys,
                           XHCI_TRB_SET_SLOT(d->slot));
    if (code != CODE_SUCCESS && code != CODE_SHORT_XFER) {
        dbg_puts("XHCI: Configure Endpoint failed (dci ");
        dbg_dec(dci);
        dbg_puts(" code ");
        dbg_dec(code);
        dbg_puts(")\n");
        return -1;
    }
    ring->configured = 1;
    return 0;
}

/* Выдать слот и подготовить структуры устройства. */
static xhci_dev_t *xhci_enable_slot(u8int port, u8int speed)
{
    u32int code = xhci_cmd(CMD_ENABLE_SLOT, 0, 0);
    if (code != CODE_SUCCESS) {
        dbg_puts("XHCI: Enable Slot failed (code ");
        dbg_dec(code);
        dbg_puts(")\n");
        return 0;
    }

    /*
     * Назначенный Slot ID лежит в поле Slot ID события Command
     * Completion -- старший байт control-dword'а (биты 31:24):
     * так читают OpenBSD (XHCI_TRB_GET_SLOT) и Linux, туда же
     * пишет QEMU ((slotid << 24) в xhci_write_event).  Спецификация
     * 6.4.2.2 допускает дубль в Completion Parameter -- берём
     * его как запасной вариант.
     */
    int slot = (int)((last_cmd_evt.control >> 24) & 0xff);
    if (slot == 0)
        slot = (int)(last_cmd_evt.param & 0xff);
    if (slot < 1 || slot >= XHCI_MAX_SLOTS) {
        dbg_puts("XHCI: bad slot id\n");
        return 0;
    }

    for (int i = 0; i < XHCI_MAX_DEV; i++) {
        xhci_dev_t *d = &xhci_devs[i];
        if (d->used)
            continue;

        d->used  = 1;
        d->addr  = 0;
        d->slot  = (u8int)slot;
        d->port  = port;
        d->speed = speed;

        d->dc_phys = dma_page();            /* output device context */
        d->ictx_phys = dma_page();          /* input context         */
        if (d->dc_phys == 0 || d->ictx_phys == 0) {
            d->used = 0;
            return 0;
        }
        dcbaa[slot] = d->dc_phys;           /* контроллер читает сам */
        xhci_barrier();

        if (ring_init(&d->ep0) != 0) {
            d->used = 0;
            return 0;
        }
        for (int k = 0; k < (int)(sizeof(d->ep) / sizeof(d->ep[0])); k++) {
            d->ep[k].active = 0;
            d->ep[k].configured = 0;
            d->ep[k].pending = 0;
            d->ep[k].ev_head = 0;
            d->ep[k].ev_tail = 0;
        }
        return d;
    }

    dbg_puts("XHCI: no free device slots in stack\n");
    return 0;
}

static void xhci_disable_slot(xhci_dev_t *d)
{
    if (d->slot)
        xhci_cmd(CMD_DISABLE_SLOT, 0, XHCI_TRB_SET_SLOT(d->slot));
    dcbaa[d->slot] = 0;
    d->used = 0;
    d->slot = 0;
    d->addr = 0;
}

/* Найти устройство по USB-адресу (addr != 0). */
static xhci_dev_t *xhci_find_addr(u8int addr)
{
    for (int i = 0; i < XHCI_MAX_DEV; i++)
        if (xhci_devs[i].used && xhci_devs[i].addr == addr)
            return &xhci_devs[i];
    return 0;
}

/* ==================================================================
 * Инициализация контроллера (последовательности xhci_init()/
 * xhci_reset()/xhci_config() OpenBSD).
 * ================================================================== */

static int xhci_hc_reset(void)
{
    /* Остановить контроллер и дождаться HCH (xhci_reset). */
    op_w(XOP_USBCMD, 0);
    int hch = 0;
    for (int i = 0; i < 100; i++) {
        delay_ms(1);
        hch = op_r(XOP_USBSTS) & XHCI_STS_HCH;
        if (hch)
            break;
    }
    if (!hch)
        dbg_puts("XHCI: halt timeout\n");

    /* HCRST: ждать, пока снят бит HCRST и флаг CNR. */
    op_w(XOP_USBCMD, XHCI_CMD_HCRST);
    u32int bad = 1;
    for (int i = 0; i < 100; i++) {
        delay_ms(1);
        bad = (op_r(XOP_USBCMD) & XHCI_CMD_HCRST) |
              (op_r(XOP_USBSTS) & XHCI_STS_CNR);
        if (!bad)
            break;
    }
    if (bad) {
        dbg_puts("XHCI: reset timeout\n");
        return -1;
    }
    return 0;
}

/*
 * Разбор Extended Capabilities (id 2 = Supported Protocol): какие
 * порты говорят на USB2, какие на USB3 (как в OpenBSD -- по ним
 * выбирается тип сброса порта).
 */
static void xhci_parse_xecp(void)
{
    usb3_port_base = 0;
    usb3_port_count = 0;

    u32int off = XHCI_HCC_XECP(xr32(XC_HCCPARAMS1));
    int guard = 0;

    while (off && guard++ < 32) {
        u32int dword0 = xr32(off * 4);
        u32int id = dword0 & 0xff;

        if (id == 2) {
            u32int dword1 = xr32(off * 4 + 4);
            u32int rev_major = (dword1 >> 16) & 0xff;
            u32int dword3 = xr32(off * 4 + 12);
            int base = dword3 & 0xff;               /* Port Offset (с 1) */
            int count = (dword3 >> 8) & 0xff;       /* Port Count        */

            if (rev_major == 3) {
                usb3_port_base = base;
                usb3_port_count = count;
            }
        }

        off = (dword0 >> 8) & 0xff;
    }
}

static int xhci_is_usb3_port(int port)   /* port с 1 */
{
    return usb3_port_count > 0 &&
           port >= usb3_port_base &&
           port < usb3_port_base + usb3_port_count;
}

int xhci_init(void)
{
    u8int bus, slot, func;

    if (!pci_find_class(0x0C, 0x03, 0x30, &bus, &slot, &func)) {
        dbg_puts("XHCI: controller not found on PCI bus "
                 "(QEMU hint: add -device qemu-xhci -device usb-mouse)\n");
        return 0;
    }
    dbg_puts("XHCI: controller at ");
    dbg_dec(bus); dbg_putc(':'); dbg_dec(slot); dbg_putc('.'); dbg_dec(func);
    dbg_puts("\n");
    /*
     * Метка сборки: если в логе её нет, работает старый бинарник.
     * (После fix3 лог "control transfer timeout" без диагностических
     * полей -- верный признак, что патч не был применён/пересобран.)
     */
    dbg_puts("XHCI: driver build v4 (doorbell stride 4)\n");

    /* Memory Space + Bus Master (как uhci.c включает I/O + master). */
    pci_enable_busmaster_mem(bus, slot, func);

    /* BAR0: xHCI всегда memory BAR, часто 64-битный. */
    u32int bar0 = pci_read32(bus, slot, func, 0x10);
    u64int bar_phys = bar0 & ~0xFu;
    if ((bar0 & 0x6) == 0x4)
        bar_phys |= (u64int)pci_read32(bus, slot, func, 0x14) << 32;
    if ((bar0 & 0x6) != 0 && (bar0 & 0x6) != 0x4) {
        dbg_puts("XHCI: unexpected BAR type\n");
        return 0;
    }

    mmio = (volatile u8int *)p2v(bar_phys);

    dbg_puts("XHCI: BAR0 phys=0x");
    dbg_hex64(bar_phys);
    dbg_puts("\n");

    /* Смещения блоков регистров (xhci_init OpenBSD). */
    op_base = xr32(XC_CAPLENGTH) & 0xFF;
    db_base = xr32(XC_DBOFF) & ~0x3u;
    rt_base = xr32(XC_RTSOFF) & ~0xFu;
    /*
     * HCIVERSION (16 бит, BCD) -- биты 31:16 выровненного dword по
     * смещению 0x00 (рядом с CAPLENGTH).  xr32(0x02) -- невыровненное
     * чтение, которое QEMU/железо отвечают непредсказуемо.
     */
    u32int version = (xr32(XC_CAPLENGTH) >> 16) & 0xFFFFu;
    dbg_puts("XHCI: version ");
    dbg_dec(version >> 8); dbg_putc('.'); dbg_dec(version & 0xFF);
    dbg_puts(" CAPLENGTH=0x"); dbg_hex32(op_base);
    dbg_puts(" DBOFF=0x"); dbg_hex32(db_base);
    dbg_puts(" RTSOFF=0x"); dbg_hex32(rt_base);
    dbg_puts("\n");

    /* Сброс контроллера (xhci_reset). */
    if (xhci_hc_reset() != 0)
        return 0;

    /* Параметры (xhci_init). */
    u32int hcs1 = xr32(XC_HCSPARAMS1);
    u32int hcs2 = xr32(XC_HCSPARAMS2);
    u32int hcc1 = xr32(XC_HCCPARAMS1);

    max_ports = XHCI_HCS1_N_PORTS(hcs1);
    if (max_ports > XHCI_MAX_PORTS)
        max_ports = XHCI_MAX_PORTS;

    int devslots = XHCI_HCS1_DEVSLOT_MAX(hcs1);
    if (devslots > XHCI_MAX_SLOTS)
        devslots = XHCI_MAX_SLOTS;
    max_slots = devslots;

    ctx_size = XHCI_HCC_CSZ(hcc1) ? 64 : 32;
    int spb = (int)XHCI_HCS2_SPB_MAX(hcs2);

    dbg_puts("XHCI: ports=");
    dbg_dec(max_ports);
    dbg_puts(" slots=");
    dbg_dec(devslots);
    dbg_puts(" ctxsize=");
    dbg_dec(ctx_size);
    dbg_puts(" scratch=");
    dbg_dec(spb);
    dbg_puts("\n");

    xhci_parse_xecp();
    if (usb3_port_count) {
        dbg_puts("XHCI: USB3 ports ");
        dbg_dec(usb3_port_base);
        dbg_puts("..");
        dbg_dec(usb3_port_base + usb3_port_count - 1);
        dbg_puts("\n");
    }

    /* DCBAA (sc_dcbaa). */
    dcbaa_phys = dma_page();
    if (dcbaa_phys == 0) return 0;
    dcbaa = (volatile u64int *)p2v(dcbaa_phys);

    /* Кольцо команд, кольцо событий, ERST. */
    if (ring_init(&cmd_ring) != 0) return 0;
    if (ring_init(&evt_ring) != 0) return 0;

    erst_phys = dma_page();
    if (erst_phys == 0) return 0;
    volatile u64int *erst = (volatile u64int *)p2v(erst_phys);
    erst[0] = evt_ring.phys;                    /* адрес сегмента */
    erst[1] = XHCI_RING_N;                      /* число записей  */
    erst[2] = 0;
    xhci_barrier();

    evt_enq = 0;
    evt_cycle = 1;

    /* Scratchpad-буферы, если контроллер их требует (OpenBSD
       xhci_scratchpad_alloc; у QEMU их 0). */
    if (spb > 0) {
        if (spb > 16) spb = 16;
        u64int arr_phys = dma_page();
        if (arr_phys == 0) return 0;
        volatile u64int *arr = (volatile u64int *)p2v(arr_phys);
        int ok = 1;
        for (int i = 0; i < spb; i++) {
            u64int p = dma_page();
            if (p == 0) { ok = 0; break; }
            arr[i] = p;
        }
        if (!ok) return 0;
        dcbaa[0] = arr_phys;
        xhci_barrier();
    }

    /* Очистить событие/команду и выйти на чистый polling. */
    last_cmd_evt.ready = 0;
    last_xfer_evt.fresh = 0;
    for (int i = 0; i < XHCI_MAX_DEV; i++)
        xhci_devs[i].used = 0;
    cur_enum_dev = 0;
    pending_mps = 0;

    ctl_bounce_phys = dma_page();
    if (ctl_bounce_phys == 0) return 0;
    ctl_bounce = (volatile u8int *)p2v(ctl_bounce_phys);

    /*
     * Программирование контроллера (xhci_config).  Прерывания не
     * включаем: USBCMD = RS, INTE = 0, IMAN.IE = 0 -- всё polling.
     */
    op_w(XOP_CONFIG, (op_r(XOP_CONFIG) & ~0xFFu) | (u32int)max_slots);

    op_w(XOP_DCBAAP, (u32int)dcbaa_phys);
    op_w(XOP_DCBAAP + 4, (u32int)(dcbaa_phys >> 32));

    op_w(XOP_CRCR, (u32int)cmd_ring.phys | 0x1u /* RCS */);
    op_w(XOP_CRCR + 4, (u32int)(cmd_ring.phys >> 32));

    rt_w(XR_ERSTSZ, 1);
    rt_w(XR_ERSTBA, (u32int)erst_phys);
    rt_w(XR_ERSTBA + 4, (u32int)(erst_phys >> 32));
    rt_w(XR_ERDP, (u32int)evt_ring.phys | 0x8u);
    rt_w(XR_ERDP + 4, (u32int)(evt_ring.phys >> 32));

    rt_w(XR_IMAN, XHCI_IMAN_INTR_PEND);         /* сбросить IP, IE=0 */
    rt_w(XR_IMOD, 0);

    op_w(XOP_USBCMD, XHCI_CMD_RS);              /* Run, без INTE */
    delay_ms(10);

    if (op_r(XOP_USBSTS) & (XHCI_STS_HCE | XHCI_STS_HCH)) {
        dbg_puts("XHCI: controller halted/error after start\n");
        return 0;
    }

    controller_ok = 1;
    dbg_puts("XHCI: controller started\n");
    return 1;
}

/* ==================================================================
 * Корневые порты.
 * ================================================================== */

/* Начальный MPS EP0 по скорости порта. */
static u16int xhci_initial_mps(u8int speed)
{
    if (speed == XHCI_SPEED_HIGH)
        return 64;
    if (speed == XHCI_SPEED_SUPER)
        return 512;
    return 8;                               /* low / full */
}

int xhci_port_count(void)
{
    return controller_ok ? max_ports : 0;
}

int xhci_port_connected(int port)
{
    if (!controller_ok || port < 0 || port >= max_ports)
        return 0;
    /* Порты PORTSC нумеруются с 1, наружу -- с 0, как в uhci.c. */
    return (op_r(XOP_PORTSC(port + 1)) & XHCI_PS_CCS) != 0;
}

int xhci_port_reset(int port)
{
    if (!controller_ok || port < 0 || port >= max_ports)
        return 0;

    int p = port + 1;
    u32int v = op_r(XOP_PORTSC(p));

    if (!(v & XHCI_PS_CCS))
        return 0;

    /*
     * Включить питание порта, если порт ещё не запитан
     * (контроллеры с PPC не подают питание сами).
     */
    if (!(v & XHCI_PS_PP))
        op_w(XOP_PORTSC(p), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PP);

    if (xhci_is_usb3_port(p)) {
        /*
         * USB3: сброс не нужен -- линк сам проходит Polling -> U0,
         * PED поднимается при переходе в U0.  Если линк завис,
         * делаем Warm Reset (аналог OpenBSD-поведения для SS).
         */
        int ok = 0;
        for (int t = 0; t < 300; t++) {
            v = op_r(XOP_PORTSC(p));
            if ((v & XHCI_PS_PED) && XHCI_PS_GET_PLS(v) == 0) {
                ok = 1;
                break;
            }
            delay_ms(1);
        }
        if (!ok) {
            v = op_r(XOP_PORTSC(p));
            op_w(XOP_PORTSC(p), (v & ~XHCI_PS_CLEAR) | XHCI_PS_WPR);
            for (int t = 0; t < 300; t++) {
                v = op_r(XOP_PORTSC(p));
                if ((v & XHCI_PS_PED) && XHCI_PS_GET_PLS(v) == 0) {
                    ok = 1;
                    break;
                }
                delay_ms(1);
            }
        }
        if (!ok)
            return 0;

        /* Погасить биты изменения (W1C). */
        v = op_r(XOP_PORTSC(p));
        op_w(XOP_PORTSC(p), v | XHCI_PS_WRC | XHCI_PS_PLC | XHCI_PS_CSC);
    } else {
        /*
         * USB2: классический сброс PR (~50-60 мс, как хаб-таймер
         * в OpenBSD), затем ждём PRC и PED.
         */
        v = op_r(XOP_PORTSC(p));
        op_w(XOP_PORTSC(p), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PR);
        delay_ms(60);

        int ok = 0;
        for (int t = 0; t < 300; t++) {
            v = op_r(XOP_PORTSC(p));
            if (v & XHCI_PS_PRC) {
                ok = 1;
                break;
            }
            delay_ms(1);
        }

        /* Снять PR (RW-бит: пишем 0) и погасить изменения. */
        v = op_r(XOP_PORTSC(p));
        op_w(XOP_PORTSC(p),
             (v & ~XHCI_PS_PR) | XHCI_PS_PRC | XHCI_PS_PEC | XHCI_PS_CSC);

        if (!ok)
            return 0;

        v = op_r(XOP_PORTSC(p));
        if (!(v & XHCI_PS_CCS))
            return 0;                       /* устройство отвалилось */
        if (!(v & XHCI_PS_PED))
            return 0;                       /* не включился */
    }

    /*
     * Скорость: для USB2 валидна после сброса, для USB3 -- после U0.
     */
    v = op_r(XOP_PORTSC(p));
    u8int speed = XHCI_PS_SPEED(v);
    if (speed == 0)
        speed = XHCI_SPEED_FULL;

    /*
     * Выдать слот и перевести устройство в DEFAULT через
     * Address Device BSR=1 (двухшаговая адресация OpenBSD:
     * Low/Full-Speed устройства не принимают SET_ADDRESS до
     * чтения дескриптора).
     */
    xhci_dev_t *d = xhci_enable_slot((u8int)p, speed);
    if (d == 0)
        return 0;

    if (xhci_address_device(d, 1, xhci_initial_mps(speed)) != 0) {
        xhci_disable_slot(d);
        return 0;
    }

    cur_enum_dev = d;
    pending_mps = 0;

    dbg_puts("XHCI: port ");
    dbg_dec(p);
    dbg_puts(" speed ");
    dbg_dec(speed);
    dbg_puts(" slot ");
    dbg_dec(d->slot);
    dbg_puts(" ready (addr 0)\n");
    return 1;
}

/* ==================================================================
 * Control-передачи (по образцу xhci_device_ctrl_start() OpenBSD:
 * Setup TRB с IDT -> Data TRB -> Status TRB с IOC).
 * ================================================================== */

int xhci_control_transfer(u8int addr, usb_setup_packet_t *setup,
                          void *buffer, u16int length, int in_dir)
{
    if (!controller_ok)
        return -1;

    xhci_dev_t *d;

    if (addr == 0) {
        d = cur_enum_dev;                   /* фаза адреса 0 */
        if (d == 0)
            return -1;

        /*
         * SET_ADDRESS перехватываем: в xHCI адрес устройству
         * назначает сам контроллер при Address Device BSR=0.
         * USB-адрес из запроса используем только как наш ключ
         * маршрутизации (слот <-> addr).
         */
        if (setup->bRequest == USB_REQ_SET_ADDRESS &&
            (setup->bmRequestType & 0x7F) == 0) {

            u16int mps = pending_mps ? pending_mps
                                     : xhci_initial_mps(d->speed);
            if (xhci_address_device(d, 0, mps) != 0)
                return -1;

            d->addr = (u8int)setup->wValue;
            cur_enum_dev = 0;
            pending_mps = 0;

            /*
             * Адрес на шине выбирает сам контроллер (спека 4.6.5):
             * сверяем с Device Address выходного Slot Context
             * (dword3, биты 7:0; у QEMU он равен slot id).  Ключ
             * стека (wValue) остаётся маршрутизационным.
             */
            volatile u8int *odc = (volatile u8int *)p2v(d->dc_phys);
            u8int wire = (u8int)(*(volatile u32int *)(odc + 12) & 0xFFu);

            dbg_puts("XHCI: slot ");
            dbg_dec(d->slot);
            dbg_puts(" now answers as addr ");
            dbg_dec(d->addr);
            if (wire != 0 && wire != d->addr) {
                dbg_puts(" (wire addr ");
                dbg_dec(wire);
                dbg_puts(")");
            }
            dbg_puts("\n");
            return 0;
        }
    } else {
        d = xhci_find_addr(addr);
        if (d == 0)
            return -1;
    }

    if (length > 4096)
        return -1;                          /* один Data TRB <= 4 KiB */

    /*
     * Bounce-буфер: TRB ссылается на физический адрес, а буферы
     * вызывающего кода лежат где угодно в higher-half.
     */
    for (int i = 0; i < 4096; i++)
        ctl_bounce[i] = 0;
    if (length && !in_dir) {
        u8int *src = (u8int *)buffer;
        for (u16int i = 0; i < length; i++)
            ctl_bounce[i] = src[i];
    }

    /*
     * Setup TRB: 8 байт пакета -- прямо в TRB (IDT), TRT задаёт
     * наличие/направление Data-этапа.
     */
    u64int setup_param = 0;
    u8int *sp = (u8int *)&setup_param;
    sp[0] = setup->bmRequestType;
    sp[1] = setup->bRequest;
    sp[2] = (u8int)(setup->wValue & 0xFF);
    sp[3] = (u8int)(setup->wValue >> 8);
    sp[4] = (u8int)(setup->wIndex & 0xFF);
    sp[5] = (u8int)(setup->wIndex >> 8);
    sp[6] = (u8int)(setup->wLength & 0xFF);
    sp[7] = (u8int)(setup->wLength >> 8);

    u32int trt = 0;
    if (length)
        trt = in_dir ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT;

    ring_put(&d->ep0, setup_param, XHCI_TRB_LEN(8),
             XHCI_TRB_SET_TYPE(TRB_SETUP) | XHCI_TRB_IDT | trt);

    /* Data TRB (если есть): для IN -- ISP+IOC, чтобы поймать short
       packet и узнать реально переданное число байт. */
    u64int data_phys = 0;
    if (length) {
        u32int dflags = XHCI_TRB_SET_TYPE(TRB_DATA);
        if (in_dir)
            dflags |= XHCI_TRB_DIR_IN | XHCI_TRB_ISP | XHCI_TRB_IOC;
        data_phys = ring_put(&d->ep0, ctl_bounce_phys, XHCI_TRB_LEN(length),
                             dflags);
    }

    /* Status TRB: IOC; направление -- обратное Data (IN для OUT). */
    u32int sflags = XHCI_TRB_SET_TYPE(TRB_STATUS) | XHCI_TRB_IOC;
    if (length == 0 || !in_dir)
        sflags |= XHCI_TRB_DIR_IN;
    u64int status_phys = ring_put(&d->ep0, 0, 0, sflags);

    d->ep0.pending++;
    xhci_doorbell(d->slot, 1);              /* EP0 == dci 1 */

    /*
     * Ожидание события от Status TRB (по дороге ловим событие от
     * Data TRB для IN -- оттуда берём актуальную длину).
     */
    int actlen = -1;
    int done = 0;
    for (int t = 0; t < 500 && !done; t++) {    /* ~500 мс */
        xhci_event_drain();

        while (d->ep0.ev_head != d->ep0.ev_tail) {
            u64int evp = d->ep0.ev_q[d->ep0.ev_head].trb_phys;
            u32int evc = d->ep0.ev_q[d->ep0.ev_head].code;
            u32int evr = d->ep0.ev_q[d->ep0.ev_head].remain;
            d->ep0.ev_head = (d->ep0.ev_head + 1) & 7;
            d->ep0.pending--;

            if (data_phys && evp == data_phys) {
                actlen = (int)length - (int)evr;
                continue;
            }
            if (evp == status_phys) {
                done = 1;
                if (evc != CODE_SUCCESS && evc != CODE_SHORT_XFER) {
                    dbg_puts("XHCI: control transfer failed (code ");
                    dbg_dec(evc);
                    dbg_puts(")\n");
                    return -1;
                }
                break;
            }
            /* Событие не от этого TD: сообщаем, чтобы заметить
               рассинхронизацию кольца/событий сразу. */
            dbg_puts("XHCI: stray transfer event (code ");
            dbg_dec(evc);
            dbg_puts(")\n");
        }

        if (!done)
            delay_ms(1);
    }

    if (!done) {
        /* Диагностика: слот, глубина очереди, код последнего события.
           last_evt=0 -- событий нет вообще (doorbell/kick не дошёл);
           иначе -- контроллер ответил ошибкой с этим кодом. */
        dbg_puts("XHCI: control transfer timeout (slot ");
        dbg_dec(d->slot);
        dbg_puts(" enq=");
        dbg_dec(d->ep0.enq);
        dbg_puts(" pending=");
        dbg_dec(d->ep0.pending);
        dbg_puts(" last_evt=");
        dbg_dec((int)last_xfer_code);
        dbg_puts(")\n");
        return -1;
    }

    if (in_dir && length) {
        int n = (actlen >= 0) ? actlen : (int)length;
        if (n > (int)length)
            n = length;
        u8int *dst = (u8int *)buffer;
        for (int i = 0; i < n; i++)
            dst[i] = ctl_bounce[i];
    }

    return 0;
}

void xhci_set_ep0_max_packet(u8int addr, u16int mps)
{
    if (!controller_ok)
        return;

    if (addr == 0) {
        /* До SET_ADDRESS -- просто запомнить, применится при BSR=0. */
        pending_mps = mps;
        return;
    }

    /*
     * После SET_ADDRESS -- перевыпустить Address Device BSR=0 с
     * настоящим bMaxPacketSize0 (Evaluate Context; так же делает
     * OpenBSD при повторном xhci_context_setup).
     */
    xhci_dev_t *d = xhci_find_addr(addr);
    if (d == 0)
        return;
    if (mps == d->ep0_mps)
        return;         /* MPS не изменился: Address Device уже применён,
                           не дёргаем устройство повторным Address Device
                           (QEMU при нём делает usb_device_reset). */
    xhci_address_device(d, 0, mps);
}

/* ==================================================================
 * Передачи данных по не-EP0 конечным точкам (interrupt IN для мыши).
 * ================================================================== */

int xhci_transfer(u8int addr, u8int endpoint, int in_dir,
                  void *buffer, u16int length, int *toggle,
                  u16int ep_max_packet)
{
    if (!controller_ok || endpoint == 0)
        return -1;

    xhci_dev_t *d = xhci_find_addr(addr);
    if (d == 0)
        return -1;

    if (length > 4096)
        length = 4096;

    /*
     * DCI (Device Context Index): EP0 = 1, EP n OUT = 2n, EP n IN = 2n+1.
     */
    int dci = endpoint * 2 + (in_dir ? 1 : 0);
    int epi = dci - 2;                      /* индекс в d->ep[] */
    if (epi < 0 || epi >= (int)(sizeof(d->ep) / sizeof(d->ep[0])))
        return -1;

    xhci_ring_t *r = &d->ep[epi];

    if (!r->active) {
        /*
         * Первое обращение к конечной точке: аллоцировать кольцо и
         * добавить её в device context (Configure Endpoint).
         *
         * Тип EP: для IN -- interrupt (boot-мышь/клавиатура), для
         * OUT -- bulk.  bInterval до драйвера не доходит, ставим 4:
         * для FS это 4 мс, для HS/SS -- 2^3 * 125 мкс = 1 мс.
         */
        if (ring_init(r) != 0)
            return -1;
        r->bounce_phys = dma_page();
        if (r->bounce_phys == 0)
            return -1;
        r->bounce = (volatile u8int *)p2v(r->bounce_phys);

        int ep_type = in_dir ? 7 /* interrupt IN */ : 2 /* bulk OUT */;
        if (xhci_configure_ep(d, dci, ep_type,
                              ep_max_packet ? ep_max_packet : 8,
                              r) != 0)
            return -1;

        dbg_puts("XHCI: slot ");
        dbg_dec(d->slot);
        dbg_puts(" ep ");
        dbg_dec(endpoint);
        dbg_puts(in_dir ? "IN configured (interrupt)\n"
                        : "OUT configured (bulk)\n");
    }

    /*
     * 1) Забрать события от предыдущих Normal TRB (мышь могла
     *    передать отчёт между нашими опросами).
     */
    xhci_event_drain();
    while (r->ev_head != r->ev_tail) {
        u32int evc = r->ev_q[r->ev_head].code;
        u32int evr = r->ev_q[r->ev_head].remain;
        r->ev_head = (r->ev_head + 1) & 7;
        r->pending--;

        if (evc != CODE_SUCCESS && evc != CODE_SHORT_XFER)
            return -1;

        if (in_dir) {
            int n = (int)length - (int)evr;
            if (n < 0) n = 0;
            if (n > (int)length) n = length;
            u8int *dst = (u8int *)buffer;
            for (int i = 0; i < n; i++)
                dst[i] = r->bounce[i];
        }
        *toggle ^= 1;                       /* HC сам ведёт toggle, это
                                               только для вызывающего */
        return 0;
    }

    /*
     * 2) Событий нет: поставить Normal TRB и подождать немного.
     *    Не дождались -- вернуть 1 (NAK): TRB остаётся в кольце,
     *    отчёт заберём при следующем опросе.
     */
    if (r->pending >= XHCI_RING_N - 4)
        return 1;                           /* кольцо почти заполнено */

    if (!in_dir) {
        u8int *src = (u8int *)buffer;
        for (u16int i = 0; i < length; i++)
            r->bounce[i] = src[i];
    }

    u32int nflags = XHCI_TRB_SET_TYPE(TRB_NORMAL) | XHCI_TRB_IOC;
    if (in_dir)
        nflags |= XHCI_TRB_DIR_IN;

    r->last_trb_phys = ring_put(r, r->bounce_phys, XHCI_TRB_LEN(length),
                                nflags);
    r->pending++;

    xhci_doorbell(d->slot, dci);

    for (int t = 0; t < 20; t++) {          /* ~2 мс -- отзывчивость */
        xhci_event_drain();
        while (r->ev_head != r->ev_tail) {
            u32int evc = r->ev_q[r->ev_head].code;
            u32int evr = r->ev_q[r->ev_head].remain;
            r->ev_head = (r->ev_head + 1) & 7;
            r->pending--;

            if (evc != CODE_SUCCESS && evc != CODE_SHORT_XFER)
                return -1;

            if (in_dir) {
                int n = (int)length - (int)evr;
                if (n < 0) n = 0;
                if (n > (int)length) n = length;
                u8int *dst = (u8int *)buffer;
                for (int i = 0; i < n; i++)
                    dst[i] = r->bounce[i];
            }
            *toggle ^= 1;
            return 0;
        }
        short_delay(1000);
    }

    return 1;                               /* NAK / отчёт ещё не готов */
}
