#include "ixg_driver_internal.h"
#include "ixg_compat.h"
#include "ixg_mutants.h"

#if defined(__GNUC__)
#define IXG_DRIVER_ALIGNED(x) __attribute__((aligned(x)))
#else
#define IXG_DRIVER_ALIGNED(x) __declspec(align(x))
#endif

#define IXG_DRIVER_DMA_DEFAULT_MASK_BITS 40u
#define IXG_DRIVER_DMA_DEFAULT_APERTURE_MB 512u
#define IXG_DRIVER_DMA_PAGE_SIZE 4096u
#define IXG_DRIVER_DMA_MAX_PTES 65536u
#define IXG_DRIVER_DMA_SANDBOX_BYTES (1u * 1024u * 1024u)
#define IXG_DRIVER_DMA_SANDBOX_ALIGN 64u
#define IXG_DRIVER_DMA_SANDBOX_IOVA_BASE 0x40000000ull
#define IXG_DRIVER_DMA_DESC_STAGE_BYTES 256u
#define IXG_DRIVER_DMA_SANDBOX_MIN_MB 16u
#define IXG_DRIVER_DMA_SANDBOX_MAX_MB 1024u
#define IXG_DRIVER_DMA_SANDBOX_RATIO_SHIFT 4u
#define IXG_DRIVER_DMA_PTE_VALID (1ull << 0)
#define IXG_DRIVER_DMA_PTE_WRITE (1ull << 1)
#define IXG_DRIVER_DMA_PTE_SNOOP (1ull << 2)

static ixg_hw_ops_t* g_hw_ops = 0;
static uint32_t g_hw_detected = 0;
static uint32_t g_hw_has_mutant = 0;
static uint32_t g_driver_initialized = 0;
static ixg_driver_caps_t g_driver_caps;
ixg_driver_hw_state_t g_ixg_driver_hw;
ixg_driver_gpu_cmd_t g_ixg_driver_cmd_ring[IXG_DRIVER_CMD_RING_CAP];
IXG_DRIVER_ALIGNED(4096) ix_u64 g_ixg_driver_dma_ptes[IXG_DRIVER_DMA_MAX_PTES];
IXG_DRIVER_ALIGNED(4096) static ix_u64 g_ixg_driver_dma_l0[512u];
IXG_DRIVER_ALIGNED(4096) static ix_u64 g_ixg_driver_dma_l1[512u];
IXG_DRIVER_ALIGNED(4096) static ix_u64 g_ixg_driver_dma_l2[512u];
IXG_DRIVER_ALIGNED(4096) static ix_u8 g_ixg_driver_dma_sandbox[IXG_DRIVER_DMA_SANDBOX_BYTES];
ix_u8* g_ixg_driver_dma_sandbox_ptr = g_ixg_driver_dma_sandbox;
ix_u32 g_ixg_driver_dma_sandbox_bytes = IXG_DRIVER_DMA_SANDBOX_BYTES;
static ixg_hw_ops_t g_ixg_driver_vendor_ops;
static ix_u32 g_ixg_driver_rescue_blob_id = 0u;
static ix_u32 g_ixg_driver_rescue_counter = 0u;
static ix_u32 g_ixg_driver_dma_sandbox_head = IXG_DRIVER_DMA_PAGE_SIZE;

void ixg_backend_nvidia_init_ops(ixg_hw_ops_t* out_ops);
void ixg_backend_nvidia_post_init(void);
void ixg_backend_amd_init_ops(ixg_hw_ops_t* out_ops);
void ixg_backend_amd_post_init(void);
void ixg_backend_intel_init_ops(ixg_hw_ops_t* out_ops);
void ixg_backend_intel_post_init(void);

static const ixg_driver_profile_t g_ixg_driver_profiles[] = {
  {0x10DEu, 0x0008u, 0x00FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0x00000040u, 0x00000044u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000040u, 0x00000044u, 0x00000048u, 0x00000000u},
   "nvidia_legacy_nv1_tnt_mmio"},
  {0x10DEu, 0x0100u, 0x02FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_celsius_kelvin_rankine_bar"},
  {0x10DEu, 0x0300u, 0x0BFFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_NVIDIA, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_tesla_bar"},
  {0x10DEu, 0x0D00u, 0x0FBFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_NVIDIA, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_fermi_bar"},
  {0x10DEu, 0x0FC0u, 0x11FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_NVIDIA, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_kepler_bar"},
  {0x10DEu, 0x1200u, 0x17FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_NVIDIA, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_maxwell_bar"},
  {0x10DEu, 0x1B00u, 0x1DFFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_NVIDIA, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_pascal_bar"},
  {0x10DEu, 0x1E00u, 0x2FFFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_NVIDIA, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_modern_bar"},
  {0x10DEu, 0x2900u, 0x33FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_NVIDIA, 0u, 0u,
   {0x00003240u, 0x00003244u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00003248u, 0x00000000u, 0x00001000u, 0x00003200u},
   "nvidia_blackwell_bar"},
  {0x10DEu, 0x0FC0u, 0x2FFFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_NVIDIA, 0u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0x00000040u, 0x00000044u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0000008Cu, 0x00000088u, 0x00000000u, 0x00000048u},
   "nvidia_userd_gpfifo"},
  {0x1002u, 0x6600u, 0x6FFFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_AMD, 0u, (ix_u8)IXG_DRIVER_DBELL_TAIL, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00009000u, 0x00009004u, 0x0000A000u, 0x00009010u},
   "amd_gcn_bar"},
  {0x1002u, 0x7300u, 0x737Fu, (ix_u8)IXG_DRIVER_VENDOR_PATH_AMD, 0u, (ix_u8)IXG_DRIVER_DBELL_TAIL, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0001C000u, 0x0001C004u, 0x0001C008u, 0x0001C00Cu},
   "amd_rdna2_bar"},
  {0x1002u, 0x7400u, 0x75FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_AMD, 0u, (ix_u8)IXG_DRIVER_DBELL_TAIL, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0001C000u, 0x0001C004u, 0x0001C008u, 0x0001C00Cu},
   "amd_rdna3_bar"},
  {0x1002u, 0x7600u, 0x77FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_AMD, 0u, (ix_u8)IXG_DRIVER_DBELL_TAIL, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0001C000u, 0x0001C004u, 0x0001C008u, 0x0001C00Cu},
   "amd_rdna4_bar"},
  {0x1002u, 0x7380u, 0x741Fu, (ix_u8)IXG_DRIVER_VENDOR_PATH_AMD, 0u, (ix_u8)IXG_DRIVER_DBELL_TAIL, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0001C000u, 0x0001C004u, 0x0001C008u, 0x0001C00Cu},
   "amd_cdna_bar"},
  {0x1022u, 0x7300u, 0x75FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_AMD, 0u, (ix_u8)IXG_DRIVER_DBELL_TAIL, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0001C000u, 0x0001C004u, 0x0001C008u, 0x0001C00Cu},
   "amd_alt_rdna_bar"},
  {0x8086u, 0x1900u, 0x3FFFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_INTEL, 1u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00002038u, 0x00000000u, 0x0000203Cu, 0x00002030u, 0x00002034u, 0x00000000u, 0x00002040u},
   "intel_gen9_12_rcs"},
  {0x8086u, 0x4F80u, 0x56FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_INTEL, 1u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00005038u, 0x00000000u, 0x0000503Cu, 0x00005030u, 0x00005034u, 0x00000000u, 0x00005040u},
   "intel_arc_rcs"},
  {0x8086u, 0xD200u, 0xE4FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_INTEL, 1u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00005038u, 0x00000000u, 0x0000503Cu, 0x00005030u, 0x00005034u, 0x00000000u, 0x00005040u},
   "intel_battlemage_rcs"},
  {0x8086u, 0x7D00u, 0xA7FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_INTEL, 1u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0x00000000u, 0x00000000u, 0x00002038u, 0x00000000u, 0x0000203Cu, 0x00002030u, 0x00002034u, 0x00000000u, 0x00002040u},
   "intel_npu_accel_rcs"},
  {0x1234u, 0x1100u, 0x11FFu, (ix_u8)IXG_DRIVER_VENDOR_PATH_EXOTIC, 1u, (ix_u8)IXG_DRIVER_DBELL_OP, 0u, 0u,
   {0,0,0,0,0,0,0,0,0}, "qemu_bga_fifo"},
};

