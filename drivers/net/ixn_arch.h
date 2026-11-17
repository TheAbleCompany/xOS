#ifndef IXN_ARCH_H
#define IXN_ARCH_H

#include "ixn_types.h"

#define IXN_CACHE_LINE 64u
#define IXN_PAGE_SHIFT 12u
#define IXN_PAGE_SIZE  (1u << IXN_PAGE_SHIFT)

/* x86_64 intrinsics эмулируются макросами: ядро собрано без SSE,
 * а clang<->gcc расходятся в объявлении _mm_* в заголовках. */
#define IXN_CPU_PAUSE() __asm__ volatile("pause" ::: "memory")
#define IXN_MFENCE()    __asm__ volatile("mfence" ::: "memory")
#define IXN_SFENCE()    __asm__ volatile("sfence" ::: "memory")
#define IXN_LFENCE()    __asm__ volatile("lfence" ::: "memory")

static inline void ixn_outl(ixn_u16 port, ixn_u32 data) {
    __asm__ volatile("outl %0, %1" : : "a"(data), "d"(port));
}
static inline ixn_u32 ixn_inl(ixn_u16 port) {
    ixn_u32 ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}
static inline void ixn_outw(ixn_u16 port, ixn_u16 data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "d"(port));
}
static inline ixn_u16 ixn_inw(ixn_u16 port) {
    ixn_u16 ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}
