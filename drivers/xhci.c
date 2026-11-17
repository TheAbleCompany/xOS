/*
 * ============================================================
 * drivers/xhci.c -- xHCI host controller driver + USB HID mouse
 * ============================================================
 *
 * Реализовано (по мотивам xHCI spec 1.2, Linux xhci-hcd, Haiku, SerenityOS):
 *   - поиск контроллера по PCI class 0C/03/30, включение bus master,
 *     определение размера BAR, собственное UC-отображение MMIO
 *     (не зависит от того, покрывает ли HHDM Limine адрес BAR);
 *   - BIOS->OS handoff (USB Legacy Support), отключение SMI;
 *   - Intel Panther/Lynx Point: переключение портов с EHCI на xHCI;
 *   - HCRST, DCBAA, scratchpad, command ring, event ring (1 сегмент);
 *   - перечисление root-портов (USB2: port reset, USB3: link training),
 *     Enable Slot / Address Device / Evaluate Context / Configure EP;
 *   - USB-хабы (USB2 и USB3, с TT-информацией для LS/FS за HS-хабом);
 *   - HID boot-protocol мышь (SET_PROTOCOL(boot) + SET_IDLE), опрос
 *     interrupt IN endpoint, передача в mouse_handle_packet();
 *   - hot-plug / hot-unplug root-портов, восстановление halted EP.
 *
 * Контексты 32/64 байта (HCCPARAMS1.CSZ) поддерживаются.
 */

#include <stdint.h>
#include "xhci.h"
#include "mouse.h"
#include "../lib/common.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

/* Поддержка USB-клавиатуры: события идут в общий клавиатурный стек через
 * keyboard_handle_scancode() (drivers/keyboard.c) -- тот же путь, что и у
 * PS/2 клавиатуры. Оттуда события попадают в очередь keyboard_pop_event()
 * и дальше в PTY/оконный сервер (kernel/ipc.c, axshell_input.c). */
extern void keyboard_handle_scancode(unsigned char keycode);

extern void boot_stage(const char *msg);

/* ------------------------------------------------------------------ */
/* Логирование                                                        */
/* ------------------------------------------------------------------ */

static int xh_trace = 0;   /* 1 = подробная трассировка событий */

/*
 * r12-hwdiag: весь лог xHCI теперь зеркалится на экран (flanterm),
 * а не только в COM1 — на реальном железе без serial-кабеля
 * диагностику USB было не увидеть. Строки буферизуются и
 * сбрасываются по '\n' (все сообщения завершаются lg("\n")).
 */

#include "../flanterm/flanterm.h"
extern struct flanterm_context *ft_ctx;

static char     sbuf[128];
static unsigned sbuf_n = 0;

static void sbuf_flush(void)
{
    sbuf[sbuf_n] = 0;
    debug_puts(sbuf);

    if (ft_ctx)
        flanterm_write(ft_ctx, sbuf, sbuf_n);

    sbuf_n = 0;
}

static void sbuf_putc(char c)
{
    if (sbuf_n >= sizeof(sbuf) - 1)
        sbuf_flush();

    sbuf[sbuf_n++] = c;
}

static void sbuf_puts(const char *s)
{
    while (*s)
        sbuf_putc(*s++);
}

static void lg(const char *s)
{
    /* Фрагмент, начинающийся с «XHCI:», открывает новую строку.
     * Если в буфере остался незакрытый хвост (сообщение было
     * прервано) — сбросить его, чтобы строки не склеивались. */
    if (sbuf_n && s[0] == 'X' && s[1] == 'H' && s[2] == 'C' && s[3] == 'H' && s[4] == 'I' && s[5] == ':')
        sbuf_flush();

    sbuf_puts(s);

    if (sbuf_n && sbuf[sbuf_n - 1] == '\n')
        sbuf_flush();
}

static void lgh(u64int v)
{
    const char *h = "0123456789ABCDEF";
    int started = 0;

    sbuf_puts("0x");

    for (int i = 60; i >= 0; i -= 4) {
        int n = (int)((v >> i) & 15);
        if (n || started || i == 0) { started = 1; sbuf_putc(h[n]); }
    }
}

static void lgd(u64int v)
{
    char tmp[24];
    int i = 0;

    if (v == 0) { sbuf_putc('0'); return; }

    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) sbuf_putc(tmp[--i]);
}

/* ------------------------------------------------------------------ */
/* Низкоуровневые помощники                                           */
/* ------------------------------------------------------------------ */

#define BARRIER() __asm__ volatile("" ::: "memory")
#define MFENCE()  __asm__ volatile("mfence" ::: "memory")

static void udelay(u32int us)
{
    /* outb на порт 0x80 занимает ~1 мкс на реальном железе и не зависит
     * от PIT/прерываний (работает даже при выключенных IRQ). */
    while (us--) outb(0x80, 0);
}

static void mdelay(u32int ms) { udelay(ms * 1000); }