static ix_u32 ixg_pci_read(ix_u8 b, ix_u8 s, ix_u8 f, ix_u8 o) {
  ix_u32 addr = (1u << 31) | ((ix_u32)b << 16) | ((ix_u32)s << 11) | ((ix_u32)f << 8) | (o & 0xFCu);
  ixk_outl(0xCF8, addr);
  return ixk_inl(0xCFC);
}

static ix_u64 ixg_driver_detect_total_ram_bytes(void) {
  return 256ull * 1024ull * 1024ull;
}

void ixg_driver_dma_init_sandbox_pool(void) {
  ix_u64 ram_bytes;
  ix_u32 ram_mb;
  ix_u32 target_mb;
  ix_u32 target_bytes;
  void* mem;
  if (g_ixg_driver_dma_sandbox_ptr && g_ixg_driver_dma_sandbox_bytes > 0u &&
      g_ixg_driver_dma_sandbox_ptr != g_ixg_driver_dma_sandbox) {
    return;
  }
  ram_bytes = ixg_driver_detect_total_ram_bytes();
  ram_mb = (ix_u32)(ram_bytes >> 20u);
  if (ram_mb == 0u) {
    g_ixg_driver_dma_sandbox_ptr = g_ixg_driver_dma_sandbox;
    g_ixg_driver_dma_sandbox_bytes = IXG_DRIVER_DMA_SANDBOX_BYTES;
    return;
  }
  target_mb = ram_mb >> IXG_DRIVER_DMA_SANDBOX_RATIO_SHIFT;
  if (target_mb < IXG_DRIVER_DMA_SANDBOX_MIN_MB) target_mb = IXG_DRIVER_DMA_SANDBOX_MIN_MB;
  if (target_mb > IXG_DRIVER_DMA_SANDBOX_MAX_MB) target_mb = IXG_DRIVER_DMA_SANDBOX_MAX_MB;
  target_mb = ((target_mb + 15u) & ~15u);
  target_bytes = target_mb << 20u;
  if (target_bytes <= IXG_DRIVER_DMA_SANDBOX_BYTES) {
    g_ixg_driver_dma_sandbox_ptr = g_ixg_driver_dma_sandbox;
    g_ixg_driver_dma_sandbox_bytes = IXG_DRIVER_DMA_SANDBOX_BYTES;
    return;
  }
  mem = malloc(target_bytes);
  if (mem) {
    g_ixg_driver_dma_sandbox_ptr = (ix_u8*)mem;
    g_ixg_driver_dma_sandbox_bytes = target_bytes;
  } else {
    g_ixg_driver_dma_sandbox_ptr = g_ixg_driver_dma_sandbox;
    g_ixg_driver_dma_sandbox_bytes = IXG_DRIVER_DMA_SANDBOX_BYTES;
  }
}

ix_u64 ixg_driver_dma_stage_upload(const void* src, ix_u32 size_bytes) {
  ix_u32 aligned_size;
  ix_u32 offset;
  ix_u32 capacity;
  ix_u8* base;
  if (!src || size_bytes == 0u || !g_ixg_driver_hw.dma_sandbox_only) return 0u;
  ixg_driver_dma_init_sandbox_pool();
  base = g_ixg_driver_dma_sandbox_ptr ? g_ixg_driver_dma_sandbox_ptr : g_ixg_driver_dma_sandbox;
  capacity = g_ixg_driver_dma_sandbox_bytes ? g_ixg_driver_dma_sandbox_bytes : IXG_DRIVER_DMA_SANDBOX_BYTES;
  if (capacity <= IXG_DRIVER_DMA_PAGE_SIZE) return 0u;
  aligned_size = ((size_bytes + IXG_DRIVER_DMA_SANDBOX_ALIGN - 1u) & ~(IXG_DRIVER_DMA_SANDBOX_ALIGN - 1u));
  if (aligned_size == 0u || aligned_size > (capacity - IXG_DRIVER_DMA_PAGE_SIZE)) return 0u;
  offset = ((g_ixg_driver_dma_sandbox_head + IXG_DRIVER_DMA_SANDBOX_ALIGN - 1u) & ~(IXG_DRIVER_DMA_SANDBOX_ALIGN - 1u));
  if (offset < IXG_DRIVER_DMA_PAGE_SIZE || offset + aligned_size > capacity) offset = IXG_DRIVER_DMA_PAGE_SIZE;
  if (offset + aligned_size > capacity) return 0u;
  memcpy(base + offset, src, size_bytes);
  g_ixg_driver_dma_sandbox_head = offset + aligned_size;
  if (g_ixg_driver_dma_sandbox_head >= capacity) g_ixg_driver_dma_sandbox_head = IXG_DRIVER_DMA_PAGE_SIZE;
  return g_ixg_driver_hw.dma_aperture_base + (ix_u64)offset;
}

/*
 * ============================================================
 * CPU-visible render target ("readback" path)
 * ============================================================
 *
 * Буфер кадра в системной RAM, отображённый в GPU через те же
 * PTE DMA-aperture, что и staging-песочница.  GPU пишет кадр
 * сюда, CPU читает обратно обычным memcpy -- так пиксели окна
 * становятся настоящим GPU-выводом, а не SW-зеркалом.
 *
 * PTE-пространство: dma_pte_count (после песочницы) ..
 * IXG_DRIVER_DMA_MAX_PTES.  При переполнении аллокация
 * проваливается -> вызывающий бэкенд вернёт -1, и axgl
 * честно откатится на программный растеризатор.
 */

static ix_u8  *ixg_rt_alloc_base = 0;   /* как выделил malloc      */
static ix_u8  *ixg_rt_cpu        = 0;   /* выровнено на 4096       */
static ix_u64  ixg_rt_iova       = 0;
static ix_u32  ixg_rt_w          = 0;
static ix_u32  ixg_rt_h          = 0;
static ix_u32  ixg_rt_stride_px  = 0;
static ix_u32  ixg_rt_bytes      = 0;

#define IXG_RT_ALIGN 4096u
#define IXG_RT_MAX_BYTES (16u * 1024u * 1024u)

