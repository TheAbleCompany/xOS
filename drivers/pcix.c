/*
 * drivers/pcix.c -- см. pcix.h.
 */

#include "pcix.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

/* ------------------------------------------------------------------ */
/*  Конфигурационное пространство                                      */
/* ------------------------------------------------------------------ */

static u32int cfg_addr(u8int bus, u8int dev, u8int fn, u32int off)
{
    return 0x80000000u | ((u32int)bus << 16) | ((u32int)(dev & 31) << 11) |
           ((u32int)(fn & 7) << 8) | (off & 0xFCu);
}

static u32int cfg_rd32(u8int bus, u8int dev, u8int fn, u32int off)
{
    outl(PCI_ADDR, cfg_addr(bus, dev, fn, off));
    return inl(PCI_DATA);
}

static void cfg_wr32(u8int bus, u8int dev, u8int fn, u32int off, u32int v)
{
    outl(PCI_ADDR, cfg_addr(bus, dev, fn, off));
    outl(PCI_DATA, v);
}

u32int pcix_read32(const pcix_dev_t *d, u32int off)
{
    return cfg_rd32(d->bus, d->dev, d->fn, off);
}

u16int pcix_read16(const pcix_dev_t *d, u32int off)
{
    return (u16int)(pcix_read32(d, off & ~3u) >> ((off & 2u) * 8u));
}

void pcix_write32(const pcix_dev_t *d, u32int off, u32int v)
{
    cfg_wr32(d->bus, d->dev, d->fn, off, v);
}

void pcix_write16(const pcix_dev_t *d, u32int off, u16int v)
{
    u32int sh  = (off & 2u) * 8u;
    u32int cur = pcix_read32(d, off & ~3u);
    cur = (cur & ~(0xFFFFu << sh)) | ((u32int)v << sh);
    pcix_write32(d, off & ~3u, cur);
}

/* ------------------------------------------------------------------ */
/*  Обход                                                              */
/* ------------------------------------------------------------------ */

static u8int g_bus_seen[256];
static int   g_visited;

static int scan_bus(u8int bus, pcix_visit_fn fn, void *ctx, int depth)
{
    if (g_bus_seen[bus] || depth > 16)
        return 0;
    g_bus_seen[bus] = 1;

    for (u32int dev = 0; dev < 32; dev++) {
        u32int id0 = cfg_rd32(bus, (u8int)dev, 0, 0x00);
        if ((id0 & 0xFFFFu) == 0xFFFFu)
            continue;

        u32int hdr0 = cfg_rd32(bus, (u8int)dev, 0, 0x0C);
        u32int nfn  = (hdr0 & 0x00800000u) ? 8u : 1u;

        for (u32int fnn = 0; fnn < nfn; fnn++) {
            u32int id = cfg_rd32(bus, (u8int)dev, (u8int)fnn, 0x00);
            if ((id & 0xFFFFu) == 0xFFFFu)
                continue;

            u32int cls = cfg_rd32(bus, (u8int)dev, (u8int)fnn, 0x08);
            u32int hdr = cfg_rd32(bus, (u8int)dev, (u8int)fnn, 0x0C);

            pcix_dev_t d;
            d.bus = bus; d.dev = (u8int)dev; d.fn = (u8int)fnn;
            d.vendor = (u16int)(id & 0xFFFFu);
            d.device = (u16int)(id >> 16);
            d.rev        = (u8int)(cls & 0xFF);
            d.prog_if    = (u8int)((cls >> 8) & 0xFF);
            d.sub_class  = (u8int)((cls >> 16) & 0xFF);
            d.base_class = (u8int)((cls >> 24) & 0xFF);
            d.hdr_type   = (u8int)((hdr >> 16) & 0x7F);

            g_visited++;
            if (fn && fn(&d, ctx))
                return 1;

            /* PCI-PCI / CardBus мост: идём на secondary-шину. */
            if (d.base_class == 0x06 && d.sub_class == 0x04 &&
                d.hdr_type == 0x01) {
                u32int busnum = cfg_rd32(bus, (u8int)dev, (u8int)fnn, 0x18);
                u8int  sec = (u8int)((busnum >> 8) & 0xFF);
                if (sec != 0 && sec != bus) {
                    if (scan_bus(sec, fn, ctx, depth + 1))
                        return 1;
                }
            }
        }
    }
    return 0;
}

int pcix_enum(pcix_visit_fn fn, void *ctx)
{
    for (int i = 0; i < 256; i++)
        g_bus_seen[i] = 0;
    g_visited = 0;

    scan_bus(0, fn, ctx, 0);

    /*
     * Дополнительные корневые сегменты (многосокетные серверы) требуют
     * ACPI MCFG/_CRS; перебор всех 256 шин вслепую на части чипсетов
     * небезопасен, поэтому здесь только иерархия через мосты.
     */
    return g_visited;
}

/* ------------------------------------------------------------------ */
/*  BAR / команды                                                      */
/* ------------------------------------------------------------------ */

