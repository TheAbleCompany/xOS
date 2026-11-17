#ifndef IXG_COMPAT_H
#define IXG_COMPAT_H

#include "ixg_types.h"
#include "ixg_platform.h"
#include <string.h>
#include <stdlib.h>

#define ix_memset memset
#define ix_memcpy memcpy
#define ix_memmove memmove
#define ix_strlen(s) ((ix_u32)strlen(s))
#define ix_strncpy strncpy
#define ix_strcmp(a,b) strcmp((a),(b))
#define ix_strstr strstr
#define ix_strcpy strcpy

static inline void ixk_video_write(const char *s) { (void)s; }
static inline void ixk_video_write_line(const char *s) { (void)s; }
static inline void ixk_video_write_format(const char *fmt, ...) { (void)fmt; }
static inline void ixk_video_write_hex64(ix_u64 v) { (void)v; }

static inline void ixk_outl(ix_u16 port, ix_u32 data) {
    __asm__ volatile("outl %0, %1" : : "a"(data), "d"(port));
}
static inline ix_u32 ixk_inl(ix_u16 port) {
    ix_u32 ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}
static inline void ixk_outb(ix_u16 port, ix_u8 data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "d"(port));
}
static inline ix_u8 ixk_inb(ix_u16 port) {
    ix_u8 ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

static inline int ixk_pci_read_config_dword(ix_u8 bus, ix_u8 slot, ix_u8 func, ix_u8 offset, ix_u32 *out) {
    ix_u32 addr = (1u << 31) | ((ix_u32)bus << 16) | ((ix_u32)slot << 11) | ((ix_u32)func << 8) | (offset & 0xFCu);
    ixk_outl(0xCF8, addr);
    *out = ixk_inl(0xCFC);
    return 1;
}
static inline int ixk_pci_read_config_word(ix_u8 bus, ix_u8 slot, ix_u8 func, ix_u8 offset, ix_u16 *out) {
    ix_u32 v;
    if (!ixk_pci_read_config_dword(bus, slot, func, offset & 0xFCu, &v)) return 0;
    *out = (ix_u16)(v >> ((offset & 2u) * 8u));
    return 1;
}

static inline ix_u64 ixk_pit_ticks(void) {
    ix_u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((ix_u64)hi << 32) | lo;
}

static inline void *ixm_mngr_alloc(ix_u32 pid, ix_u64 size, ix_u32 align, ix_u32 flags) {
    (void)pid; (void)align; (void)flags;
    return malloc((size_t)size);
}
static inline void ixm_mngr_free(ix_u32 pid, void *ptr) {
    (void)pid;
    free(ptr);
}
static inline int ixm_is_user_range(ix_u64 addr, ix_u32 size) {
    (void)addr; (void)size;
    return 1;
}
static inline int ixm_mngr_validate(ix_u32 pid, ix_u64 addr, ix_u32 size, ix_u32 mode) {
    (void)pid; (void)addr; (void)size; (void)mode;
    return 1;
}
static inline ix_u64 ixm_virt_to_phys(ix_u64 vaddr) {
    return vaddr;
}

#define IXM_MNGR_READ 1
#define IXM_MNGR_WRITE 2
#define IXM_MNGR_HEAP 4

#define IX_IOMMU_DOMAIN_GPU 1
#define IX_IOMMU_FLAG_READ 1
#define IX_IOMMU_FLAG_WRITE 2
#define IX_IOMMU_FLAG_DEVICE 4

static inline int ix_iommu_register_dma(ix_u32 domain, ix_u64 phys, ix_u32 size, ix_u32 flags) {
    (void)domain; (void)phys; (void)size; (void)flags;
    return 0;
}
static inline int ix_iommu_unregister_dma(ix_u32 domain, ix_u64 phys, ix_u32 size) {
    (void)domain; (void)phys; (void)size;
    return 0;
}
static inline int ix_iommu_assert_safe(ix_u32 domain, ix_u64 addr, ix_u32 size, ix_u32 flags) {
    (void)domain; (void)addr; (void)size; (void)flags;
    return 1;
}

static inline const char *ix_reg_read(const char *key) {
    (void)key;
    return 0;
}
static inline void ix_reg_write(const char *key, const char *val) {
    (void)key; (void)val;
}

typedef ix_u64 ixk_irq_lock_t;
static inline ix_u64 ixk_irq_lock_acquire(volatile ixk_irq_lock_t *lock) {
    (void)lock;
    ix_u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
    return flags;
}
static inline void ixk_irq_lock_release(volatile ixk_irq_lock_t *lock, ix_u64 flags) {
    (void)lock;
    if (flags & 0x200) __asm__ volatile("sti");
}

typedef struct {
    ix_u8 *data;
    ix_u32 used;
    ix_u32 current_cap;
} ix_dlimit_t;

#define IX_DLIMIT_FLAG_ZERO_INIT 1
#define IX_DLIMIT_FLAG_PAGE_ALLOC 2
#define IX_DLIMIT_FLAG_NO_SHRINK 4

static inline void ix_dlimit_init(ix_dlimit_t *d, ix_u32 initial, ix_u32 max_cap, ix_u32 elem_size, ix_u32 flags) {
    (void)flags;
    d->data = (ix_u8*)malloc((size_t)initial * elem_size);
    d->used = 0;
    d->current_cap = initial;
}
static inline void ix_dlimit_init_ex(ix_dlimit_t *d, ix_u32 initial, ix_u32 max_cap, ix_u32 elem_size, ix_u32 flags, ix_u32 a, ix_u32 b) {
    (void)flags; (void)a; (void)b;
    d->data = (ix_u8*)malloc((size_t)initial * elem_size);
    d->used = 0;
    d->current_cap = initial;
}
static inline int ix_dlimit_reserve(ix_dlimit_t *d, ix_u32 count) {
    d->used += count;
    return 1;
}
static inline void ix_dlimit_free(ix_dlimit_t *d) {
    if (d->data) free(d->data);
    d->data = 0; d->used = 0; d->current_cap = 0;
}

static inline int ix_page_blob_store(void *data, ix_u32 size, ix_u32 *out_id) {
    (void)data; (void)size;
    if (out_id) *out_id = 0;
    return -1;
}
static inline int ix_page_blob_release(ix_u32 id) {
    (void)id;
    return 0;
}

static inline int ixfs_mkdir(const char *path) {
    (void)path;
    return 0;
}
static inline int ixfs_stat(const char *path, void *info) {
    (void)path; (void)info;
    return -1;
}
static inline int ixfs_open(const char *path, ix_u32 flags) {
    (void)path; (void)flags;
    return -1;
}
static inline int ixfs_read(int fd, void *buf, ix_u32 size) {
    (void)fd; (void)buf; (void)size;
    return -1;
}
static inline int ixfs_write(int fd, const void *buf, ix_u32 size) {
    (void)fd; (void)buf; (void)size;
    return -1;
}
static inline void ixfs_close(int fd) {
    (void)fd;
}

#define IX_FS_READ 1
#define IX_FS_WRITE 2
#define IX_FS_CREATE 4
#define IX_FS_TRUNC 8

typedef struct {
    ix_u32 size;
} ix_file_info_t;

typedef struct {
    ix_u32 magic;
    ix_u64 e820_ptr;
    ix_u32 e820_count;
} IxBootInfo;

#define IX_BOOTINFO_MAGIC 0x49584249u

static const IxBootInfo *g_ix_bootinfo = 0;

typedef struct {
    ix_u64 base;
    ix_u64 size;
    ix_u32 type;
} IxE820Entry;

typedef struct {
    ix_u32 tier;
} ix_simd_caps_t;

#define IX_SIMD_TIER_SSE2 2
#define IX_SIMD_TIER_AVX_FMA 3
#define IX_SIMD_TIER_AVX2 4
#define IX_SIMD_TIER_AVX512 5

static inline void ix_simd_get_caps(ix_simd_caps_t *caps) {
    caps->tier = IX_SIMD_TIER_SSE2;
}
static inline void ix_simd_memcpy(void *dst, const void *src, ix_size n) {
    memcpy(dst, src, n);
}
static inline void ix_simd_add_u32(ix_u32 *dst, const ix_u32 *a, const ix_u32 *b, ix_u32 count) {
    for (ix_u32 i = 0; i < count; i++) dst[i] = a[i] + b[i];
}
static inline void ix_simd_saxpy_f32(float *out, const float *in0, const float *in1, ix_u32 count, float s0, float s1) {
    for (ix_u32 i = 0; i < count; i++) out[i] = s0 * in0[i] + s1 * in1[i];
}

typedef struct {
    ix_u32 body_len;
} ix_http_response_t;

static inline int ix_http_get(const char *url, ix_u8 *buf, ix_u32 cap, ix_http_response_t *resp) {
    (void)url; (void)buf; (void)cap;
    resp->body_len = 0;
    return 0;
}

typedef struct {
    ix_u32 dx_level_major;
    ix_u32 feature_flags;
} ixg_driver_feature_caps_t;

static inline int ixg_driver_get_feature_caps(ixg_driver_feature_caps_t *out) {
    memset(out, 0, sizeof(*out));
    return 0;
}

static inline void ixg_driver_hw_set_texture_stage(ix_u8 a, ix_u8 b, ix_u8 c, ix_u8 d, ix_u8 e, ix_u8 f, ix_u8 g) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;
}
static inline void ixg_driver_hw_set_transform_constants(const float *p, ix_u32 c) {
    (void)p;(void)c;
}
static inline void ixg_driver_hw_set_material(const float *a, const float *b, const float *c, const float *d, float e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
}
static inline void ixg_driver_hw_set_render_state(const ix_u32 *p, ix_u32 c) {
    (void)p;(void)c;
}

static inline void ixg2d_runtime_tune_set(ix_u32 v) { (void)v; }
static inline void ixg3d_runtime_tune_set(ix_u32 v) { (void)v; }

typedef struct {
    ix_u32 status;
} ix_uha_status_t;

static inline int ixg_driver_get_uha_status(ix_uha_status_t *out) {
    memset(out, 0, sizeof(*out));
    return 0;
}

#endif