static ix_u64 ixg_driver_dma_map_persistent(const void* vaddr, ix_u32 bytes) {
  ix_u64 va;
  ix_u32 pages, i;
  if (!vaddr || bytes == 0u) return 0u;
  if (!g_ixg_driver_hw.dma_sandbox_only) return 0u;
  va = (ix_u64)(uintptr_t)vaddr;
  pages = (bytes + IXG_RT_ALIGN - 1u) / IXG_RT_ALIGN;
  if (pages == 0u) return 0u;
  if (g_ixg_driver_hw.dma_pte_count + pages > IXG_DRIVER_DMA_MAX_PTES) return 0u;
  for (i = 0u; i < pages; i++) {
    ix_u64 pv = va + (ix_u64)i * IXG_RT_ALIGN;
    ix_u64 phys = ixm_virt_to_phys(pv);
    ix_u32 idx = g_ixg_driver_hw.dma_pte_count + i;
    g_ixg_driver_dma_ptes[idx] =
        (phys & 0x000FFFFFFFFFF000ull) |
        IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE | IXG_DRIVER_DMA_PTE_SNOOP;
  }
  {
    ix_u64 iova = g_ixg_driver_hw.dma_aperture_base +
                  (ix_u64)g_ixg_driver_hw.dma_pte_count * IXG_RT_ALIGN;
    g_ixg_driver_hw.dma_pte_count += pages;
    ix_iommu_assert_safe(IX_IOMMU_DOMAIN_GPU, iova, bytes,
                         IX_IOMMU_FLAG_READ | IX_IOMMU_FLAG_WRITE);
    return iova;
  }
}

int ixg_driver_render_target_setup(ix_u32 w, ix_u32 h) {
  ix_u32 bytes;
  ix_u8* base;
  ix_u8* aligned;
  ix_u64 iova;
  if (!g_hw_detected) return -1;
  if (w == 0u || h == 0u) return -1;
  if ((ix_u64)w * (ix_u64)h * 4ull > (ix_u64)IXG_RT_MAX_BYTES) return -1;

  /* Идемпотентность: та же геометрия -- не трогаем ничего. */
  if (ixg_rt_cpu && ixg_rt_w == w && ixg_rt_h == h) return 0;

  bytes = w * h * 4u;
  base = malloc((ix_u32)bytes + IXG_RT_ALIGN);
  if (!base) return -1;
  aligned = (ix_u8*)(((uintptr_t)base + IXG_RT_ALIGN - 1u) & ~(uintptr_t)(IXG_RT_ALIGN - 1u));

  iova = ixg_driver_dma_map_persistent(aligned, bytes);
  if (!iova) {
    free(base);
    return -1;
  }

  /* Предыдущая цель освобождается только ПОСЛЕ успешного
   * маппинга новой (set_target может зваться на live-пайплайне). */
  if (ixg_rt_alloc_base) free(ixg_rt_alloc_base);

  ixg_rt_alloc_base = base;
  ixg_rt_cpu        = aligned;
  ixg_rt_iova       = iova;
  ixg_rt_w          = w;
  ixg_rt_h          = h;
  ixg_rt_stride_px  = w;
  ixg_rt_bytes      = bytes;
  memset(ixg_rt_cpu, 0, bytes);
  return 0;
}

void*  ixg_driver_render_target_cpu(void)      { return ixg_rt_cpu; }
ix_u64 ixg_driver_render_target_gpu_addr(void) { return ixg_rt_iova; }
ix_u32 ixg_driver_render_target_stride_px(void){ return ixg_rt_stride_px; }
ix_u32 ixg_driver_render_target_height(void)   { return ixg_rt_h; }

static void ixg_driver_init_dma_policy(ixg_driver_hw_state_t* hw) {
  if (!hw) return;
  hw->dma_mode = (ix_u8)IXG_DRIVER_DMA_MODE_AUTO;
  hw->gart_enabled = 1u;
  hw->iommu_enabled = 0u;
  hw->render_hw_only = 1u;
  hw->render_submit_mirror_3d = 0u;
  hw->dma_sandbox_only = 1u;
  hw->dma_mask_bits = IXG_DRIVER_DMA_DEFAULT_MASK_BITS;
  hw->dma_aperture_mb = IXG_DRIVER_DMA_DEFAULT_APERTURE_MB;
  hw->dma_aperture_base = IXG_DRIVER_DMA_SANDBOX_IOVA_BASE;
  if (hw->vendor_id == 0x8086u) {
    hw->iommu_enabled = 1u;
    hw->dma_mode = (ix_u8)IXG_DRIVER_DMA_MODE_IOMMU;
  } else if (hw->vendor_id == 0x10DEu || hw->vendor_id == 0x1002u || hw->vendor_id == 0x1022u) {
    hw->dma_mode = (ix_u8)IXG_DRIVER_DMA_MODE_GART;
  }
}

void ixg_driver_detect_pcie_caps(ix_u8 bus, ix_u8 slot, ix_u8 func, ix_u8* out_gen, ix_u8* out_lanes) {
  ix_u32 cap_ptr = 0u;
  ix_u32 cap_hdr;
  ix_u8 gen = 0u;
  ix_u8 lanes = 0u;
  ix_u32 cap_ttl = 48u;
  if (!out_gen || !out_lanes) return;
  *out_gen = 0u;
  *out_lanes = 0u;
  cap_ptr = ixg_pci_read(bus, slot, func, 0x34u);
  if (cap_ptr == 0xFFFFFFFFu || cap_ptr == 0u) return;
  cap_ptr &= 0xFCu;
  while (cap_ptr >= 0x40u && cap_ptr < 0x100u) {
    if (cap_ttl-- == 0u) return;
    cap_hdr = ixg_pci_read(bus, slot, func, (ix_u8)cap_ptr);
    if ((cap_hdr & 0xFFu) == 0x10u) {
      ix_u32 link_cap = ixg_pci_read(bus, slot, func, (ix_u8)(cap_ptr + 0x0Cu));
      gen = (ix_u8)(link_cap & 0xFu);
      lanes = (ix_u8)((link_cap >> 4u) & 0x3Fu);
      if (gen > 0u && lanes > 0u) {
        if (lanes >= 16u) lanes = 16u;
        else if (lanes >= 8u) lanes = 8u;
        else if (lanes >= 4u) lanes = 4u;
        else if (lanes >= 2u) lanes = 2u;
        else lanes = 1u;
      }
      *out_gen = gen;
      *out_lanes = lanes;
      return;
    }
    cap_ptr = (cap_hdr >> 8u) & 0xFCu;
  }
}

static ix_u64 ixg_driver_dma_phys_mask(ix_u16 bits) {
  if (bits >= 52u) return 0x000FFFFFFFFFF000ull;
  if (bits <= 12u) return 0x0000000000001000ull - 1ull;
  return ((1ull << bits) - 1ull) & 0x000FFFFFFFFFFFFFull;
}