int pcix_bar(const pcix_dev_t *d, int n, u64int *base, int *is_io, int *is64)
{
    if (n < 0 || n > 5)
        return 0;

    u32int lo = pcix_read32(d, 0x10u + (u32int)n * 4u);
    if (lo == 0 || lo == 0xFFFFFFFFu)
        return 0;

    if (is_io)  *is_io = (lo & 1u) ? 1 : 0;
    if (is64)   *is64  = 0;

    if (lo & 1u) {                      /* I/O BAR */
        if (base) *base = lo & ~3u;
        return 1;
    }

    u64int b = lo & ~0xFu;              /* биты 3:0 -- флаги, не адрес */
    if (((lo >> 1) & 3u) == 2u && n < 5) {   /* 64-bit memory BAR */
        u32int hi = pcix_read32(d, 0x10u + (u32int)(n + 1) * 4u);
        b |= (u64int)hi << 32;
        if (is64) *is64 = 1;
    }
    if (base) *base = b;
    return b != 0;
}

void pcix_enable(const pcix_dev_t *d, u16int flags)
{
    u16int cmd = pcix_read16(d, 0x04);
    cmd |= flags;
    pcix_write16(d, 0x04, cmd);
}

/* ------------------------------------------------------------------ */
/*  MMIO                                                               */
/* ------------------------------------------------------------------ */

void *pcix_map_mmio(u64int phys, u64int size)
{
    return vmm_map_mmio(phys, size);
}

/* ------------------------------------------------------------------ */
/*  DMA                                                                */
/* ------------------------------------------------------------------ */

void *pcix_dma_alloc(u64int pages, int below4g, u64int *phys)
{
    u64int p;

    if (pages == 0)
        return 0;

    p = below4g ? pmm_alloc_pages_below(pages, 0x100000000ULL)
                : pmm_alloc_pages(pages);

    if (!p)
        return 0;

    void *v = vmm_phys_to_virt(p);
    memset((u8int *)v, 0, (u32int)(pages * 4096ULL));
    if (phys) *phys = p;
    return v;
}

void pcix_dma_free(void *virt, u64int phys, u64int pages)
{
    (void)virt;
    if (phys)
        pmm_free_pages(phys, pages);
}

/* ------------------------------------------------------------------ */
/*  Задержки                                                           */
/* ------------------------------------------------------------------ */

/*
 * Скорость outb(0x80) сильно зависит от чипсета (LPC/eSPI, 0.2..3 мкс),
 * поэтому таймауты на ней "плавали".  Калибруем TSC по каналу 2 PIT
 * (10 мс) и считаем задержки по TSC; если калибровка не удалась --
 * запасной вариант на порту 0x80.
 */
static u64int g_tsc_per_us = 0;
static int    g_delay_ready = 0;

static inline u64int rd_tsc(void)
{
    u32int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64int)hi << 32) | lo;
}

static void delay_calibrate(void)
{
    g_delay_ready = 1;

    u8int p61 = inb(0x61);
    outb(0x61, (u8int)((p61 & 0xFC) | 0x01));   /* gate канала 2 вкл, динамик выкл */
    outb(0x43, 0xB0);                           /* ch2, lo/hi, режим 0, binary */
    outb(0x42, (u8int)(11932 & 0xFF));          /* 11932 / 1.193182 МГц ~ 10 мс */
    outb(0x42, (u8int)(11932 >> 8));

    u64int t0 = rd_tsc();
    u32int guard = 0;
    while (!(inb(0x61) & 0x20)) {               /* OUT2 -> 1 по окончании счёта */
        if (++guard > 20000000u) break;
    }
    u64int t1 = rd_tsc();
    outb(0x61, p61);

    if (guard <= 20000000u) {
        u64int per_us = (t1 - t0) / 10000u;
        if (per_us >= 100 && per_us <= 20000)   /* 100 МГц .. 20 ГГц */
            g_tsc_per_us = per_us;
    }

    debug_puts("[PCIX] delay source: ");
    if (g_tsc_per_us) {
        debug_puts("TSC, ");
        debug_putnum(g_tsc_per_us);
        debug_puts(" MHz\n");
    } else {
        debug_puts("port 0x80 (TSC calibration failed)\n");
    }
}

void pcix_udelay(u32int us)
{
    if (!g_delay_ready)
        delay_calibrate();

    if (g_tsc_per_us) {
        u64int end = rd_tsc() + (u64int)us * g_tsc_per_us;
        while (rd_tsc() < end)
            __asm__ volatile("pause");
        return;
    }
    while (us--)
        outb(0x80, 0);
}

void pcix_mdelay(u32int ms)
{
    while (ms--)
        pcix_udelay(1000);
}

void pcix_puthex(u64int v)
{
    static const char hx[] = "0123456789abcdef";
    char buf[17];
    int  n = 0;
    do { buf[n++] = hx[v & 15]; v >>= 4; } while (v);
    debug_putc('0'); debug_putc('x');
    while (n) debug_putc(buf[--n]);
}

void pcix_puthexw(u64int v, int width)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = width - 1; i >= 0; i--)
        debug_putc(hx[(v >> (i * 4)) & 15]);
}