static inline void ixn_outb(ixn_u16 port, ixn_u8 data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "d"(port));
}
static inline ixn_u8 ixn_inb(ixn_u16 port) {
    ixn_u8 ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

static inline void ixn_io_wait(void) { ixn_outb(0x80u, 0u); }

static inline void ixn_pause(void) { IXN_CPU_PAUSE(); }
static inline void ixn_mfence(void) { IXN_MFENCE(); }
static inline void ixn_sfence(void) { IXN_SFENCE(); }

static inline void ixn_halt(void) { __asm__ volatile("hlt"); }
static inline void ixn_enable_irq(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void ixn_disable_irq(void) { __asm__ volatile("cli" ::: "memory"); }

static inline ixn_u64 ixn_irq_save(void) {
    ixn_u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void ixn_irq_restore(ixn_u64 flags) {
    if (flags & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

static inline ixn_u64 ixn_rdtsc(void) {
    ixn_u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((ixn_u64)hi << 32) | (ixn_u64)lo;
}

static inline void ixn_cpuid(ixn_u32 leaf, ixn_u32 subleaf,
                             ixn_u32 *a, ixn_u32 *b, ixn_u32 *c, ixn_u32 *d) {
    ixn_u32 ra, rb, rc, rd;
    __asm__ volatile("cpuid"
                     : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                     : "a"(leaf), "c"(subleaf));
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

static inline ixn_u32 ixn_atomic_cmpxchg32(volatile ixn_u32 *p, ixn_u32 old, ixn_u32 neu) {
    return __sync_val_compare_and_swap(p, old, neu);
}

static inline ixn_u32 ixn_atomic_xchg32(volatile ixn_u32 *p, ixn_u32 neu) {
    return __sync_lock_test_and_set(p, neu);
}

static inline ixn_u32 ixn_atomic_add32(volatile ixn_u32 *p, ixn_u32 v) {
    return __sync_fetch_and_add(p, v);
}

static inline ixn_u32 ixn_atomic_inc32(volatile ixn_u32 *p) {
    return ixn_atomic_add32(p, 1u) + 1u;
}

static inline ixn_u32 ixn_atomic_dec32(volatile ixn_u32 *p) {
    return ixn_atomic_add32(p, (ixn_u32)-1) - 1u;
}

static inline ixn_u32 ixn_atomic_or32(volatile ixn_u32 *p, ixn_u32 v) {
    return __sync_fetch_and_or(p, v);
}

static inline ixn_u32 ixn_atomic_and32(volatile ixn_u32 *p, ixn_u32 v) {
    return __sync_fetch_and_and(p, v);
}

typedef ixn_u64 ixn_irq_lock_t;

static inline ixn_u64 ixn_irq_lock_acquire(volatile ixn_irq_lock_t *lock) {
    (void)lock;
    return ixn_irq_save();
}

static inline void ixn_irq_lock_release(volatile ixn_irq_lock_t *lock, ixn_u64 flags) {
    (void)lock;
    ixn_irq_restore(flags);
}

static inline ixn_u32 ixn_rand_next(ixn_u32 *state) {
    ixn_u32 x = *state;
    if (x == 0u) x = 0x9E3779B9u;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    *state = x;
    return x;
}

static inline void ixn_mmio_write32(ixn_u8 volatile *base, ixn_u32 off, ixn_u32 val) {
    *(ixn_u32 volatile *)(base + off) = val;
}
static inline ixn_u32 ixn_mmio_read32(ixn_u8 volatile *base, ixn_u32 off) {
    return *(ixn_u32 volatile *)(base + off);
}
static inline void ixn_mmio_write16(ixn_u8 volatile *base, ixn_u32 off, ixn_u16 val) {
    *(ixn_u16 volatile *)(base + off) = val;
}
static inline ixn_u16 ixn_mmio_read16(ixn_u8 volatile *base, ixn_u32 off) {
    return *(ixn_u16 volatile *)(base + off);
}
static inline void ixn_mmio_write8(ixn_u8 volatile *base, ixn_u32 off, ixn_u8 val) {
    *(ixn_u8 volatile *)(base + off) = val;
}
static inline ixn_u8 ixn_mmio_read8(ixn_u8 volatile *base, ixn_u32 off) {
    return *(ixn_u8 volatile *)(base + off);
}
static inline void ixn_mmio_flush(ixn_u8 volatile *base) {
    IXN_MFENCE();
    (void)*(ixn_u32 volatile *)(base);
}

static inline int ixn_pci_read_config_dword(ixn_u8 bus, ixn_u8 slot, ixn_u8 func,
                                            ixn_u8 offset, ixn_u32 *out) {
    ixn_u32 addr = (1u << 31) | ((ixn_u32)bus << 16) | ((ixn_u32)slot << 11) |
                   ((ixn_u32)func << 8) | ((ixn_u32)offset & 0xFCu);
    ixn_outl(0xCF8, addr);
    *out = ixn_inl(0xCFC);
    return 1;
}

static inline int ixn_pci_read_config_word(ixn_u8 bus, ixn_u8 slot, ixn_u8 func,
                                           ixn_u8 offset, ixn_u16 *out) {
    ixn_u32 v;
    if (!ixn_pci_read_config_dword(bus, slot, func, (ixn_u8)(offset & 0xFCu), &v)) return 0;
    *out = (ixn_u16)(v >> ((offset & 2u) * 8u));
    return 1;
}

static inline int ixn_pci_read_config_byte(ixn_u8 bus, ixn_u8 slot, ixn_u8 func,
                                           ixn_u8 offset, ixn_u8 *out) {
    ixn_u32 v;
    if (!ixn_pci_read_config_dword(bus, slot, func, (ixn_u8)(offset & 0xFCu), &v)) return 0;
    *out = (ixn_u8)(v >> ((offset & 3u) * 8u));
    return 1;
}

static inline void ixn_pci_write_config_dword(ixn_u8 bus, ixn_u8 slot, ixn_u8 func,
                                              ixn_u8 offset, ixn_u32 val) {
    ixn_u32 addr = (1u << 31) | ((ixn_u32)bus << 16) | ((ixn_u32)slot << 11) |
                   ((ixn_u32)func << 8) | ((ixn_u32)offset & 0xFCu);
    ixn_outl(0xCF8, addr);
    ixn_outl(0xCFC, val);
}

static inline void ixn_pci_write_config_word(ixn_u8 bus, ixn_u8 slot, ixn_u8 func,
                                             ixn_u8 offset, ixn_u16 val) {
    ixn_u32 addr = (1u << 31) | ((ixn_u32)bus << 16) | ((ixn_u32)slot << 11) |
                   ((ixn_u32)func << 8) | ((ixn_u32)offset & 0xFCu);
    ixn_u32 shift, mask, cur;
    ixn_outl(0xCF8, addr);
    cur = ixn_inl(0xCFC);
    shift = (ixn_u32)(offset & 2u) * 8u;
    mask = 0xFFFFu << shift;
    ixn_outl(0xCFC, (cur & ~mask) | ((ixn_u32)val << shift));
}

static inline void ixn_pci_write_config_byte(ixn_u8 bus, ixn_u8 slot, ixn_u8 func,
                                             ixn_u8 offset, ixn_u8 val) {
    ixn_u32 addr = (1u << 31) | ((ixn_u32)bus << 16) | ((ixn_u32)slot << 11) |
                   ((ixn_u32)func << 8) | ((ixn_u32)offset & 0xFCu);
    ixn_u32 shift, mask, cur;
    ixn_outl(0xCF8, addr);
    cur = ixn_inl(0xCFC);
    shift = (ixn_u32)(offset & 3u) * 8u;
    mask = 0xFFu << shift;
    ixn_outl(0xCFC, (cur & ~mask) | ((ixn_u32)val << shift));
}

#endif