static void ixg_driver_dma_rebuild_page_tables(void) {
  ix_u64 aperture_bytes;
  ix_u32 max_ptes;
  ix_u64 mask;
  ix_u32 out_count = 0u;
  memset(g_ixg_driver_dma_ptes, 0, sizeof(g_ixg_driver_dma_ptes));
  memset(g_ixg_driver_dma_l0, 0, sizeof(g_ixg_driver_dma_l0));
  memset(g_ixg_driver_dma_l1, 0, sizeof(g_ixg_driver_dma_l1));
  memset(g_ixg_driver_dma_l2, 0, sizeof(g_ixg_driver_dma_l2));
  g_ixg_driver_hw.dma_table_ready = 0u;
  g_ixg_driver_hw.dma_pte_count = 0u;
  aperture_bytes = (ix_u64)g_ixg_driver_hw.dma_aperture_mb << 20u;
  if (aperture_bytes == 0u) aperture_bytes = (ix_u64)IXG_DRIVER_DMA_DEFAULT_APERTURE_MB << 20u;
  max_ptes = (ix_u32)(aperture_bytes / IXG_DRIVER_DMA_PAGE_SIZE);
  if (max_ptes > IXG_DRIVER_DMA_MAX_PTES) max_ptes = IXG_DRIVER_DMA_MAX_PTES;
  mask = ixg_driver_dma_phys_mask(g_ixg_driver_hw.dma_mask_bits);
  if (g_ixg_driver_hw.dma_sandbox_only) {
    ix_u32 sandbox_ptes;
    ix_u8* sandbox_base;
    ix_u32 sandbox_bytes;
    ixg_driver_dma_init_sandbox_pool();
    sandbox_base = g_ixg_driver_dma_sandbox_ptr ? g_ixg_driver_dma_sandbox_ptr : g_ixg_driver_dma_sandbox;
    sandbox_bytes = g_ixg_driver_dma_sandbox_bytes ? g_ixg_driver_dma_sandbox_bytes : IXG_DRIVER_DMA_SANDBOX_BYTES;
    sandbox_ptes = (sandbox_bytes / IXG_DRIVER_DMA_PAGE_SIZE);
    if (max_ptes > sandbox_ptes) max_ptes = sandbox_ptes;
    for (ix_u32 i = 0u; i < max_ptes; i++) {
      ix_u64 vaddr = (ix_u64)(uintptr_t)(sandbox_base + (i * IXG_DRIVER_DMA_PAGE_SIZE));
      ix_u64 phys = ixm_virt_to_phys(vaddr);
      if (phys == 0u || (phys & ~mask) != 0u) continue;
      g_ixg_driver_dma_ptes[out_count++] =
        (phys & 0x000FFFFFFFFFF000ull) |
        IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE | IXG_DRIVER_DMA_PTE_SNOOP;
    }
    if (g_ixg_driver_hw.dma_aperture_base == 0u)
      g_ixg_driver_hw.dma_aperture_base = IXG_DRIVER_DMA_SANDBOX_IOVA_BASE;
    if (g_ixg_driver_hw.dma_aperture_mb == 0u || g_ixg_driver_hw.dma_aperture_mb > (sandbox_bytes >> 20u))
      g_ixg_driver_hw.dma_aperture_mb = (sandbox_bytes >> 20u);
    g_ixg_driver_dma_sandbox_head = IXG_DRIVER_DMA_PAGE_SIZE;
    g_ixg_driver_hw.dma_pte_count = out_count;
    g_ixg_driver_hw.dma_table_ready = out_count ? 1u : 0u;
    if (out_count) {
      g_ixg_driver_dma_l0[0] = ixm_virt_to_phys((ix_u64)(uintptr_t)g_ixg_driver_dma_l1) | IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE;
      g_ixg_driver_dma_l1[0] = ixm_virt_to_phys((ix_u64)(uintptr_t)g_ixg_driver_dma_l2) | IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE;
      g_ixg_driver_dma_l2[0] = ixm_virt_to_phys((ix_u64)(uintptr_t)g_ixg_driver_dma_ptes) | IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE;
    }
    return;
  }
  for (ix_u32 i = 0u; i < max_ptes && out_count < max_ptes; i++) {
    ix_u64 phys = (ix_u64)i * (ix_u64)IXG_DRIVER_DMA_PAGE_SIZE;
    if ((phys & ~mask) != 0u) break;
    g_ixg_driver_dma_ptes[out_count++] = (phys & 0x000FFFFFFFFFF000ull) | IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE;
  }
  if (g_ixg_driver_hw.dma_aperture_base == 0u)
    g_ixg_driver_hw.dma_aperture_base = 0x100000ull;
  g_ixg_driver_hw.dma_pte_count = out_count;
  g_ixg_driver_hw.dma_table_ready = out_count ? 1u : 0u;
  if (out_count) {
    g_ixg_driver_dma_l0[0] = ixm_virt_to_phys((ix_u64)(uintptr_t)g_ixg_driver_dma_l1) | IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE;
    g_ixg_driver_dma_l1[0] = ixm_virt_to_phys((ix_u64)(uintptr_t)g_ixg_driver_dma_l2) | IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE;
    g_ixg_driver_dma_l2[0] = ixm_virt_to_phys((ix_u64)(uintptr_t)g_ixg_driver_dma_ptes) | IXG_DRIVER_DMA_PTE_VALID | IXG_DRIVER_DMA_PTE_WRITE;
  }
}

static void ixg_driver_dma_flush_tlb(void) {
  if (!g_ixg_driver_hw.ready) return;
  if (g_ixg_driver_hw.regs.iommu_tlb_inv != 0u) {
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.iommu_tlb_inv, 1u);
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.iommu_tlb_inv, 0u);
  }
  IXG_INLINE_MFENCE();
}

void ixg_driver_program_dma_registers(void) {
  ix_u64 aperture_bytes;
  ix_u64 aperture_end;
  ix_u64 pte_phys;
  ix_u32 ctrl = 0u;
  if (!g_ixg_driver_hw.ready) return;
  ixg_driver_dma_rebuild_page_tables();
  aperture_bytes = (ix_u64)g_ixg_driver_hw.dma_aperture_mb << 20u;
  if (aperture_bytes == 0u) aperture_bytes = (ix_u64)IXG_DRIVER_DMA_DEFAULT_APERTURE_MB << 20u;
  aperture_end = g_ixg_driver_hw.dma_aperture_base + aperture_bytes - 1u;
  if (g_ixg_driver_hw.regs.gart_base != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.gart_base, (ix_u32)(g_ixg_driver_hw.dma_aperture_base & 0xFFFFFFFFu));
  if (g_ixg_driver_hw.regs.gart_base_hi != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.gart_base_hi, (ix_u32)(g_ixg_driver_hw.dma_aperture_base >> 32));
  if (g_ixg_driver_hw.regs.gart_limit != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.gart_limit, (ix_u32)(aperture_end & 0xFFFFFFFFu));
  pte_phys = ixm_virt_to_phys((ix_u64)(uintptr_t)g_ixg_driver_dma_l0);
  if (g_ixg_driver_hw.regs.gart_pt_base != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.gart_pt_base, (ix_u32)(pte_phys & 0xFFFFFFFFu));
  if (g_ixg_driver_hw.regs.gart_pt_base_hi != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.gart_pt_base_hi, (ix_u32)(pte_phys >> 32));
  if (g_ixg_driver_hw.regs.iommu_ctrl != 0u) {
    if (g_ixg_driver_hw.iommu_enabled) ctrl |= 1u;
    if (g_ixg_driver_hw.gart_enabled) ctrl |= 2u;
    if (g_ixg_driver_hw.dma_mode == (ix_u8)IXG_DRIVER_DMA_MODE_IOMMU) ctrl |= 4u;
    else if (g_ixg_driver_hw.dma_mode == (ix_u8)IXG_DRIVER_DMA_MODE_GART) ctrl |= 8u;
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.iommu_ctrl, ctrl);
  }
  ixg_driver_dma_flush_tlb();
  ix_iommu_register_dma(IX_IOMMU_DOMAIN_GPU, g_ixg_driver_hw.dma_aperture_base,
                         aperture_bytes, IX_IOMMU_FLAG_READ | IX_IOMMU_FLAG_WRITE | IX_IOMMU_FLAG_DEVICE);
}

void ixg_driver_program_ring_registers(void) {
  if (!g_ixg_driver_hw.ready) return;
  if (g_ixg_driver_hw.regs.ring_base != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_base, 0u);
  if (g_ixg_driver_hw.regs.ring_size != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_size, IXG_DRIVER_CMD_RING_BYTES);
}