static u64int irq_save(void)
{
    u64int f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static void irq_restore(u64int f)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

static void zero(void *p, u32int n)
{
    volatile u8int *b = (volatile u8int *)p;
    while (n--) *b++ = 0;
}

/* ------------------------------------------------------------------ */
/* PCI                                                                */
/* ------------------------------------------------------------------ */

static u32int pci_rd(u8int b, u8int s, u8int f, u8int off)
{
    outl(0xCF8, 0x80000000u | ((u32int)b << 16) | ((u32int)s << 11) |
                ((u32int)f << 8) | (off & 0xFC));
    return inl(0xCFC);
}

static void pci_wr(u8int b, u8int s, u8int f, u8int off, u32int v)
{
    outl(0xCF8, 0x80000000u | ((u32int)b << 16) | ((u32int)s << 11) |
                ((u32int)f << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

/* ------------------------------------------------------------------ */
/* Отображение MMIO (UC) -- общее окно ядра из mm/vmm.c                */
/* ------------------------------------------------------------------ */

static void *map_mmio(u64int phys, u64int size)
{
    return vmm_map_mmio(phys, size);
}

/* ------------------------------------------------------------------ */
/* Регистры xHCI                                                      */
/* ------------------------------------------------------------------ */

static volatile u8int *cap;     /* capability regs   */
static volatile u8int *op;      /* operational regs  */
static volatile u8int *rt;      /* runtime regs      */
static volatile u8int *dbr;     /* doorbells         */

static inline u32int rd32(volatile u8int *base, u32int off)
{ return *(volatile u32int *)(base + off); }
static inline void wr32(volatile u8int *base, u32int off, u32int v)
{ *(volatile u32int *)(base + off) = v; }

#define USBCMD   0x00
#define USBSTS   0x04
#define CRCR     0x18
#define DCBAAP   0x30
#define CONFIG   0x38
#define PORTSC(n) (0x400 + 0x10 * ((n) - 1))

#define CMD_RS    (1u << 0)
#define CMD_HCRST (1u << 1)
#define STS_HCH   (1u << 0)
#define STS_CNR   (1u << 11)

/* PORTSC */
#define PS_CCS (1u << 0)
#define PS_PED (1u << 1)
#define PS_PR  (1u << 4)
#define PS_PLS_SHIFT 5
#define PS_PP  (1u << 9)
#define PS_SPEED(v) (((v) >> 10) & 0xF)
#define PS_WPR (1u << 31)
#define PS_CHANGE_MASK ((1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | \
                        (1u << 21) | (1u << 22) | (1u << 23))
#define PS_CSC (1u << 17)
#define PS_PRC (1u << 21)
#define PS_WRC (1u << 19)
/* биты, которые можно безопасно записать обратно без побочных эффектов */
#define PS_NEUTRAL_MASK ((1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 24) | \
                         (1u << 30) | (0xFu << 5) | (3u << 14) | (1u << 9) | \
                         (7u << 25))

/* ------------------------------------------------------------------ */
/* DMA-память                                                         */
/* ------------------------------------------------------------------ */

static int ac64 = 0;

static void *dma_page(u64int *phys)
{
    u64int p = pmm_alloc_page();
    if (!p) return 0;
    if (!ac64 && p >= 0x100000000ULL) {
        pmm_free_page(p);
        lg("XHCI: DMA page above 4G but controller is 32-bit\n");
        return 0;
    }
    void *v = vmm_phys_to_virt(p);
    zero(v, 4096);
    *phys = p;
    return v;
}

/* ------------------------------------------------------------------ */
/* TRB-кольца                                                         */
/* ------------------------------------------------------------------ */

typedef struct { volatile u32int d[4]; } trb_t;

typedef struct {
    trb_t   *t;
    u64int   phys;
    u32int   enq;
    u32int   cyc;
    u32int   n;       /* число TRB, последний -- Link */
} ring_t;

#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT  10
#define TRB_ADDR_DEV      11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CTX      13
#define TRB_RESET_EP      14
#define TRB_SET_DEQ       16
#define TRB_EV_TRANSFER   32
#define TRB_EV_CMD        33
#define TRB_EV_PORT       34

static void ring_init(ring_t *r, void *mem, u64int phys, u32int n)
{
    r->t = (trb_t *)mem; r->phys = phys; r->enq = 0; r->cyc = 1; r->n = n;
    trb_t *l = &r->t[n - 1];
    l->d[0] = (u32int)phys;
    l->d[1] = (u32int)(phys >> 32);
    l->d[2] = 0;
    l->d[3] = (TRB_LINK << 10) | (1u << 1);   /* TC, cycle=0 */
}

static u64int ring_push(ring_t *r, u32int d0, u32int d1, u32int d2, u32int d3)
{
    trb_t *t = &r->t[r->enq];
    u64int p = r->phys + (u64int)r->enq * 16;
    t->d[0] = d0; t->d[1] = d1; t->d[2] = d2;
    BARRIER();
    t->d[3] = (d3 & ~1u) | r->cyc;
    r->enq++;
    if (r->enq == r->n - 1) {
        trb_t *l = &r->t[r->n - 1];
        l->d[3] = (TRB_LINK << 10) | (1u << 1) | r->cyc;
        r->cyc ^= 1;
        r->enq = 0;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Состояние контроллера                                              */
/* ------------------------------------------------------------------ */

#define MAX_SLOTS_USED 32
#define MAX_PORTS      64
#define EV_N           256
#define CMD_N          64
#define RING_N         64

typedef struct { u16int off; u8int size; u8int sgn; s32int lmin, lmax; u8int ok; } hfld_t;

typedef struct {
    u8int  report_id;       /* 0 = нет Report ID                       */
    u8int  absolute;
    hfld_t x, y, wheel;
    hfld_t btn[8];
    u8int  found;
} hidmap_t;

typedef struct xdev {
    int      used;
    u8int    slot;
    u8int    rport;        /* root hub port (1-based)             */
    u8int    speed;        /* PORTSC speed ID                     */
    u32int   route;
    u8int    depth;        /* 0 = на root-порте                   */
    u8int    parent_slot;  /* 0 = root                            */
    u8int    parent_port;
    u8int    tt_slot, tt_port;
    u8int   *in_ctx;  u64int in_phys;
    u8int   *out_ctx; u64int out_phys;
    ring_t   ep0;     void *ep0_mem; u64int ep0_phys;
    u16int   mps0;
    /* hub */
    int      is_hub;
    u8int    hub_ports;
    u8int    hub_ss;
    u8int    hub_pwrgood;
    u32int   hub_poll_at;   /* tick последнего опроса портов хаба (hot-plug) */
    /* mouse */
    int      mouse;
    u8int    mdci;
    u16int   mrpt_len;
    ring_t   mring;   void *mring_mem; u64int mring_phys;
    u8int   *mrpt;    u64int mrpt_phys;
    int      use_map;     /* 1 = отчёты по report descriptor, 0 = boot */
    hidmap_t map;
    /* keyboard (boot-протокол: 8 байт = модификаторы + reserv + 6 скан) */
    int      kbd;
    u8int    kdci;
    u16int   krpt_len;
    ring_t   kring;   void *kring_mem; u64int kring_phys;
    u8int   *krpt;    u64int krpt_phys;
    u8int    kmod_prev;      /* состояние модификаторов прошлого отчёта */
    u8int    kkey_prev[6];   /* usage-коды нажатых клавиш прошлого отчёта */
    /* какой endpoint (dci) ждёт восстановления после ошибки */
    u8int    err_dci;
    volatile int need_recover;
} xdev_t;

static xdev_t devs[MAX_SLOTS_USED + 1];

static int      xready = 0;
static int      csz = 32;
static u32int   max_slots = 0, max_ports = 0;
static u32int   db_off, rt_off;
static u64int  *dcbaa;   static u64int dcbaa_phys;
static ring_t   cmdr;
static trb_t   *evr;     static u64int ev_phys;
static u32int   ev_idx = 0, ev_cyc = 1;
static u8int    port_usb3[MAX_PORTS + 1];
static void    *ctl_buf; static u64int ctl_phys;

static volatile int in_service = 0;
static volatile u64int port_attention = 0;
static int mouse_cnt = 0;
static int kbd_cnt = 0;

/* ожидание команды */
static volatile int cmd_pending = 0;
static volatile u32int cmd_code = 0, cmd_slot = 0;
/* ожидание control-transfer */
static volatile int ctl_pending = 0;
static volatile u32int ctl_code = 0, ctl_slot_wait = 0;

/* ------------------------------------------------------------------ */
/* Doorbells                                                          */
/* ------------------------------------------------------------------ */

static void doorbell(u32int slot, u32int target)
{
    MFENCE();
    wr32(dbr, slot * 4, target);
}

/* ------------------------------------------------------------------ */
/* Мини-разбор HID report descriptor (указатели)                      */
/* ------------------------------------------------------------------ */

static s32int hid_item_val(const u8int *p, u32int n, int sgn)
{
    s32int v = 0;
    for (u32int i = 0; i < n; i++) v |= (s32int)p[i] << (8 * i);
    if (sgn && n && n < 4 && (p[n - 1] & 0x80)) v |= (s32int)(~0u << (8 * n));
    return v;
}

static s32int hid_get(const u8int *r, u32int len, const hfld_t *f)
{
    if (!f->ok) return 0;
    u32int v = 0;
    for (u32int i = 0; i < f->size && i < 32; i++) {
        u32int bit = f->off + i;
        if ((bit >> 3) >= len) break;
        if (r[bit >> 3] & (1u << (bit & 7))) v |= 1u << i;
    }
    if (f->sgn && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size;
    return (s32int)v;
}

/* Находит в дескрипторе первое определение указателя (X+Y). Возвращает 1. */
static int hid_parse_pointer(const u8int *d, u32int len, hidmap_t *m)
{
    zero(m, sizeof(*m));
    u32int bitpos[16];  for (int i = 0; i < 16; i++) bitpos[i] = 0;
    u32int upage = 0, rsize = 0, rcount = 0, rid = 0;
    s32int lmin = 0, lmax = 0;
    u32int usages[16]; u32int nus = 0; u32int umin = 0, umax = 0;
    int have_x_id = -1;

    for (u32int i = 0; i < len; ) {
        u8int b = d[i];
        if (b == 0xFE) { if (i + 1 >= len) break; i += 3 + d[i + 1]; continue; }
        u32int sz = b & 3; if (sz == 3) sz = 4;
        u8int type = (b >> 2) & 3, tag = b >> 4;
        if (i + 1 + sz > len) break;
        const u8int *v = &d[i + 1];

        if (type == 1) {                                   /* Global */
            switch (tag) {
            case 0: upage = (u32int)hid_item_val(v, sz, 0); break;
            case 1: lmin  = hid_item_val(v, sz, 1); break;
            case 2: lmax  = hid_item_val(v, sz, lmin < 0); break;
            case 7: rsize = (u32int)hid_item_val(v, sz, 0); break;
            case 8: rid   = (u32int)hid_item_val(v, sz, 0) & 15; break;
            case 9: rcount= (u32int)hid_item_val(v, sz, 0); break;
            }
        } else if (type == 2) {                            /* Local */
            u32int val = (u32int)hid_item_val(v, sz, 0);
            if (tag == 0 && nus < 16) usages[nus++] = (sz == 4) ? (val & 0xFFFF) : val;
            else if (tag == 1) umin = val;
            else if (tag == 2) umax = val;
        } else if (type == 0) {                            /* Main */
            if (tag == 8) {                                /* Input */
                u32int flags = (u32int)hid_item_val(v, sz, 0);
                for (u32int n = 0; n < rcount; n++) {
                    u32int u = 0; int have_u = 0;
                    if (n < nus) { u = usages[n]; have_u = 1; }
                    else if (nus == 0 && umax >= umin && umin) { u = umin + n; have_u = 1; }
                    else if (nus) { u = usages[nus - 1]; have_u = 1; }
                    if (!(flags & 1) && have_u) {          /* Data */
                        hfld_t f;
                        f.off = (u16int)(bitpos[rid] + n * rsize + (rid ? 8 : 0));
                        f.size = (u8int)rsize; f.sgn = lmin < 0;
                        f.lmin = lmin; f.lmax = lmax; f.ok = 1;
                        if (upage == 1 && (u == 0x30 || u == 0x31 || u == 0x38) &&
                            (have_x_id < 0 || have_x_id == (int)rid)) {
                            if (u == 0x30) { m->x = f; m->absolute = !(flags & 4); have_x_id = (int)rid; m->report_id = (u8int)rid; }
                            else if (u == 0x31) m->y = f;
                            else m->wheel = f;
                        } else if (upage == 9 && u >= 1 && u <= 8 &&
                                   (have_x_id < 0 || have_x_id == (int)rid)) {
                            m->btn[u - 1] = f;
                        }
                    }
                }
                bitpos[rid] += rcount * rsize;
            }
            nus = 0; umin = umax = 0;
        }
        i += 1 + sz;
    }
    /* кнопки, объявленные до X в том же report id, могли не пройти фильтр */
    m->found = m->x.ok && m->y.ok;
    return m->found;
}

/* ------------------------------------------------------------------ */
/* Обработка отчётов мыши                                             */
/* ------------------------------------------------------------------ */

static void mouse_queue_trb(xdev_t *d)
{
    ring_push(&d->mring, (u32int)d->mrpt_phys, (u32int)(d->mrpt_phys >> 32),
              d->mrpt_len, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    doorbell(d->slot, d->mdci);
}

extern int m_cursor_x, m_cursor_y;
extern volatile int mouse_buttons;
extern uint32_t framebuffer_width, framebuffer_height;

static void mouse_report(xdev_t *d, u32int len)
{
    if (!d->use_map) {
        if (len < 3) return;
        int dx = (int)(int8_t)d->mrpt[1];
        int dy = (int)(int8_t)d->mrpt[2];
        unsigned b = d->mrpt[0] & 7u;
        /* USB: +Y вниз.  mouse_handle_packet() ждёт PS/2 (+Y вверх) */
        mouse_handle_packet(dx, -dy, b);
        return;
    }

    const u8int *r = d->mrpt;
    const hidmap_t *m = &d->map;
    if (m->report_id) {
        if (len < 1 || r[0] != m->report_id) return;
    }
    unsigned b = 0;
    for (int i = 0; i < 8; i++)
        if (m->btn[i].ok && hid_get(r, len, &m->btn[i])) b |= 1u << i;
    b &= 7u;

    int x = hid_get(r, len, &m->x), y = hid_get(r, len, &m->y);
    if (!m->absolute) {
        mouse_handle_packet(x, -y, b);
    } else {
        s32int xr = m->x.lmax - m->x.lmin + 1, yr = m->y.lmax - m->y.lmin + 1;
        if (xr <= 0) xr = 1;
        if (yr <= 0) yr = 1;
        long long cx = ((long long)(x - m->x.lmin) * (long long)framebuffer_width) / xr;
        long long cy = ((long long)(y - m->y.lmin) * (long long)framebuffer_height) / yr;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx >= (long long)framebuffer_width)  cx = (long long)framebuffer_width - 1;
        if (cy >= (long long)framebuffer_height) cy = (long long)framebuffer_height - 1;
        m_cursor_x = (int)cx;
        m_cursor_y = (int)cy;
        mouse_buttons = (int)b;
    }
}

/* ------------------------------------------------------------------ */
/* USB-клавиатура (boot-протокол)                                     */
/* ------------------------------------------------------------------ */

/*
 * Отчёт boot-клавиатуры: 8 байт
 *   [0] модификаторы, [1] зарезервировано, [2..7] коды клавиш (usage)
 *
 * Таблица HID usage -> PS/2 scancode (set 1, тот же, что шлёт PS/2
 * клавиатура в keyboard_handle_scancode()).  Бит 0x80 = клавиша
 * "расширенная" (в PS/2 шла бы с префиксом 0xE0).
 */
#define KBD_EXT 0x80

static const u8int kbd_usage_sc[256] = {
    /* 0x00..0x03 -- пусто / ошибки / rollover */
    [0x00] = 0, [0x01] = 0, [0x02] = 0, [0x03] = 0,

    /* 0x04..0x1D: a..z */
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,

    /* 0x1E..0x27: 1..9, 0 */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,
    [0x26] = 0x0A, [0x27] = 0x0B,

    [0x28] = 0x1C,          /* Enter            */
    [0x29] = 0x01,          /* Esc              */
    [0x2A] = 0x0E,          /* Backspace        */
    [0x2B] = 0x0F,          /* Tab              */
    [0x2C] = 0x39,          /* Space            */
    [0x2D] = 0x0C,          /* - _              */
    [0x2E] = 0x0D,          /* = +              */
    [0x2F] = 0x1A,          /* [ {              */
    [0x30] = 0x1B,          /* ] }              */
    [0x31] = 0x2B,          /* \ |              */
    [0x33] = 0x27,          /* ; :              */
    [0x34] = 0x28,          /* ' "              */
    [0x35] = 0x29,          /* ` ~              */
    [0x36] = 0x33,          /* , <              */
    [0x37] = 0x34,          /* . >              */
    [0x38] = 0x35,          /* / ?              */
    [0x39] = 0x3A,          /* Caps Lock        */

    /* 0x3A..0x45: F1..F12 (F11/F12 в PS/2 -- расширенные) */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,
    [0x42] = 0x43, [0x43] = 0x44,
    [0x44] = KBD_EXT | 0x57,      /* F11 */
    [0x45] = KBD_EXT | 0x58,      /* F12 */

    [0x46] = KBD_EXT | 0x37,      /* Print Screen (SysReq) */
    [0x47] = 0x46,          /* Scroll Lock */
    [0x48] = 0,             /* Pause (0xE1-скан, пропускаем) */
    [0x49] = KBD_EXT | 0x52,      /* Insert */
    [0x4A] = KBD_EXT | 0x47,      /* Home   */
    [0x4B] = KBD_EXT | 0x49,      /* Page Up */
    [0x4C] = KBD_EXT | 0x53,      /* Delete */
    [0x4D] = KBD_EXT | 0x4F,      /* End    */
    [0x4E] = KBD_EXT | 0x51,      /* Page Down */
    [0x4F] = KBD_EXT | 0x4D,      /* Right  */
    [0x50] = KBD_EXT | 0x4B,      /* Left   */
    [0x51] = KBD_EXT | 0x50,      /* Down   */
    [0x52] = KBD_EXT | 0x48,      /* Up     */
    [0x53] = 0x45,          /* NumLock */
    [0x54] = KBD_EXT | 0x35,      /* KP /    */
    [0x55] = 0x37,          /* KP *    */
    [0x56] = 0x4A,          /* KP -    */
    [0x57] = 0x4E,          /* KP +    */
    [0x58] = KBD_EXT | 0x1C,      /* KP Enter */
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,   /* KP 1..3 */
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D,   /* KP 4..6 */
    [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,   /* KP 7..9 */
    [0x62] = 0x52,          /* KP 0    */
    [0x63] = 0x53,          /* KP .    */
    [0x64] = KBD_EXT | 0x5E,      /* ISO <>  */
};

/* Модификаторы: [битоёмкость hid][0 = make, 1 = break scancode] */
static const u8int kbd_mod_sc[8][2] = {
    { 0x1D, 0x1D | 0x80 },          /* Left Ctrl   */
    { 0x2A, 0x2A | 0x80 },          /* Left Shift  */
    { 0x38, 0x38 | 0x80 },          /* Left Alt    */
    { KBD_EXT | 0x5B, KBD_EXT | (0x5B | 0x80) },   /* Left GUI  */
    { KBD_EXT | 0x1D, KBD_EXT | (0x1D | 0x80) },   /* Right Ctrl */
    { 0x36, 0x36 | 0x80 },          /* Right Shift */
    { KBD_EXT | 0x38, KBD_EXT | (0x38 | 0x80) },   /* Right Alt  */
    { KBD_EXT | 0x5C, KBD_EXT | (0x5C | 0x80) },   /* Right GUI  */
};

static void keyboard_queue_trb(xdev_t *d)
{
    ring_push(&d->kring, (u32int)d->krpt_phys, (u32int)(d->krpt_phys >> 32),
              d->krpt_len, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    doorbell(d->slot, d->kdci);
}

/* Отправить через общий стек клавиатуры один scancode,
 * при необходимости с префиксом 0xE0. */
static void kbd_send_sc(u8int e, int is_break)
{
    u8int sc = (u8int)(e & 0x7F);

    if (is_break)
        sc |= 0x80;

    if (e & KBD_EXT)
        keyboard_handle_scancode(0xE0);

    keyboard_handle_scancode(sc);
}

static void keyboard_report(xdev_t *d, u32int len)
{
    if (len < 8)
        return;

    u8int mod = d->krpt[0];
    u8int keys[6];
    for (int i = 0; i < 6; i++)
        keys[i] = d->krpt[2 + i];

    /* 1. Модификаторы -- diff с прошлым отчётом. */
    for (int m = 0; m < 8; m++) {
        u8int bit = (u8int)(1u << m);
        if ((mod & bit) && !(d->kmod_prev & bit))
            kbd_send_sc(kbd_mod_sc[m][0], 0);
        if (!(mod & bit) && (d->kmod_prev & bit))
            kbd_send_sc(kbd_mod_sc[m][1], 1);
    }
    d->kmod_prev = mod;

    /* 2. Отпускания -- клавиши, которых нет в новом отчёте. */
    for (int j = 0; j < 6; j++) {
        u8int prev = d->kkey_prev[j];
        if (prev <= 3)
            continue;                    /* пусто/rollover */
        int still = 0;
        for (int i = 0; i < 6; i++)
            if (keys[i] == prev) { still = 1; break; }
        if (!still) {
            u8int sc = kbd_usage_sc[prev];
            if (sc)
                kbd_send_sc(sc, 1);
        }
    }

    /* 3. Нажатия -- новые клавиши. */
    for (int i = 0; i < 6; i++) {
        u8int k = keys[i];
        if (k <= 3)
            continue;
        int already = 0;
        for (int j = 0; j < 6; j++)
            if (d->kkey_prev[j] == k) { already = 1; break; }
        if (!already) {
            u8int sc = kbd_usage_sc[k];
            if (sc)
                kbd_send_sc(sc, 0);
        }
    }

    for (int i = 0; i < 6; i++)
        d->kkey_prev[i] = keys[i];
}

/* ------------------------------------------------------------------ */
/* Разбор event ring                                                  */
/* ------------------------------------------------------------------ */

static void handle_event(trb_t *e)
{
    u32int d2 = e->d[2], d3 = e->d[3];
    u32int type = (d3 >> 10) & 0x3F;

    if (type == TRB_EV_CMD) {
        if (cmd_pending) {
            cmd_code = d2 >> 24;
            cmd_slot = d3 >> 24;
            BARRIER();
            cmd_pending = 0;
        }
    } else if (type == TRB_EV_TRANSFER) {
        u32int slot = d3 >> 24, epid = (d3 >> 16) & 0x1F, code = d2 >> 24;
        u32int resid = d2 & 0xFFFFFF;
        if (slot > MAX_SLOTS_USED) return;
        xdev_t *d = &devs[slot];
        if (epid == 1) {
            if (ctl_pending && ctl_slot_wait == slot) {
                ctl_code = code;
                BARRIER();
                ctl_pending = 0;
            }
        } else if (d->used && d->kbd && epid == (u32int)d->kdci) {
            if (code == 1 || code == 13) {
                keyboard_report(d, (u32int)d->krpt_len - resid);
                keyboard_queue_trb(d);
            } else {
                d->err_dci = (u8int)epid;
                d->need_recover = 1;   /* STALL / babble / txn error ... */
            }
        } else if (d->used && d->mouse && epid == d->mdci) {
            if (code == 1 || code == 13) {
                mouse_report(d, d->mrpt_len - resid);
                mouse_queue_trb(d);
            } else {
                d->err_dci = (u8int)epid;
                d->need_recover = 1;   /* STALL / babble / txn error ... */
            }
        }
    } else if (type == TRB_EV_PORT) {
        u32int port = e->d[0] >> 24;      /* Port ID -- в DW0, не в DW3 */
        if (xh_trace) { lg("XHCI: port change event, port "); lgd(port); lg("\n"); }
        if (port >= 1 && port <= MAX_PORTS)
            port_attention |= (1ULL << (port - 1));
    }
}

static void service(void)
{
    u64int fl = irq_save();
    if (in_service) { irq_restore(fl); return; }
    in_service = 1;
    irq_restore(fl);

    int any = 0;
    for (;;) {
        trb_t *e = &evr[ev_idx];
        u32int d3 = e->d[3];
        if ((d3 & 1) != ev_cyc) break;
        BARRIER();
        handle_event(e);
        any = 1;
        if (++ev_idx == EV_N) { ev_idx = 0; ev_cyc ^= 1; }
    }
    if (any) {
        u64int p = ev_phys + (u64int)ev_idx * 16;
        wr32(rt, 0x20 + 0x1C, (u32int)(p >> 32));
        wr32(rt, 0x20 + 0x18, (u32int)p | (1u << 3));   /* EHB */
    }
    in_service = 0;
}

void xhci_timer_poll(void)
{
    if (xready) service();
}

/* ------------------------------------------------------------------ */
/* Команды                                                            */
/* ------------------------------------------------------------------ */

/* Возвращает completion code (1 = success) или -1 при таймауте. */
static int cmd_run(u32int d0, u32int d1, u32int d2, u32int d3, u32int *slot_out)
{
    cmd_pending = 1;
    ring_push(&cmdr, d0, d1, d2, d3);
    doorbell(0, 0);
    for (int i = 0; i < 40000; i++) {          /* ~2 c */
        service();
        if (!cmd_pending) {
            if (slot_out) *slot_out = cmd_slot;
            return (int)cmd_code;
        }
        udelay(50);
    }
    cmd_pending = 0;
    lg("XHCI: command timeout\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Control transfer на EP0                                            */
/* ------------------------------------------------------------------ */

/* len > 0 && dir_in : данные читаются в ctl_buf;
 * len > 0 && !dir_in: данные берутся из ctl_buf.  0 = успех. */
static int ctrl(xdev_t *d, u8int bm, u8int req, u16int val, u16int idx,
                u16int len, int dir_in)
{
    u32int trt = len ? (dir_in ? 3 : 2) : 0;

    ctl_slot_wait = d->slot;
    ctl_pending = 1;

    ring_push(&d->ep0,
              (u32int)bm | ((u32int)req << 8) | ((u32int)val << 16),
              (u32int)idx | ((u32int)len << 16), 8,
              (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (len)
        ring_push(&d->ep0, (u32int)ctl_phys, (u32int)(ctl_phys >> 32), len,
                  (TRB_DATA << 10) | (dir_in ? (1u << 16) : 0));
    int status_in = (len == 0) || !dir_in;
    ring_push(&d->ep0, 0, 0, 0,
              (TRB_STATUS << 10) | (1u << 5) | (status_in ? (1u << 16) : 0));
    doorbell(d->slot, 1);

    for (int i = 0; i < 40000; i++) {          /* ~2 c */
        service();
        if (!ctl_pending) {
            if (ctl_code == 1 || ctl_code == 13) return 0;
            lg("XHCI: control xfer error code=");
            lgd(ctl_code);
            lg("\n");
            /* STALL и т.п. -- сбросить EP0 */
            if (ctl_code == 6 || ctl_code == 4 || ctl_code == 3) {
                cmd_run(0, 0, 0, (TRB_RESET_EP << 10) | (1u << 16) |
                                  ((u32int)d->slot << 24), 0);
                u64int p = d->ep0.phys + (u64int)d->ep0.enq * 16;
                cmd_run((u32int)p | d->ep0.cyc, (u32int)(p >> 32), 0,
                        (TRB_SET_DEQ << 10) | (1u << 16) |
                        ((u32int)d->slot << 24), 0);
            }
            return -1;
        }
        udelay(50);
    }
    ctl_pending = 0;
    lg("XHCI: control xfer timeout\n");
    return -1;
}

static int get_desc(xdev_t *d, u8int type, u8int idx, u16int len)
{
    zero(ctl_buf, len);
    return ctrl(d, 0x80, 6, ((u16int)type << 8) | idx, 0, len, 1);
}

/* ------------------------------------------------------------------ */
/* Выделение/освобождение устройства                                  */
/* ------------------------------------------------------------------ */

static u16int default_mps0(u8int speed)
{
    switch (speed) {
    case 4: case 5: return 512;
    case 3:         return 64;
    default:        return 8;     /* LS / FS (FS уточняется позже) */
    }
}

static void dev_free(xdev_t *d)
{
    if (d->in_ctx)    pmm_free_page(d->in_phys);
    if (d->out_ctx)   pmm_free_page(d->out_phys);
    if (d->ep0_mem)   pmm_free_page(d->ep0_phys);
    if (d->mring_mem) pmm_free_page(d->mring_phys);
    zero(d, sizeof(*d));
}

static void dev_remove(xdev_t *d)
{
    if (!d->used) return;
    lg("XHCI: removing slot ");
    lgd(d->slot);
    lg("\n");
    if (d->mouse) { mouse_cnt--; mouse_handle_packet(0, 0, 0); }
    d->mouse = 0;
    if (d->kbd) kbd_cnt--;          /* клавиатура ушла -- стек событий живёт дальше */
    d->kbd = 0;
    u32int slot = d->slot;
    cmd_run(0, 0, 0, (TRB_DISABLE_SLOT << 10) | (slot << 24), 0);
    dcbaa[slot] = 0;
    dev_free(d);
}

/* ------------------------------------------------------------------ */
/* Построение input-контекстов                                        */
/* ------------------------------------------------------------------ */

#define IC_CTRL(d)       ((volatile u32int *)((d)->in_ctx))
#define IC_SLOT(d)       ((volatile u32int *)((d)->in_ctx + csz))
#define IC_EP(d, dci)    ((volatile u32int *)((d)->in_ctx + csz * ((dci) + 1)))
#define OC_SLOT(d)       ((volatile u32int *)((d)->out_ctx))
#define OC_EP(d, dci)    ((volatile u32int *)((d)->out_ctx + csz * (dci)))

static void ctx_ep_set_deq(volatile u32int *e, u64int phys, u32int dcs)
{
    e[2] = (u32int)(phys & ~0xFULL) | dcs;
    e[3] = (u32int)(phys >> 32);
}

/* ------------------------------------------------------------------ */
/* Enumeration одного устройства                                      */
/* ------------------------------------------------------------------ */

static int  parse_and_setup(xdev_t *d);
static void hub_scan(xdev_t *h);

static xdev_t *enum_device(u8int rport, u8int speed, u32int route, u8int depth,
                           xdev_t *parent, u8int parent_port)
{
    u32int slot = 0;
    int rc = cmd_run(0, 0, 0, (TRB_ENABLE_SLOT << 10), &slot);
    if (rc != 1 || slot == 0 || slot > MAX_SLOTS_USED) {
        lg("XHCI: Enable Slot failed rc=");
        lgd((u64int)(long long)rc);
        lg("\n");
        return 0;
    }
    xdev_t *d = &devs[slot];
    zero(d, sizeof(*d));
    d->used = 1; d->slot = (u8int)slot; d->rport = rport; d->speed = speed;
    d->route = route; d->depth = depth;
    d->mps0 = default_mps0(speed);
    if (parent) {
        d->parent_slot = parent->slot; d->parent_port = parent_port;
        /* ближайший TT для LS/FS устройств */
        if (parent->speed == 3 && (speed == 1 || speed == 2)) {
            d->tt_slot = parent->slot; d->tt_port = parent_port;
        } else {
            d->tt_slot = parent->tt_slot; d->tt_port = parent->tt_port;
        }
    }

    d->in_ctx  = (u8int *)dma_page(&d->in_phys);
    d->out_ctx = (u8int *)dma_page(&d->out_phys);
    d->ep0_mem = dma_page(&d->ep0_phys);
    if (!d->in_ctx || !d->out_ctx || !d->ep0_mem) goto fail;
    ring_init(&d->ep0, d->ep0_mem, d->ep0_phys, RING_N);
    dcbaa[slot] = d->out_phys;

    /* ---- Address Device ---- */
    IC_CTRL(d)[1] = 3;                          /* A0 | A1 */
    IC_SLOT(d)[0] = (route & 0xFFFFF) | ((u32int)speed << 20) | (1u << 27);
    IC_SLOT(d)[1] = (u32int)rport << 16;
    IC_SLOT(d)[2] = (u32int)d->tt_slot | ((u32int)d->tt_port << 8);
    {
        volatile u32int *e = IC_EP(d, 1);
        e[1] = (3u << 1) | (4u << 3) | ((u32int)d->mps0 << 16);
        ctx_ep_set_deq(e, d->ep0_phys, 1);
        e[4] = 8;
    }
    rc = cmd_run((u32int)d->in_phys, (u32int)(d->in_phys >> 32), 0,
                 (TRB_ADDR_DEV << 10) | (slot << 24), 0);
    if (rc != 1) {
        lg("XHCI: Address Device failed rc=");
        lgd((u64int)(long long)rc);
        lg("\n");
        goto fail;
    }
    mdelay(5);

    /* ---- Первые 8 байт дескриптора: bMaxPacketSize0 ---- */
    if (get_desc(d, 1, 0, 8) != 0) {
        /* ещё одна попытка: некоторым устройствам нужно время */
        mdelay(20);
        if (get_desc(d, 1, 0, 8) != 0) goto fail;
    }
    {
        u8int *b = (u8int *)ctl_buf;
        u16int mps = b[7];
        if (d->speed >= 4) mps = (u16int)(1u << b[7]);   /* SS: 2^n */
        if (mps && mps != d->mps0) {
            d->mps0 = mps;
            zero(d->in_ctx, 4096);
            IC_CTRL(d)[1] = 2;                           /* A1 */
            volatile u32int *e = IC_EP(d, 1);
            volatile u32int *o = OC_EP(d, 1);
            e[0] = o[0]; e[1] = (o[1] & 0xFFFF) | ((u32int)mps << 16);
            e[2] = o[2]; e[3] = o[3]; e[4] = o[4];
            rc = cmd_run((u32int)d->in_phys, (u32int)(d->in_phys >> 32), 0,
                         (TRB_EVAL_CTX << 10) | (slot << 24), 0);
            if (rc != 1) { lg("XHCI: Evaluate Context failed\n"); goto fail; }
        }
    }

    /* ---- Полный дескриптор устройства + конфигурация ---- */
    if (get_desc(d, 1, 0, 18) != 0) goto fail;
    {
        u8int *b = (u8int *)ctl_buf;
        lg("XHCI: slot "); lgd(slot);
        lg(" VID:PID "); lgh((u64int)b[8] | ((u64int)b[9] << 8));
        lg(":"); lgh((u64int)b[10] | ((u64int)b[11] << 8));
        lg(" class "); lgd(b[4]);
        lg(" speed "); lgd(speed);
        lg("\n");
    }
    if (parse_and_setup(d) < 0) goto fail;

    if (d->is_hub) hub_scan(d);
    return d;

fail:
    dev_remove(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Разбор конфигурации, настройка мыши / хаба                         */
/* ------------------------------------------------------------------ */

/* Настройка interrupt IN endpoint HID-устройства (мышь или клавиатура).
 * is_kbd=1 -- создать приёмный буфер/кольцо под 8-байтовые boot-отчёты
 * клавиатуры, 0 -- мышиной путь (структуры mring/mrpt). */
static int configure_int_ep(xdev_t *d, u8int epaddr, u16int mps,
                            u8int interval, int is_kbd)
{
    u32int dci = (u32int)(epaddr & 0xF) * 2 + 1;       /* IN */

    ring_t   *ring;  void **mem;  u64int *phys;
    u8int   **rpt;   u64int *rpt_p; u16int *len;  u8int *dcip;

    if (is_kbd) {
        ring = &d->kring; mem = &d->kring_mem; phys = &d->kring_phys;
        rpt  = &d->krpt;  rpt_p = &d->krpt_phys; len = &d->krpt_len;
        dcip = &d->kdci;
    } else {
        ring = &d->mring; mem = &d->mring_mem; phys = &d->mring_phys;
        rpt  = &d->mrpt;  rpt_p = &d->mrpt_phys; len = &d->mrpt_len;
        dcip = &d->mdci;
    }

    *mem = dma_page(phys);
    if (!*mem) return -1;
    ring_init(ring, *mem, *phys, RING_N);
    *rpt  = (u8int *)*mem + 2048;
    *rpt_p = *phys + 2048;
    *len = is_kbd ? (mps > 8 ? 8 : mps) : (mps > 64 ? 64 : mps);
    *dcip = (u8int)dci;

    /* interval: для LS/FS -- миллисекунды -> экспонента (2^n * 125 мкс) */
    u32int iv;
    if (d->speed == 1 || d->speed == 2) {
        u32int ms8 = (u32int)(interval ? interval : 1) * 8;
        iv = 0;
        while ((2u << iv) <= ms8 && iv < 10) iv++;
        if (iv < 3) iv = 3;
        if (iv > 10) iv = 10;
    } else {
        iv = interval ? (u32int)interval - 1 : 0;
        if (iv > 15) iv = 15;
    }

    zero(d->in_ctx, 4096);
    IC_CTRL(d)[1] = 1u | (1u << dci);
    volatile u32int *s = IC_SLOT(d);
    volatile u32int *os = OC_SLOT(d);
    for (int i = 0; i < 8; i++) s[i] = os[i];
    s[0] = (s[0] & 0x07FFFFFF) | (dci << 27);
    volatile u32int *e = IC_EP(d, dci);
    e[0] = iv << 16;
    e[1] = (3u << 1) | (7u << 3) | ((u32int)mps << 16);
    ctx_ep_set_deq(e, *phys, 1);
    e[4] = (u32int)mps | ((u32int)mps << 16);

    int rc = cmd_run((u32int)d->in_phys, (u32int)(d->in_phys >> 32), 0,
                     (TRB_CONFIG_EP << 10) | ((u32int)d->slot << 24), 0);
    if (rc != 1) {
        lg("XHCI: Configure Endpoint failed rc=");
        lgd((u64int)(long long)rc);
        lg("\n");
        return -1;
    }
    return 0;
}

static int configure_hub_slot(xdev_t *d, int mtt, u32int think)
{
    zero(d->in_ctx, 4096);
    IC_CTRL(d)[1] = 1;                                  /* A0 */
    volatile u32int *s = IC_SLOT(d);
    volatile u32int *os = OC_SLOT(d);
    for (int i = 0; i < 8; i++) s[i] = os[i];
    s[0] |= (1u << 26) | (mtt ? (1u << 25) : 0);
    s[1] = (s[1] & 0x00FFFFFF) | ((u32int)d->hub_ports << 24);
    s[2] = (s[2] & ~(3u << 16)) | ((think & 3) << 16);
    int rc = cmd_run((u32int)d->in_phys, (u32int)(d->in_phys >> 32), 0,
                     (TRB_CONFIG_EP << 10) | ((u32int)d->slot << 24), 0);
    return rc == 1 ? 0 : -1;
}

typedef struct {
    u8int  iface, proto, ep, iv;
    u16int mps, rdlen;
} hidcand_t;

static u8int cfgbuf[1024];

static int parse_and_setup(xdev_t *d)
{
    u8int devclass = ((u8int *)ctl_buf)[4];

    if (get_desc(d, 2, 0, 9) != 0) return -1;
    u16int total = (u16int)((u8int *)ctl_buf)[2] | ((u16int)((u8int *)ctl_buf)[3] << 8);
    if (total < 9) return -1;
    if (total > sizeof(cfgbuf)) total = sizeof(cfgbuf);
    if (get_desc(d, 2, 0, total) != 0) return -1;
    for (u32int i = 0; i < total; i++) cfgbuf[i] = ((u8int *)ctl_buf)[i];

    u8int *p = cfgbuf;
    u8int cfgval = p[5];

    int is_hub_cfg = (devclass == 9);
    hidcand_t cand[4]; int ncand = 0;
    int cur = -1;                      /* индекс кандидата для текущего интерфейса */

    for (u32int off = 0; off + 2 <= total && p[off] >= 2; off += p[off]) {
        u8int len = p[off], type = p[off + 1];
        if (type == 4 && len >= 9) {                    /* interface */
            u8int num = p[off + 2], alt = p[off + 3];
            u8int cls = p[off + 5], pro = p[off + 7];
            if (cls == 9) is_hub_cfg = 1;
            cur = -1;
            /* HID-интерфейсы интересны целиком: proto 1 = клавиатура,
             * 2 = мышь (boot), 0 = generic (разбирается по descriptor'у). */
            if (cls == 3 && alt == 0 && ncand < 4) {
                cur = ncand++;
                cand[cur].iface = num; cand[cur].proto = pro;
                cand[cur].ep = 0; cand[cur].mps = 0; cand[cur].iv = 0; cand[cur].rdlen = 0;
            }
        } else if (type == 0x21 && len >= 9 && cur >= 0) {      /* HID descriptor */
            for (u32int k = 6; k + 2 < len; k += 3)
                if (p[off + k] == 0x22)
                    cand[cur].rdlen = (u16int)(p[off + k + 1] | (p[off + k + 2] << 8));
        } else if (type == 5 && len >= 7 && cur >= 0) {         /* endpoint */
            u8int addr = p[off + 2], attr = p[off + 3];
            if ((attr & 3) == 3 && (addr & 0x80) && !cand[cur].ep) {
                cand[cur].ep = addr;
                cand[cur].mps = (u16int)(p[off + 4] | (p[off + 5] << 8)) & 0x7FF;
                cand[cur].iv = p[off + 6];
            }
        }
    }

    /* SET_CONFIGURATION */
    if (ctrl(d, 0x00, 9, cfgval, 0, 0, 0) != 0) return -1;

    if (is_hub_cfg) {
        d->is_hub = 1;
        d->hub_ss = (d->speed >= 4);
        zero(ctl_buf, 16);
        if (ctrl(d, 0xA0, 6, d->hub_ss ? 0x2A00 : 0x2900, 0, 12, 1) != 0) {
            lg("XHCI: hub descriptor failed\n");
            return -1;
        }
        u8int *h = (u8int *)ctl_buf;
        d->hub_ports = h[2];
        d->hub_pwrgood = h[5];
        u32int think = (((u32int)h[3] | ((u32int)h[4] << 8)) >> 5) & 3;
        lg("XHCI: hub with "); lgd(d->hub_ports); lg(" ports\n");
        if (d->hub_ss)
            ctrl(d, 0x20, 0x0C, d->depth, 0, 0, 0);     /* SET_HUB_DEPTH */
        if (configure_hub_slot(d, 0, think) != 0) {
            lg("XHCI: hub slot configure failed\n");
            return -1;
        }
        return 0;
    }

    /* --- Приоритет: boot-клавиатура (proto 1) --- */
    for (int i = 0; i < ncand; i++) {
        hidcand_t *c = &cand[i];
        if (c->proto != 1 || !c->ep) continue;
        lg("XHCI: HID boot keyboard on iface "); lgd(c->iface);
        lg(" ep "); lgh(c->ep); lg(" mps "); lgd(c->mps); lg("\n");
        if (configure_int_ep(d, c->ep, c->mps, c->iv, 1) != 0) return -1;
        ctrl(d, 0x21, 0x0B, 0, c->iface, 0, 0);          /* SET_PROTOCOL(Boot) */
        ctrl(d, 0x21, 0x0A, 0, c->iface, 0, 0);          /* SET_IDLE(0)        */
        zero(d->kkey_prev, sizeof(d->kkey_prev));
        d->kmod_prev = 0;
        d->kbd = 1;
        kbd_cnt++;
        keyboard_queue_trb(d);
        boot_stage("xHCI: USB keyboard attached");
        /* продолжаем: комбинированные клавиатуры могут иметь и мышь */
    }

    /* --- boot-мышь (subclass/proto = 2) -- самый надёжный путь --- */
    for (int i = 0; i < ncand; i++) {
        hidcand_t *c = &cand[i];
        if (c->proto != 2 || !c->ep) continue;
        lg("XHCI: HID boot mouse on iface "); lgd(c->iface);
        lg(" ep "); lgh(c->ep); lg(" mps "); lgd(c->mps); lg("\n");
        d->use_map = 0;
        if (configure_int_ep(d, c->ep, c->mps, c->iv, 0) != 0) return -1;
        ctrl(d, 0x21, 0x0B, 0, c->iface, 0, 0);          /* SET_PROTOCOL(Boot) */
        ctrl(d, 0x21, 0x0A, 0, c->iface, 0, 0);          /* SET_IDLE(0)        */
        d->mouse = 1;
        mouse_cnt++;
        mouse_queue_trb(d);
        boot_stage("xHCI: USB mouse attached");
        /* продолжаем: комбинированные мыши могут иметь и клавиатуру */
    }

    /* --- любой HID-указатель (планшет/тачпад/мышь без boot-протокола),
     *     только если мыши ещё нет --- */
    if (!d->mouse && !d->kbd) {
        for (int i = 0; i < ncand; i++) {
            hidcand_t *c = &cand[i];
            if (!c->ep || !c->rdlen) continue;
            u16int rl = c->rdlen > 2048 ? 2048 : c->rdlen;
            zero(ctl_buf, rl);
            if (ctrl(d, 0x81, 6, 0x2200, c->iface, rl, 1) != 0) continue;
            if (!hid_parse_pointer((u8int *)ctl_buf, rl, &d->map)) continue;
            lg("XHCI: HID pointer (report protocol) iface "); lgd(c->iface);
            lg(d->map.absolute ? " absolute" : " relative");
            lg(" ep "); lgh(c->ep); lg(" mps "); lgd(c->mps); lg("\n");
            d->use_map = 1;
            if (configure_int_ep(d, c->ep, c->mps, c->iv, 0) != 0) return -1;
            ctrl(d, 0x21, 0x0A, 0, c->iface, 0, 0);      /* SET_IDLE(0) */
            d->mouse = 1;
            mouse_cnt++;
            mouse_queue_trb(d);
            boot_stage("xHCI: USB pointer attached");
            break;
        }
    }

    if (d->mouse || d->kbd)
        return 0;

    /*
     * r12-hwdiag: подробная диагностика — печатаем все найденные
     * интерфейсы и HID-кандидаты, чтобы по экрану было видно,
     * почему устройство не классифицировалось.
     */
    lg("XHCI: no driver for device (class ");
    lgd(devclass);
    lg("), ifaces ");
    lgd(ncand);
    lg(":");
    for (int i = 0; i < ncand; i++) {
        lg(" ["); lgd(cand[i].iface);
        lg(" p"); lgd(cand[i].proto);
        lg(cand[i].ep ? " ep-ok" : " no-ep");
        lg(cand[i].rdlen ? " rd-ok" : " no-rd");
        lg("]");
    }
    if (!ncand)
        lg(" (no HID ifaces)");
    lg("\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Хабы                                                               */
/* ------------------------------------------------------------------ */

static int hub_port_status(xdev_t *h, u8int port, u16int *st, u16int *ch)
{
    if (ctrl(h, 0xA3, 0, 0, port, 4, 1) != 0) return -1;
    u8int *b = (u8int *)ctl_buf;
    *st = (u16int)(b[0] | (b[1] << 8));
    *ch = (u16int)(b[2] | (b[3] << 8));
    return 0;
}

/* Ресет + перечисление устройства, появившегося на порту хаба.
 * Выделил из hub_scan: используется и при загрузке, и при hot-plug. */
static void hub_connect_port(xdev_t *h, u8int port)
{
    u16int st, ch;

    ctrl(h, 0x23, 1, 16, port, 0, 0);                /* C_PORT_CONNECTION */
    /* PORT_RESET */
    ctrl(h, 0x23, 3, 4, port, 0, 0);
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        mdelay(10);
        if (hub_port_status(h, port, &st, &ch) != 0) return;
        if (ch & (1u << 4)) { ok = 1; break; }
    }
    if (!ok) { lg("XHCI: hub port reset timeout\n"); return; }
    ctrl(h, 0x23, 1, 20, port, 0, 0);                /* C_PORT_RESET */
    if (hub_port_status(h, port, &st, &ch) != 0) return;
    if (!(st & 2)) { lg("XHCI: hub port not enabled\n"); return; }
    mdelay(20);

    u8int speed;
    if (h->hub_ss)          speed = 4;
    else if (st & (1u << 9))  speed = 2;             /* LS */
    else if (st & (1u << 10)) speed = 3;             /* HS */
    else                      speed = 1;             /* FS */

    u32int route = h->route | ((u32int)(port > 15 ? 15 : port) << (4 * h->depth));
    lg("XHCI: hub port "); lgd(port); lg(" device, speed "); lgd(speed); lg("\n");
    enum_device(h->rport, speed, route, (u8int)(h->depth + 1), h, port);
}

/* Есть ли уже перечисленное устройство на этом порту хаба? */
static int hub_port_busy(xdev_t *h, u8int port)
{
    for (u32int s = 1; s <= MAX_SLOTS_USED; s++)
        if (devs[s].used &&
            devs[s].parent_slot == h->slot &&
            devs[s].parent_port == port)
            return 1;
    return 0;
}

/*
 * r12-hubhotplug: периодический опрос портов хаба.
 *
 * Раньше hub_scan выполнялся один раз при enumeration хаба, поэтому
 * устройства, воткнутые ПОСЛЕ загрузки в порт, разведённый через
 * внутренний хаб (типично для ноутбуков), не обнаруживались никогда.
 * Теперь xhci_deferred() опрашивает хабы каждые ~400 мс: подключение
 * перечисляется, отключение — удаляет слот (рекурсивно для хабов).
 */
static void dev_remove_recursive(xdev_t *d);

static void hub_hotplug_poll(xdev_t *h)
{
    for (u8int port = 1; port <= h->hub_ports; port++) {
        u16int st, ch;
        if (hub_port_status(h, port, &st, &ch) != 0) continue;

        /* ack change-битов (ClearFeature C_PORT_xxx) */
        if (ch) {
            for (u32int bit = 0; bit < 16; bit++)
                if (ch & (1u << bit))
                    ctrl(h, 0x23, 1, (u16int)(1u << bit), port, 0, 0);
        }

        xdev_t *child = 0;
        for (u32int s = 1; s <= MAX_SLOTS_USED; s++)
            if (devs[s].used &&
                devs[s].parent_slot == h->slot &&
                devs[s].parent_port == port) {
                child = &devs[s];
                break;
            }

        if (st & 1) {
            if (child) continue;
            hub_connect_port(h, port);
        } else {
            if (child) {
                lg("XHCI: hub port "); lgd(port); lg(" device removed\n");
                dev_remove_recursive(child);
            }
        }
    }
}

static void dev_remove_recursive(xdev_t *d)
{
    for (u32int s = 1; s <= MAX_SLOTS_USED; s++)
        if (devs[s].used && devs[s].parent_slot == d->slot)
            dev_remove_recursive(&devs[s]);
    dev_remove(d);
}

static void hub_scan(xdev_t *h)
{
    if (h->depth >= 5) return;
    for (u8int port = 1; port <= h->hub_ports; port++)
        ctrl(h, 0x23, 3, 8, port, 0, 0);                /* PORT_POWER */
    mdelay((u32int)h->hub_pwrgood * 2 + 100);

    for (u8int port = 1; port <= h->hub_ports; port++) {
        u16int st, ch;
        if (hub_port_status(h, port, &st, &ch) != 0) continue;
        if (!(st & 1)) continue;                         /* нет устройства */
        hub_connect_port(h, port);
    }
}

/* ------------------------------------------------------------------ */
/* Root-порты                                                         */
/* ------------------------------------------------------------------ */

static void port_ack(u32int port, u32int ps)
{
    wr32(op, PORTSC(port), (ps & PS_NEUTRAL_MASK) | (ps & PS_CHANGE_MASK));
}

/* Возвращает 1, если порт включён и устройство готово к адресации. */
static int port_reset_and_enable(u32int port)
{
    u32int ps = rd32(op, PORTSC(port));

    if (!(ps & PS_PP)) {
        wr32(op, PORTSC(port), (ps & PS_NEUTRAL_MASK) | PS_PP);
        mdelay(30);
        ps = rd32(op, PORTSC(port));
    }

    if (port_usb3[port]) {
        /* USB3: линк обучается сам. Ждём U0 / PED до 500 мс. */
        for (int i = 0; i < 50 && !(ps & PS_PED); i++) {
            mdelay(10);
            ps = rd32(op, PORTSC(port));
            if (!(ps & PS_CCS)) return 0;
        }
        if (ps & PS_PED) { port_ack(port, ps); return 1; }
    }

    /* USB2 (или USB3, не вышедший в U0): port reset */
    port_ack(port, ps);
    ps = rd32(op, PORTSC(port));
    wr32(op, PORTSC(port), (ps & PS_NEUTRAL_MASK) | PS_PR);
    for (int i = 0; i < 100; i++) {
        mdelay(10);
        ps = rd32(op, PORTSC(port));
        if (ps & PS_PRC) break;
        if (!(ps & PS_CCS)) return 0;
    }
    if (!(ps & PS_PRC)) {
        lg("XHCI: port reset timeout\n");
        return 0;
    }
    port_ack(port, ps);
    mdelay(20);                                   /* TRSTRCY */
    ps = rd32(op, PORTSC(port));
    return (ps & PS_CCS) && (ps & PS_PED);
}

static xdev_t *root_dev(u32int port)
{
    for (u32int s = 1; s <= MAX_SLOTS_USED; s++)
        if (devs[s].used && devs[s].depth == 0 && devs[s].rport == port)
            return &devs[s];
    return 0;
}

static void remove_port_tree(u32int port)
{
    /* сначала потомки (хабы), потом сам root-device */
    for (u32int s = 1; s <= MAX_SLOTS_USED; s++)
        if (devs[s].used && devs[s].rport == port && devs[s].depth != 0)
            dev_remove(&devs[s]);
    xdev_t *r = root_dev(port);
    if (r) dev_remove(r);
}

static void handle_port(u32int port)
{
    u32int ps = rd32(op, PORTSC(port));
    int csc = (ps & PS_CSC) != 0;
    port_ack(port, ps);

    xdev_t *cur = root_dev(port);

    if (!(ps & PS_CCS)) {
        if (cur) {
            lg("XHCI: port "); lgd(port); lg(" disconnected\n");
            remove_port_tree(port);
        }
        return;
    }
    if (cur && !csc) return;
    if (cur && csc) remove_port_tree(port);

    lg("XHCI: port "); lgd(port); lg(" connect (PORTSC=");
    lgh(ps); lg(port_usb3[port] ? ") USB3\n" : ") USB2\n");

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!port_reset_and_enable(port)) {
            if (!(rd32(op, PORTSC(port)) & PS_CCS)) return;
            continue;
        }
        ps = rd32(op, PORTSC(port));
        u8int speed = (u8int)PS_SPEED(ps);
        if (speed == 0) speed = port_usb3[port] ? 4 : 1;
        if (enum_device((u8int)port, speed, 0, 0, 0, 0)) return;
        mdelay(100);
    }
    lg("XHCI: port "); lgd(port); lg(" enumeration failed\n");
}

/* ------------------------------------------------------------------ */
/* Восстановление halted endpoint'а мыши                              */
/* ------------------------------------------------------------------ */

static u8int rec_cnt[MAX_SLOTS_USED + 1];

/* Восстановление halted interrupt-IN endpoint'а (мышь или клавиатура).
 * Реальный endpoint берётся из d->err_dci, заполненного в handle_event(). */
static void recover_hid_ep(xdev_t *d)
{
    d->need_recover = 0;
    if (++rec_cnt[d->slot] > 8) {
        lg("XHCI: HID endpoint keeps failing, giving up\n");
        if (d->err_dci && d->err_dci == d->kdci) d->kbd = 0;
        else                                     d->mouse = 0;
        return;
    }
    u32int is_kbd = d->err_dci && d->err_dci == d->kdci;
    ring_t *r     = is_kbd ? &d->kring : &d->mring;
    u32int dci    = is_kbd ? d->kdci   : d->mdci;
    cmd_run(0, 0, 0, (TRB_RESET_EP << 10) | (dci << 16) | ((u32int)d->slot << 24), 0);
    u64int p = r->phys + (u64int)r->enq * 16;
    cmd_run((u32int)p | r->cyc, (u32int)(p >> 32), 0,
            (TRB_SET_DEQ << 10) | (dci << 16) | ((u32int)d->slot << 24), 0);
    if (is_kbd)
        keyboard_queue_trb(d);
    else
        mouse_queue_trb(d);
}

/* ------------------------------------------------------------------ */
/* Deferred-работа (контекст процесса)                                */
/* ------------------------------------------------------------------ */

extern u32int tick;
static volatile int def_busy = 0;
static u32int late_scan_at = 0;
static int    late_scan_done = 0;

void xhci_deferred(void)
{
    if (!xready || def_busy) return;

    int work = (port_attention != 0);
    for (u32int s = 1; s <= MAX_SLOTS_USED; s++) {
        if (devs[s].used && devs[s].need_recover) work = 1;
        if (devs[s].used && devs[s].is_hub &&
            (u32int)(tick - devs[s].hub_poll_at) >= 400) work = 1;
    }
    if (!late_scan_done && late_scan_at && tick >= late_scan_at) work = 1;
    if (!work) return;

    u64int fl = irq_save();
    if (def_busy) { irq_restore(fl); return; }
    def_busy = 1;
    irq_restore(fl);

    if (!late_scan_done && late_scan_at && tick >= late_scan_at) {
        late_scan_done = 1;
        for (u32int p = 1; p <= max_ports; p++)
            if ((rd32(op, PORTSC(p)) & PS_CCS) && !root_dev(p))
                port_attention |= (1ULL << (p - 1));
    }

    while (port_attention) {
        u32int p = 1;
        u64int m = port_attention;
        while (!(m & 1)) { m >>= 1; p++; }
        port_attention &= ~(1ULL << (p - 1));
        handle_port(p);
    }

    for (u32int s = 1; s <= MAX_SLOTS_USED; s++)
        if (devs[s].used && (devs[s].mouse || devs[s].kbd) && devs[s].need_recover)
            recover_hid_ep(&devs[s]);

    /*
     * r12-hubhotplug: периодический опрос портов хабов (~400 мс) —
     * hot-plug устройств, разведённых через внутренний хаб.
     */
    for (u32int s = 1; s <= MAX_SLOTS_USED; s++)
        if (devs[s].used && devs[s].is_hub &&
            (u32int)(tick - devs[s].hub_poll_at) >= 400) {
            devs[s].hub_poll_at = tick;
            hub_hotplug_poll(&devs[s]);
        }

    def_busy = 0;
}

int xhci_present(void)     { return xready; }
int xhci_mouse_count(void) { return mouse_cnt; }
int xhci_kbd_count(void)   { return kbd_cnt; }

/* ------------------------------------------------------------------ */
/* Расширенные capabilities: BIOS handoff, protocol -> USB2/USB3 порты */
/* ------------------------------------------------------------------ */

static void parse_ext_caps(u32int hccparams1)
{
    u32int off = ((hccparams1 >> 16) & 0xFFFF) * 4;
    int guard = 64;
    while (off && guard--) {
        u32int c = rd32(cap, off);
        u32int id = c & 0xFF, next = (c >> 8) & 0xFF;

        if (id == 1) {                                  /* USB Legacy Support */
            if (c & (1u << 16)) {                       /* BIOS owned */
                lg("XHCI: BIOS handoff...\n");
                wr32(cap, off, c | (1u << 24));         /* OS owned */
                for (int i = 0; i < 1000; i++) {
                    if (!(rd32(cap, off) & (1u << 16))) break;
                    mdelay(1);
                }
                if (rd32(cap, off) & (1u << 16))
                    lg("XHCI: BIOS did not release controller\n");
            }
            u32int v = rd32(cap, off + 4);
            v &= (3u << 1) | (0xFFu << 5) | (7u << 17);   /* SMI enables off */
            v |= (7u << 29);                              /* clear SMI status */
            wr32(cap, off + 4, v);
        } else if (id == 2) {                           /* Supported Protocol */
            u32int major = c >> 24;
            u32int d2 = rd32(cap, off + 8);
            u32int first = d2 & 0xFF, cnt = (d2 >> 8) & 0xFF;
            for (u32int p = first; p < first + cnt && p <= MAX_PORTS; p++)
                port_usb3[p] = (major >= 3);
        }
        if (!next) break;
        off += next * 4;
    }
}

/* ------------------------------------------------------------------ */
/* Инициализация                                                      */
/* ------------------------------------------------------------------ */

static int wait_sts(u32int mask, u32int want, u32int ms)
{
    for (u32int i = 0; i < ms; i++) {
        if ((rd32(op, USBSTS) & mask) == want) return 1;
        mdelay(1);
    }
    return 0;
}

static int pci_find_xhci(u8int *bus, u8int *slot, u8int *fn)
{
    for (u32int b = 0; b < 256; b++) {
        for (u32int s = 0; s < 32; s++) {
            u32int id0 = pci_rd((u8int)b, (u8int)s, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            u32int hdr = (pci_rd((u8int)b, (u8int)s, 0, 0x0C) >> 16) & 0xFF;
            u32int nf = (hdr & 0x80) ? 8 : 1;
            for (u32int f = 0; f < nf; f++) {
                if ((pci_rd((u8int)b, (u8int)s, (u8int)f, 0) & 0xFFFF) == 0xFFFF)
                    continue;
                u32int cl = pci_rd((u8int)b, (u8int)s, (u8int)f, 0x08) >> 8;
                if (cl == 0x0C0330) {
                    *bus = (u8int)b; *slot = (u8int)s; *fn = (u8int)f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

int xhci_init(void)
{
    u8int B, S, F;
    if (!pci_find_xhci(&B, &S, &F)) {
        lg("XHCI: no controller on PCI\n");
        return 0;
    }

    u32int id = pci_rd(B, S, F, 0);
    lg("XHCI: controller at "); lgd(B); lg(":"); lgd(S); lg("."); lgd(F);
    lg(" VID:DID "); lgh(id & 0xFFFF); lg(":"); lgh(id >> 16); lg("\n");

    /* --- BAR0 --- */
    u32int bar0 = pci_rd(B, S, F, 0x10);
    if (bar0 & 1) { lg("XHCI: BAR0 is I/O?!\n"); return 0; }
    int is64 = ((bar0 >> 1) & 3) == 2;
    u32int bar1 = is64 ? pci_rd(B, S, F, 0x14) : 0;
    u64int base = ((u64int)bar1 << 32) | (bar0 & ~0xFULL);
    if (!base) { lg("XHCI: BAR0 not assigned by firmware\n"); return 0; }

    /* размер BAR (при выключенном декодировании) */
    u32int cmdreg = pci_rd(B, S, F, 0x04) & 0xFFFF;
    pci_wr(B, S, F, 0x04, cmdreg & ~7u);
    pci_wr(B, S, F, 0x10, 0xFFFFFFFFu);
    u32int m0 = pci_rd(B, S, F, 0x10);
    u32int m1 = 0;
    if (is64) { pci_wr(B, S, F, 0x14, 0xFFFFFFFFu); m1 = pci_rd(B, S, F, 0x14); }
    pci_wr(B, S, F, 0x10, bar0);
    if (is64) pci_wr(B, S, F, 0x14, bar1);
    u64int mask = ((u64int)(is64 ? m1 : 0xFFFFFFFFu) << 32) | (m0 & ~0xFULL);
    u64int bsize = (~mask) + 1;
    if (!is64) bsize &= 0xFFFFFFFFULL;
    if (bsize < 0x1000 || bsize > 0x100000) bsize = 0x10000;

    /* Intel Panther/Lynx Point: перекинуть порты с EHCI на xHCI */
    if ((id & 0xFFFF) == 0x8086) {
        u32int did = id >> 16;
        if (did == 0x1E31 || did == 0x8C31 || did == 0x9C31 || did == 0x9CB1) {
            pci_wr(B, S, F, 0xD8, pci_rd(B, S, F, 0xDC));
            pci_wr(B, S, F, 0xD0, pci_rd(B, S, F, 0xD4));
            lg("XHCI: Intel ports routed to xHCI\n");
        }
    }

    /* mem space + bus master + INTx disable (мы опрашиваем) */
    pci_wr(B, S, F, 0x04, (cmdreg | 0x6u | (1u << 10)) & 0xFFFF);

    void *mm = map_mmio(base, bsize);
    if (!mm) { lg("XHCI: cannot map MMIO\n"); return 0; }
    cap = (volatile u8int *)mm;

    u32int caplen = cap[0];
    u32int hciver = rd32(cap, 0) >> 16;
    u32int hcs1 = rd32(cap, 4), hcs2 = rd32(cap, 8), hcc1 = rd32(cap, 0x10);
    lg("XHCI: BAR "); lgh(base); lg(" size "); lgh(bsize);
    lg(" ver "); lgh(hciver); lg("\n");
    if (hciver == 0xFFFF || caplen < 0x10) { lg("XHCI: bad capability regs\n"); return 0; }

    max_slots = hcs1 & 0xFF;
    max_ports = hcs1 >> 24;
    if (max_ports > MAX_PORTS) max_ports = MAX_PORTS;
    ac64 = hcc1 & 1;
    csz  = (hcc1 & 4) ? 64 : 32;
    db_off = rd32(cap, 0x14) & ~3u;
    rt_off = rd32(cap, 0x18) & ~0x1Fu;
    op  = cap + caplen;
    rt  = cap + rt_off;
    dbr = cap + db_off;
    if (db_off + 4 * (max_slots + 1) > bsize || rt_off + 0x40 > bsize) {
        lg("XHCI: register offsets outside BAR\n"); return 0;
    }
    lg("XHCI: slots "); lgd(max_slots); lg(" ports "); lgd(max_ports);
    lg(" csz "); lgd(csz); lg(" ac64 "); lgd(ac64); lg("\n");

    parse_ext_caps(hcc1);

    /* --- остановить и сбросить --- */
    if (!(rd32(op, USBSTS) & STS_HCH)) {
        wr32(op, USBCMD, rd32(op, USBCMD) & ~CMD_RS);
        if (!wait_sts(STS_HCH, STS_HCH, 200)) lg("XHCI: halt timeout\n");
    }
    if (!wait_sts(STS_CNR, 0, 1000)) lg("XHCI: CNR stuck before reset\n");
    wr32(op, USBCMD, CMD_HCRST);
    {
        int ok = 0;
        for (int i = 0; i < 3000; i++) {
            if (!(rd32(op, USBCMD) & CMD_HCRST) && !(rd32(op, USBSTS) & STS_CNR)) { ok = 1; break; }
            mdelay(1);
        }
        if (!ok) { lg("XHCI: HCRST timeout\n"); return 0; }
    }
    mdelay(50);

    if (max_slots > MAX_SLOTS_USED) max_slots = MAX_SLOTS_USED;
    wr32(op, CONFIG, max_slots);

    /* --- DCBAA + scratchpad --- */
    dcbaa = (u64int *)dma_page(&dcbaa_phys);
    if (!dcbaa) return 0;
    {
        u32int nsp = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
        if (nsp) {
            lg("XHCI: scratchpad pages "); lgd(nsp); lg("\n");
            if (nsp > 512) { lg("XHCI: too many scratchpads\n"); return 0; }
            u64int arr_phys;
            u64int *arr = (u64int *)dma_page(&arr_phys);
            if (!arr) return 0;
            for (u32int i = 0; i < nsp; i++) {
                u64int pp;
                if (!dma_page(&pp)) return 0;
                arr[i] = pp;
            }
            dcbaa[0] = arr_phys;
        }
    }
    wr32(op, DCBAAP, (u32int)dcbaa_phys);
    wr32(op, DCBAAP + 4, (u32int)(dcbaa_phys >> 32));

    /* --- command ring --- */
    {
        u64int cp;
        void *cm = dma_page(&cp);
        if (!cm) return 0;
        ring_init(&cmdr, cm, cp, CMD_N);
        wr32(op, CRCR, (u32int)cp | 1);           /* RCS = 1 */
        wr32(op, CRCR + 4, (u32int)(cp >> 32));
    }

    /* --- event ring (interrupter 0) --- */
    {
        u64int erst_phys;
        u32int *erst = (u32int *)dma_page(&erst_phys);
        evr = (trb_t *)dma_page(&ev_phys);
        if (!erst || !evr) return 0;
        erst[0] = (u32int)ev_phys;
        erst[1] = (u32int)(ev_phys >> 32);
        erst[2] = EV_N;
        erst[3] = 0;
        wr32(rt, 0x20 + 0x08, 1);                           /* ERSTSZ */
        wr32(rt, 0x20 + 0x18, (u32int)ev_phys | (1u << 3)); /* ERDP   */
        wr32(rt, 0x20 + 0x1C, (u32int)(ev_phys >> 32));
        wr32(rt, 0x20 + 0x10, (u32int)erst_phys);           /* ERSTBA */
        wr32(rt, 0x20 + 0x14, (u32int)(erst_phys >> 32));
        wr32(rt, 0x20 + 0x00, 3);                           /* IMAN: IP clear, IE */
        wr32(rt, 0x20 + 0x04, 0);                           /* IMOD */
    }

    ctl_buf = dma_page(&ctl_phys);
    if (!ctl_buf) return 0;

    /* --- Run --- (INTE = 0: только опрос) --- */
    wr32(op, USBCMD, CMD_RS);
    if (!wait_sts(STS_HCH, 0, 200)) { lg("XHCI: failed to start\n"); return 0; }

    xready = 1;
    lg("XHCI: controller running\n");

    /* Дать портам обнаружить устройства, затем просканировать. */
    mdelay(150);
    for (u32int p = 1; p <= max_ports; p++) {
        service();
        if (rd32(op, PORTSC(p)) & PS_CCS)
            handle_port(p);
        else
            port_ack(p, rd32(op, PORTSC(p)));
    }
    port_attention = 0;       /* уже обработали */
    late_scan_at = tick + 1500;

    lg("XHCI: init done, mice: "); lgd((u64int)(long long)mouse_cnt);
    lg(" keybs: "); lgd((u64int)(long long)kbd_cnt); lg("\n");
    return mouse_cnt + kbd_cnt;
}