ix_u32 ixg_driver_mmio_read(ix_u32 offset) {
  ix_u64 eff;
  if (!g_ixg_driver_hw.ready || !g_ixg_driver_hw.mmio) return 0u;
  eff = (ix_u64)offset + (ix_u64)g_ixg_driver_hw.reg_addend;
  if (eff + 4u > g_ixg_driver_hw.bar0_size) return 0u;
  return *((volatile ix_u32*)(g_ixg_driver_hw.mmio + (ix_u32)eff));
}

void ixg_driver_mmio_write(ix_u32 offset, ix_u32 value) {
  ix_u64 eff;
  if (!g_ixg_driver_hw.ready || !g_ixg_driver_hw.mmio) return;
  eff = (ix_u64)offset + (ix_u64)g_ixg_driver_hw.reg_addend;
  if (eff + 4u > g_ixg_driver_hw.bar0_size) return;
  *((volatile ix_u32*)(g_ixg_driver_hw.mmio + (ix_u32)eff)) = value;
}

void ixg_driver_mmio_flush_posted(void) {
  if (!g_ixg_driver_hw.ready) return;
  IXG_INLINE_MFENCE();
  if (g_ixg_driver_hw.regs.status)
    (void)ixg_driver_mmio_read(g_ixg_driver_hw.regs.status);
  IXG_INLINE_MFENCE();
}

static ix_u32 ixg_driver_ring_head_from_mmio(void) {
  ix_u32 raw = 0u;
  if (!g_ixg_driver_hw.ready) return g_ixg_driver_hw.cmd_head_cache;
  if (g_ixg_driver_hw.regs.ring_head != 0u)
    raw = ixg_driver_mmio_read(g_ixg_driver_hw.regs.ring_head);
  else if (g_ixg_driver_hw.regs.ring_get != 0u)
    raw = ixg_driver_mmio_read(g_ixg_driver_hw.regs.ring_get);
  else
    return g_ixg_driver_hw.cmd_head_cache;
  if (raw == 0xFFFFFFFFu) {
    g_ixg_driver_hw.submit_failed = 1u;
    return g_ixg_driver_hw.cmd_head_cache;
  }
  if (g_ixg_driver_hw.ring_tail_is_bytes)
    raw /= (ix_u32)sizeof(ixg_driver_gpu_cmd_t);
  if (g_ixg_driver_hw.ring_mask != 0u)
    raw &= g_ixg_driver_hw.ring_mask;
  g_ixg_driver_hw.cmd_head_cache = raw;
  return raw;
}

ix_u32 ixg_driver_ring_used(void) {
  ix_u32 head = ixg_driver_ring_head_from_mmio();
  ix_u32 tail = g_ixg_driver_hw.cmd_tail;
  if (tail >= head) return tail - head;
  return (tail + IXG_DRIVER_CMD_RING_CAP) - head;
}

void ixg_driver_store_rescue_blob(ix_u32 reason, ix_u32 frame_id, ix_u32 load_hint) {
  (void)reason; (void)frame_id; (void)load_hint;
  g_ixg_driver_rescue_counter++;
}

void ixg_driver_kick_ring(ix_u32 op) {
  ix_u32 tail = g_ixg_driver_hw.cmd_tail;
  ix_u32 hw_tail = tail;
  if (g_ixg_driver_hw.ring_mask != 0u) hw_tail &= g_ixg_driver_hw.ring_mask;
  if (g_ixg_driver_hw.ring_tail_is_bytes)
    hw_tail *= (ix_u32)sizeof(ixg_driver_gpu_cmd_t);
  if (g_ixg_driver_hw.regs.ring_tail != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_tail, hw_tail);
  else if (g_ixg_driver_hw.regs.ring_put != 0u)
    ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_put, hw_tail);
  if (g_ixg_driver_hw.regs.doorbell != 0u) {
    if (g_ixg_driver_hw.doorbell_mode == (ix_u8)IXG_DRIVER_DBELL_NVIDIA)
      ixg_driver_mmio_write(g_ixg_driver_hw.regs.doorbell, (op << 16) | 1u);
    else if (g_ixg_driver_hw.doorbell_mode == (ix_u8)IXG_DRIVER_DBELL_TAIL)
      ixg_driver_mmio_write(g_ixg_driver_hw.regs.doorbell, hw_tail);
    else
      ixg_driver_mmio_write(g_ixg_driver_hw.regs.doorbell, op);
  }
  ixg_driver_mmio_flush_posted();
}

ix_u8 ixg_driver_wait_idle(ix_u32 spins) {
  ix_u32 remaining = spins ? spins : 1u;
  ix_u32 total = 0u;
  ix_u32 last_head = g_ixg_driver_hw.cmd_head_cache;
  ix_u32 last_used = 0xFFFFFFFFu;
  ix_u32 no_progress = 0u;
  while (remaining-- > 0u) {
    ix_u32 head = ixg_driver_ring_head_from_mmio();
    ix_u32 tail = g_ixg_driver_hw.cmd_tail;
    ix_u32 used;
    if (tail >= head) used = tail - head;
    else used = (tail + IXG_DRIVER_CMD_RING_CAP) - head;
    if (used == 0u) return 1u;
    if (head == last_head && used == last_used) {
      no_progress++;
      if ((no_progress & (IXG_DRIVER_MMIO_REKICK_SPINS - 1u)) == 0u)
        ixg_driver_kick_ring(10u);
      if (no_progress >= IXG_DRIVER_MMIO_NO_PROGRESS_SPINS) {
        if (total < IXG_DRIVER_SPIN_THEN_SLEEP_SPINS) IXG_INLINE_PAUSE();
        else if (total < IXG_DRIVER_TDR_TIMEOUT_SPINS) IXG_INLINE_PAUSE();
        else { ixg_driver_store_rescue_blob(2u, 0u, used); return 0u; }
      }
    } else {
      last_head = head;
      last_used = used;
      no_progress = 0u;
    }
    total++;
    IXG_INLINE_PAUSE();
  }
  ixg_driver_store_rescue_blob(1u, 0u, last_used == 0xFFFFFFFFu ? 0u : last_used);
  return 0u;
}

static void ixg_driver_apply_profile(ixg_driver_hw_state_t* hw, const ixg_driver_profile_t* p) {
  if (!hw || !p) return;
  hw->vendor_path = p->vendor_path;
  hw->ring_tail_is_bytes = p->ring_tail_is_bytes;
  hw->doorbell_mode = p->doorbell_mode;
  hw->reg_addend = p->reg_addend;
  hw->regs = p->regs;
  hw->profile_name = p->profile_name;
  hw->ring_mask = IXG_DRIVER_CMD_RING_CAP - 1u;
}

static const ixg_driver_profile_t* ixg_driver_find_profile(ix_u16 vendor_id, ix_u16 device_id) {
  ix_u32 i;
  for (i = 0u; i < (ix_u32)(sizeof(g_ixg_driver_profiles) / sizeof(g_ixg_driver_profiles[0])); i++) {
    const ixg_driver_profile_t* p = &g_ixg_driver_profiles[i];
    if (p->vendor_id != vendor_id) continue;
    if (device_id < p->device_id_lo || device_id > p->device_id_hi) continue;
    return p;
  }
  return 0;
}

static ix_u8 ixg_driver_profile_probe_reg(ix_u32 reg, ix_u64 add, ix_u64 bar0_size, ix_u32* out_v0, ix_u32* out_v1) {
  volatile ix_u8* mmio = g_ixg_driver_hw.mmio;
  ix_u64 off = (ix_u64)reg + add;
  volatile ix_u32* ptr;
  ix_u32 v0, v1;
  if (!mmio || reg == 0u || !out_v0 || !out_v1 || !g_ixg_driver_hw.pci_valid) return 0u;
  if (off + 4u > bar0_size) return 0u;
  ptr = (volatile ix_u32*)(mmio + (ix_u32)off);
  v0 = *ptr;
  IXG_INLINE_PAUSE();
  v1 = *ptr;
  if (v0 == 0xFFFFFFFFu && v1 == 0xFFFFFFFFu) return 0u;
  *out_v0 = v0;
  *out_v1 = v1;
  return 1u;
}

static ix_u32 ixg_driver_profile_probe_score(const ixg_driver_profile_t* p, ix_u64 bar0_size) {
  ix_u32 score = 0u;
  ix_u64 add = p ? (ix_u64)p->reg_addend : 0u;
  ix_u32 v0 = 0u, v1 = 0u;
  if (!p || !g_ixg_driver_hw.mmio) return 0u;
  if (ixg_driver_profile_probe_reg(p->regs.status, add, bar0_size, &v0, &v1)) {
    score += 3u;
    if (v0 != v1) score += 1u;
  }
  if (ixg_driver_profile_probe_reg(p->regs.ring_head, add, bar0_size, &v0, &v1)) {
    score += 2u;
    if (v0 != v1) score += 2u;
  } else if (ixg_driver_profile_probe_reg(p->regs.ring_get, add, bar0_size, &v0, &v1)) {
    score += 2u;
    if (v0 != v1) score += 2u;
  }
  if (ixg_driver_profile_probe_reg(p->regs.ring_tail, add, bar0_size, &v0, &v1)) {
    score += 2u;
    if (((v0 | v1) & 0x3u) == 0u) score += 1u;
  } else if (ixg_driver_profile_probe_reg(p->regs.ring_put, add, bar0_size, &v0, &v1)) {
    score += 2u;
    if (((v0 | v1) & 0x3u) == 0u) score += 1u;
  }
  if (ixg_driver_profile_probe_reg(p->regs.doorbell, add, bar0_size, &v0, &v1)) score += 1u;
  return score;
}

static const ixg_driver_profile_t* ixg_driver_autoselect_profile(ix_u16 vendor_id, ix_u16 device_id, ix_u64 bar0_size, ix_u32* out_best_score) {
  const ixg_driver_profile_t* best = 0;
  ix_u32 best_score = 0u;
  ix_u32 i;
  if (out_best_score) *out_best_score = 0u;
  for (i = 0u; i < (ix_u32)(sizeof(g_ixg_driver_profiles) / sizeof(g_ixg_driver_profiles[0])); i++) {
    const ixg_driver_profile_t* p = &g_ixg_driver_profiles[i];
    ix_u32 score;
    if (p->vendor_id != vendor_id) continue;
    if (device_id < p->device_id_lo || device_id > p->device_id_hi) continue;
    score = ixg_driver_profile_probe_score(p, bar0_size);
    if (device_id >= p->device_id_lo && device_id <= p->device_id_hi) score += 3u;
    if (score > best_score) { best = p; best_score = score; }
  }
  if (out_best_score) *out_best_score = best_score;
  return best;
}

static void ixg_driver_set_default_caps(void) {
  g_driver_caps.vendor_id = 0;
  g_driver_caps.device_id = 0;
  g_driver_caps.backend = (uint8_t)IXG_BACKEND_SOFTWARE;
  g_driver_caps.feature_level = 9u;
  g_driver_caps.shader_model = 3u;
  g_driver_caps.reserved0 = 0;
  g_driver_caps.feature_flags = IXG_DRIVER_FEAT_2D_FILL_RECT | IXG_DRIVER_FEAT_2D_BITBLT |
                                IXG_DRIVER_FEAT_2D_ALPHA_BLEND | IXG_DRIVER_FEAT_3D_FIXED |
                                IXG_DRIVER_FEAT_3D_TEXTURE | IXG_DRIVER_FEAT_CONTROL_SHADER;
}

static void ixg_driver_apply_vendor_caps(uint16_t vendor_id, uint16_t device_id, uint8_t headless) {
  uint32_t flags = IXG_DRIVER_FEAT_2D_FILL_RECT | IXG_DRIVER_FEAT_2D_BITBLT |
                   IXG_DRIVER_FEAT_2D_ALPHA_BLEND | IXG_DRIVER_FEAT_3D_FIXED |
                   IXG_DRIVER_FEAT_3D_SHADER | IXG_DRIVER_FEAT_3D_TEXTURE |
                   IXG_DRIVER_FEAT_3D_INDEXED | IXG_DRIVER_FEAT_CONTROL_SHADER;
  g_driver_caps.vendor_id = vendor_id;
  g_driver_caps.device_id = device_id;
  g_driver_caps.reserved0 = 0;
  g_driver_caps.feature_level = 9u;
  g_driver_caps.shader_model = 2u;
  g_driver_caps.backend = (uint8_t)IXG_BACKEND_SOFTWARE;
  if (vendor_id == 0x8086u) {
    g_driver_caps.backend = (uint8_t)IXG_BACKEND_INTEL;
    g_driver_caps.feature_level = 13u;
    g_driver_caps.shader_model = 7u;
    flags |= IXG_DRIVER_FEAT_3D_INSTANCING | IXG_DRIVER_FEAT_3D_COMPUTE |
             IXG_DRIVER_FEAT_3D_BINDLESS | IXG_DRIVER_FEAT_3D_TIMELINE_SYNC |
             IXG_DRIVER_FEAT_3D_DYNAMIC_RENDERING | IXG_DRIVER_FEAT_3D_DRAW_INDIRECT_COUNT |
             IXG_DRIVER_FEAT_3D_VRS | IXG_DRIVER_FEAT_3D_MESH_SHADER |
             IXG_DRIVER_FEAT_3D_RAY_TRACING | IXG_DRIVER_FEAT_3D_MULTI_GPU;
  } else if (vendor_id == 0x10DEu) {
    g_driver_caps.backend = (uint8_t)IXG_BACKEND_NVIDIA;
    g_driver_caps.feature_level = 12u;
    g_driver_caps.shader_model = 6u;
    flags |= IXG_DRIVER_FEAT_3D_INSTANCING | IXG_DRIVER_FEAT_3D_COMPUTE |
             IXG_DRIVER_FEAT_3D_BINDLESS | IXG_DRIVER_FEAT_3D_TIMELINE_SYNC |
             IXG_DRIVER_FEAT_3D_DYNAMIC_RENDERING | IXG_DRIVER_FEAT_3D_DRAW_INDIRECT_COUNT |
             IXG_DRIVER_FEAT_3D_MESH_SHADER | IXG_DRIVER_FEAT_3D_RAY_TRACING | IXG_DRIVER_FEAT_3D_VRS;
  } else if (vendor_id == 0x1002u || vendor_id == 0x1022u) {
    g_driver_caps.backend = (uint8_t)IXG_BACKEND_AMD;
    g_driver_caps.feature_level = 12u;
    g_driver_caps.shader_model = 6u;
    flags |= IXG_DRIVER_FEAT_3D_INSTANCING | IXG_DRIVER_FEAT_3D_COMPUTE |
             IXG_DRIVER_FEAT_3D_BINDLESS | IXG_DRIVER_FEAT_3D_TIMELINE_SYNC |
             IXG_DRIVER_FEAT_3D_DYNAMIC_RENDERING | IXG_DRIVER_FEAT_3D_DRAW_INDIRECT_COUNT;
    if (device_id >= 0x7300u) flags |= IXG_DRIVER_FEAT_3D_RAY_TRACING | IXG_DRIVER_FEAT_3D_VRS;
  }
  if (headless) flags |= IXG_DRIVER_FEAT_HEADLESS;
  g_driver_caps.feature_flags = flags;
}

static ix_u8 ixg_driver_read_pci_bar(ix_u8 bus, ix_u8 slot, ix_u8 func, ix_u8 bar_index, ix_u64* out_addr, ix_u64* out_size) {
  ix_u32 bar_lo, bar_hi;
  ix_u32 orig_lo;
  ix_u64 addr, size;
  if (bar_index > 5 || !out_addr || !out_size) return 0u;
  bar_lo = ixg_pci_read(bus, slot, func, 0x10u + bar_index * 4u);
  orig_lo = bar_lo;
  if ((bar_lo & 1u) == 0u && (bar_lo & 0x6u) == 0u) {
    ixk_outl(0xCF8, (1u << 31) | ((ix_u32)bus << 16) | ((ix_u32)slot << 11) | ((ix_u32)func << 8) | (0x10u + bar_index * 4u + 4u));
    bar_hi = ixk_inl(0xCFC);
    addr = ((ix_u64)bar_hi << 32) | (ix_u64)(bar_lo & 0xFFFFFFF0u);
    ixk_outl(0xCF8, (1u << 31) | ((ix_u32)bus << 16) | ((ix_u32)slot << 11) | ((ix_u32)func << 8) | (0x10u + bar_index * 4u));
    ixk_outl(0xCFC, 0xFFFFFFFFu);
    ixk_outl(0xCF8, (1u << 31) | ((ix_u32)bus << 16) | ((ix_u32)slot << 11) | ((ix_u32)func << 8) | (0x10u + bar_index * 4u));
    {
      ix_u32 size_lo = ixk_inl(0xCFC);
      ixk_outl(0xCF8, (1u << 31) | ((ix_u32)bus << 16) | ((ix_u32)slot << 11) | ((ix_u32)func << 8) | (0x10u + bar_index * 4u));
      ixk_outl(0xCFC, orig_lo);
      if (size_lo == 0u) { *out_addr = 0; *out_size = 0; return 0u; }
      size = (ix_u64)(size_lo & 0xFFFFFFF0u);
      if (size == 0u) { *out_addr = 0; *out_size = 0; return 0u; }
      size = size & ~(size - 1u);
    }
    *out_addr = addr;
    *out_size = size;
    return 1u;
  }
  addr = (ix_u64)(bar_lo & 0xFFFFFFF0u);
  *out_addr = addr;
  *out_size = 4096u;
  return 1u;
}

void ixg_driver_init(void) {
  ix_u16 bus, slot;
  ix_u32 id;
  ix_u32 class_reg;
  ix_u8 cls;
  ix_u16 vendor, device;
  ix_u64 bar0_addr = 0, bar0_size = 0;
  const ixg_driver_profile_t* profile;
  ix_u32 probe_score = 0u;

  memset(&g_ixg_driver_hw, 0, sizeof(g_ixg_driver_hw));
  ixg_driver_set_default_caps();

  for (bus = 0; bus < 256; bus++) {
    for (slot = 0; slot < 32; slot++) {
      id = ixg_pci_read((ix_u8)bus, slot, 0, 0x00u);
      if (id == 0xFFFFFFFFu) continue;
      class_reg = ixg_pci_read((ix_u8)bus, slot, 0, 0x08u);
      cls = (ix_u8)((class_reg >> 24) & 0xFFu);
      if (cls != 0x03u && cls != 0x12u) continue;
      vendor = (ix_u16)(id & 0xFFFFu);
      device = (ix_u16)(id >> 16);
      if (vendor != 0x8086u && vendor != 0x10DEu && vendor != 0x1002u && vendor != 0x1022u && vendor != 0x1234u) continue;

      g_ixg_driver_hw.bus = (ix_u8)bus;
      g_ixg_driver_hw.slot = (ix_u8)slot;
      g_ixg_driver_hw.func = 0;
      g_ixg_driver_hw.vendor_id = vendor;
      g_ixg_driver_hw.device_id = device;
      g_ixg_driver_hw.pci_valid = 1u;
      g_ixg_driver_hw.pci_seg = 0;
      g_ixg_driver_hw.pci_bus = (ix_u8)bus;
      g_ixg_driver_hw.pci_dev = (ix_u8)slot;

      {
        ix_u8 found_mmio = 0u;
        for (ix_u8 bar_idx = 0; bar_idx < 6; bar_idx++) {
          ix_u32 bar_lo = ixg_pci_read((ix_u8)bus, slot, 0, 0x10u + bar_idx * 4u);
          if (bar_lo == 0u || bar_lo == 0xFFFFFFFFu) continue;
          if (bar_lo & 1u) continue;
          ix_u32 bar_type = (bar_lo >> 1) & 0x3u;
          if (bar_type == 0u) {
            ix_u64 addr, sz;
            if (ixg_driver_read_pci_bar((ix_u8)bus, slot, 0, bar_idx, &addr, &sz) && sz >= 0x1000u) {
              g_ixg_driver_hw.bar0_base = addr;
              g_ixg_driver_hw.bar0_size = sz;
              g_ixg_driver_hw.mmio = (volatile ix_u8*)(uintptr_t)addr;
              found_mmio = 1u;
              break;
            }
          }
        }
        if (!found_mmio) {
          g_ixg_driver_hw.mmio = 0;
        }
      }

      if (!g_ixg_driver_hw.mmio) continue;

      profile = ixg_driver_find_profile(vendor, device);
      if (profile) {
        ixg_driver_apply_profile(&g_ixg_driver_hw, profile);
      } else {
        profile = ixg_driver_autoselect_profile(vendor, device, g_ixg_driver_hw.bar0_size, &probe_score);
        if (profile) ixg_driver_apply_profile(&g_ixg_driver_hw, profile);
      }

      ixg_driver_init_dma_policy(&g_ixg_driver_hw);
      ixg_driver_apply_vendor_caps(vendor, device, 0);
      g_ixg_driver_hw.ready = (g_ixg_driver_hw.mmio != 0) ? 1u : 0u;

      if (g_ixg_driver_hw.ready) {
        ixg_driver_program_dma_registers();
        ixg_driver_program_ring_registers();
      }

      if (vendor == 0x8086u) {
        ixg_backend_intel_init_ops(&g_ixg_driver_vendor_ops);
        ixg_backend_intel_post_init();
      } else if (vendor == 0x10DEu) {
        ixg_backend_nvidia_init_ops(&g_ixg_driver_vendor_ops);
        ixg_backend_nvidia_post_init();
      } else if (vendor == 0x1002u || vendor == 0x1022u) {
        ixg_backend_amd_init_ops(&g_ixg_driver_vendor_ops);
        ixg_backend_amd_post_init();
      }

      g_hw_ops = &g_ixg_driver_vendor_ops;
      g_hw_detected = 1;
      g_driver_initialized = 1;
      goto found_gpu;
    }
  }
 found_gpu:
  if (!g_hw_detected) {
    ixg_driver_set_default_caps();
  }
}

void ixg_driver_register_ops(ixg_hw_ops_t* ops) { g_hw_ops = ops; }

void ixg_driver_accel_rect(int x, int y, int w, int h, ix_u32 color) {
  if (g_hw_ops && g_hw_ops->hw_draw_rect) g_hw_ops->hw_draw_rect(x, y, w, h, color);
}
void ixg_driver_accel_bitblt(int dx, int dy, int w, int h, void* src, int sx, int sy) {
  if (g_hw_ops && g_hw_ops->hw_bitblt) g_hw_ops->hw_bitblt(dx, dy, w, h, src, sx, sy);
}
int ixg_driver_accel_rect_blend(int x, int y, int w, int h, ix_u32 color, ix_u8 premultiplied) {
  if (g_hw_ops && g_hw_ops->hw_draw_rect_blend) return g_hw_ops->hw_draw_rect_blend(x, y, w, h, color, premultiplied);
  return -1;
}
int ixg_driver_accel_bitblt_alpha(int dx, int dy, int w, int h, const void* src, ix_u32 stride_bytes, ix_u8 premultiplied) {
  if (g_hw_ops && g_hw_ops->hw_bitblt_alpha) return g_hw_ops->hw_bitblt_alpha(dx, dy, w, h, src, stride_bytes, premultiplied);
  return -1;
}
void ixg_driver_flush(void) { if (g_hw_ops && g_hw_ops->hw_flush) g_hw_ops->hw_flush(); }
ix_u32 ixg_driver_is_accel_ready(void) { return g_hw_detected; }
ix_u32 ixg_driver_is_gpu_3d_ready(void) { return g_hw_detected && g_hw_ops != 0; }
ix_u32 ixg_driver_has_gpu_triangles(void) { return (g_hw_ops && g_hw_ops->hw_draw_triangles) ? 1u : 0u; }
ix_u32 ixg_driver_has_gpu_triangles_textured(void) { return (g_hw_ops && g_hw_ops->hw_draw_triangles_textured) ? 1u : 0u; }
ix_u32 ixg_driver_has_gpu_compute(void) { return (g_hw_ops && g_hw_ops->hw_compute_dispatch) ? 1u : 0u; }
ix_u32 ixg_driver_has_gpu_shader_bind(void) { return (g_hw_ops && g_hw_ops->hw_bind_shaders) ? 1u : 0u; }
int ixg_driver_accel_bind_shaders(const ixg_hw_shader_bind_t* bind) {
  if (g_hw_ops && g_hw_ops->hw_bind_shaders) return g_hw_ops->hw_bind_shaders(bind);
  return -1;
}
ix_u32 ixg_driver_gpu_prefers_hardware_only(void) { return g_ixg_driver_hw.render_hw_only; }
ix_u32 ixg_driver_gpu_submit_mirror_3d(void) { return g_ixg_driver_hw.render_submit_mirror_3d; }
int ixg_driver_accel_triangles(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count) {
  if (g_hw_ops && g_hw_ops->hw_draw_triangles) return g_hw_ops->hw_draw_triangles(vertices, triangle_count);
  return -1;
}
int ixg_driver_accel_triangles_textured(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture) {
  if (g_hw_ops && g_hw_ops->hw_draw_triangles_textured) return g_hw_ops->hw_draw_triangles_textured(vertices, triangle_count, texture);
  return -1;
}
ix_u32 ixg_driver_has_render_readback(void) {
  return (g_hw_ops && g_hw_ops->hw_set_render_target && g_hw_ops->hw_readback_rect) ? 1u : 0u;
}
int ixg_driver_accel_set_render_target(ix_u32 w, ix_u32 h) {
  if (g_hw_ops && g_hw_ops->hw_set_render_target) return g_hw_ops->hw_set_render_target(w, h);
  return -1;
}
int ixg_driver_accel_readback_rect(ix_u32* dst, ix_u32 w, ix_u32 h) {
  if (g_hw_ops && g_hw_ops->hw_readback_rect) return g_hw_ops->hw_readback_rect(dst, w, h);
  return -1;
}
/* Пер-кадровая очистка CPU-visible render target. RT очищается
 * нулями только при аллокации; без этой заливки на цели остаются
 * шлейфы предыдущего кадра (фон под движущейся геометрией не
 * восстанавливается). Цвет 0 далее трактуется readback'ом как фон. */
void ixg_driver_accel_rt_fill(ix_u32 color) {
  ix_u32 i, n;
  ix_u32* p;
  if (!ixg_rt_cpu) return;
  n = ixg_rt_stride_px * ixg_rt_h;
  p = (ix_u32*)ixg_rt_cpu;
  for (i = 0; i < n; i++) p[i] = color;
}
/* Readback "поверх": пиксели цели, равные bg, считаются фоном и
 * НЕ копируются в dst -- там сохраняется то, что приложение
 * нарисовало в canvas 2D-средствами (axgl_clear + ax_text).
 * Пиксели геометрии (!= bg) пишутся поверх. Так GPU-кадр
 * комбинируется с 2D-оверлеем, а не затирает его целиком. */
int ixg_driver_accel_readback_rect_over(ix_u32* dst, ix_u32 w, ix_u32 h, ix_u32 bg) {
  ix_u32 row, x;
  if (!ixg_rt_cpu || !dst) return -1;
  if (w != ixg_rt_w || h > ixg_rt_h) return -1;
  for (row = 0; row < h; row++) {
    const ix_u32* src = (const ix_u32*)(ixg_rt_cpu + (ix_u64)row * (ix_u64)ixg_rt_stride_px * 4u);
    ix_u32* d = dst + (ix_u64)row * (ix_u64)w;
    for (x = 0; x < w; x++)
      if (src[x] != bg) d[x] = src[x];
  }
  return 0;
}
int ixg_driver_accel_compute_dispatch(const void* dispatch_desc) {
  if (g_hw_ops && g_hw_ops->hw_compute_dispatch) return g_hw_ops->hw_compute_dispatch(dispatch_desc);
  return -1;
}
int ixg_driver_accel_trace_rays(const void* trace_desc) {
  if (g_hw_ops && g_hw_ops->hw_trace_rays) return g_hw_ops->hw_trace_rays(trace_desc);
  return -1;
}
int ixg_driver_accel_control_dispatch(const void* control_desc, void* control_result) {
  if (g_hw_ops && g_hw_ops->hw_control_dispatch) return g_hw_ops->hw_control_dispatch(control_desc, control_result);
  return -1;
}
void ixg_driver_get_caps(ixg_driver_caps_t* out_caps) { if (out_caps) *out_caps = g_driver_caps; }
const char* ixg_driver_backend_name(void) {
  switch (g_driver_caps.backend) {
    case IXG_BACKEND_NVIDIA: return "NVIDIA";
    case IXG_BACKEND_AMD: return "AMD";
    case IXG_BACKEND_INTEL: return "Intel";
    case IXG_BACKEND_EXOTIC: return "Exotic";
    default: return "Software";
  }
}
void ixg_driver_get_dma_window(ix_u64* out_base, ix_u32* out_size, ix_u8* out_sandbox) {
  if (out_base) *out_base = g_ixg_driver_hw.dma_aperture_base;
  if (out_size) *out_size = g_ixg_driver_hw.dma_aperture_mb << 20u;
  if (out_sandbox) *out_sandbox = g_ixg_driver_hw.dma_sandbox_only;
}
void ixg_driver_rescue_tick(ix_u32 load_hint, ix_u32 frame_id) {
  (void)load_hint; (void)frame_id;
}