#include "ixg_backend_amd.h"
#include "ixg_compat.h"
#include "ixg_compute.h"
#include "ixg_mutants.h"
#include "ixg_isa.h"
#include "ixg_isa_encode.h"
#include "ixg_pman.h"
#include <string.h>

#define AMD_FIFO_FREE_SPINS 20000u
#define AMD_PUSH_MAX_DWORDS 256u
#define AMD_PACKET3 0xC0000000u
#define AMD_IT_NOP 0x10u
#define AMD_IT_CONTEXT_CONTROL 0x28u
#define AMD_IT_INDEX_TYPE 0x2Au
#define AMD_IT_DRAW_INDEX_AUTO 0x2Du
#define AMD_IT_NUM_INSTANCES 0x2Fu
#define AMD_IT_WRITE_DATA 0x37u
#define AMD_IT_MEM_SEMAPHORE 0x39u
#define AMD_IT_WAIT_REG_MEM 0x3Cu
#define AMD_IT_INDIRECT_BUFFER 0x3Fu
#define AMD_IT_COPY_DATA 0x40u
#define AMD_IT_PFP_SYNC_ME 0x42u
#define AMD_IT_SURFACE_SYNC 0x43u
#define AMD_IT_EVENT_WRITE 0x46u
#define AMD_IT_DMA_DATA 0x50u
#define AMD_IT_ACQUIRE_MEM 0x58u
#define AMD_IT_RELEASE_MEM 0x49u
#define AMD_IT_SET_CONFIG_REG 0x68u
#define AMD_IT_SET_CONTEXT_REG 0x69u
#define AMD_IT_SET_SH_REG 0x76u
#define AMD_IT_SET_UCONFIG_REG 0x79u
#define AMD_IT_DISPATCH_DIRECT 0x15u
#define AMD_IT_DISPATCH_INDIRECT 0x16u
#define AMD_IT_SET_BASE 0x11u
#define AMD_REG_GRBM_GFX_INDEX 0x30800u
#define AMD_REG_DB_RENDER_CONTROL 0xA000u
#define AMD_REG_DB_RENDER_OVERRIDE 0xA004u
#define AMD_REG_PA_SC_SCREEN_SCISSOR_TL 0xA00Cu
#define AMD_REG_PA_SC_SCREEN_SCISSOR_BR 0xA010u
#define AMD_REG_PA_SC_WINDOW_OFFSET 0xA080u
#define AMD_REG_PA_SC_WINDOW_SCISSOR_TL 0xA204u
#define AMD_REG_PA_SC_WINDOW_SCISSOR_BR 0xA208u
#define AMD_REG_VGT_PRIMITIVE_TYPE 0xA1C8u
#define AMD_REG_VGT_INDEX_TYPE 0xA29Cu
#define AMD_REG_VGT_VERTEX_REUSE_BLOCK_CNTL 0xA1C0u
#define AMD_REG_CB_TARGET_MASK 0xA08Cu
#define AMD_REG_CB_SHADER_MASK 0xA090u
#define AMD_REG_CB_COLOR_CONTROL 0xA202u
#define AMD_REG_CB_COLOR0_BASE 0xA318u
#define AMD_REG_CB_COLOR0_PITCH 0xA31Cu
#define AMD_REG_CB_COLOR0_SLICE 0xA320u
#define AMD_REG_CB_COLOR0_VIEW 0xA324u
#define AMD_REG_CB_COLOR0_INFO 0xA328u
#define AMD_REG_CB_COLOR0_ATTRIB 0xA32Cu
#define AMD_REG_CB_COLOR0_DCC_CONTROL 0xA330u
#define AMD_REG_CB_COLOR0_CMASK 0xA334u
#define AMD_REG_CB_COLOR0_FMASK 0xA338u
#define AMD_REG_SPI_SHADER_PGM_LO_PS 0x2C08u
#define AMD_REG_SPI_SHADER_PGM_HI_PS 0x2C0Cu
#define AMD_REG_SPI_SHADER_PGM_LO_VS 0x2C48u
#define AMD_REG_SPI_SHADER_PGM_HI_VS 0x2C4Cu
#define AMD_REG_SPI_SHADER_PGM_RSRC1_PS 0x2C10u
#define AMD_REG_SPI_SHADER_PGM_RSRC2_PS 0x2C14u
#define AMD_REG_SPI_SHADER_PGM_RSRC1_VS 0x2C50u
#define AMD_REG_SPI_SHADER_PGM_RSRC2_VS 0x2C54u
#define AMD_REG_SPI_PS_INPUT_ENA 0xA1B3u
#define AMD_REG_SPI_PS_INPUT_ADDR 0xA1B4u
#define AMD_REG_SPI_VS_OUT_CONFIG 0xA1B1u
#define AMD_REG_COMPUTE_PGM_LO 0x2E0Cu
#define AMD_REG_COMPUTE_PGM_HI 0x2E10u
#define AMD_REG_COMPUTE_PGM_RSRC1 0x2E12u
#define AMD_REG_COMPUTE_PGM_RSRC2 0x2E13u
#define AMD_REG_COMPUTE_RESOURCE_LIMITS 0x2E15u
#define AMD_REG_COMPUTE_NUM_THREAD_X 0x2E1Cu
#define AMD_REG_COMPUTE_USER_DATA_0 0x2E40u
#define AMD_REG_SQ_THREAD_TRACE_USERDATA_0 0x2C40u
#define AMD_REG_TA_CS_BC_BASE_ADDR 0xA41Cu
#define AMD_REG_SQ_BUF_RSRC_WORD0 0x2E80u
#define AMD_FUSION_MARKER 0x49584146u
#define AMD_SHADER_MAGIC 0x49584153u
#define AMD_KERNEL_PID 1u

typedef struct {
  ix_u32 id;
  ix_u32 size;
  ix_u32 hash;
  ix_u64 gpu;
  ix_u32 pman_handle;
} amd_shader_slot_t;

typedef struct {
  ix_u8  initialized;
  ix_u8  family;
  ix_u8  bound_objects;
  ix_u8  fusion_active;
  ix_u8  fusion_rr;
  ix_u8  legacy_path;
  ix_u8  compute_path;
  ix_u8  reserved0;
  ix_u32 sequence;
  ix_u32 fb_width;
  ix_u32 fb_height;
  ix_u32 fb_pitch;
  ix_u64 fb_gpu_addr;
  ix_u32 default_vs;
  ix_u32 default_ps;
  ix_u32 default_cs;
  ix_u32 shader_next;
  amd_shader_slot_t shaders[AMD_SHADER_SLOTS];
  amd_device_info_t dev_info;
  amd_vbo_slot_t vbos[AMD_VBO_SLOT_MAX];
  ix_u32 vbo_count;
  amd_dma_ib_t ib;
  ix_gpman_fusion_state_t fusion;
  ix_gpman_fusion_node_t fusion_nodes[IXGP_FUSION_MAX_NODES];
} amd_state_t;

static amd_state_t g_amd;

static ix_u32 amd_float_bits(float v) {
  ix_u32 out;
  memcpy(&out, &v, sizeof(out));
  return out;
}

static ix_u32 amd_int_to_flt(int n) {
  ix_u32 x, e, m;
  if (n == 0) return 0u;
  x = (ix_u32)(n < 0 ? -n : n);
#if IXG_COMPILER_MSVC
  e = 31u - ixg_lzcnt32(x);
#else
  e = 31u;
  { ix_u32 t = x;
    if (t >> 16) { e -= 16u; t >>= 16u; }
    if (t >> 8)  { e -= 8u;  t >>= 8u; }
    if (t >> 4)  { e -= 4u;  t >>= 4u; }
    if (t >> 2)  { e -= 2u;  t >>= 2u; }
    if (t >> 1)  { e -= 1u; }
  }
#endif
  m = (x << (23u - e)) & 0x7FFFFFu;
  return ((ix_u32)(n < 0) << 31u) | ((127u + e) << 23u) | m;
}

static ix_u32 amd_hash_blob(const ix_u8* data, ix_u32 size) {
  ix_u32 h = 2166136261u;
  ix_u32 i;
  for (i = 0u; i < size; i++) { h ^= data[i]; h *= 16777619u; }
  return h ? h : AMD_SHADER_MAGIC;
}

static ix_u64 amd_slot_mask(ix_u8 slot) {
  if (slot == 1u) return IXGP_MASK_IGPU;
  if (slot >= 2u && slot < 32u) return IXGP_MASK_DGPU((ix_u64)(slot - 2u));
  if (slot >= 32u) return IXGP_MASK_MUTANT((ix_u64)(slot - 32u));
  return IXGP_MASK_CPU;
}

static void amd_refresh_fusion(void) {
  ix_u8 i;
  ix_memset(&g_amd.fusion, 0, sizeof(g_amd.fusion));
  ix_memset(g_amd.fusion_nodes, 0, sizeof(g_amd.fusion_nodes));
  g_amd.fusion_active = 0u;
  if (ixg_pman_fusion_get(&g_amd.fusion) != 0 || !g_amd.fusion.enabled || g_amd.fusion.node_count == 0u) return;
  if (g_amd.fusion.node_count > IXGP_FUSION_MAX_NODES) g_amd.fusion.node_count = IXGP_FUSION_MAX_NODES;
  for (i = 0u; i < g_amd.fusion.node_count; i++) (void)ixg_pman_fusion_get_node(i, &g_amd.fusion_nodes[i]);
  g_amd.fusion_active = 1u;
}

static ix_u64 amd_select_fusion_mask(ix_u8 compute, ix_u32 pixel_count) {
  if (!g_amd.fusion_active || g_amd.fusion.node_count == 0u)
    return g_amd.fusion.fusion_mask ? g_amd.fusion.fusion_mask : IXGP_MASK_CPU;
  if (g_amd.fusion.mode == IX_GP_FUSION_MODE_COMPUTE && !compute) return amd_slot_mask(g_amd.fusion_nodes[0].slot);
  if (g_amd.fusion.mode == IX_GP_FUSION_MODE_SPLIT_FRAME) {
    if (compute) return g_amd.fusion.fusion_mask;
    if (g_amd.dev_info.pcie_gen < 3u || g_amd.dev_info.pcie_lanes < 8u) return amd_slot_mask(g_amd.fusion_nodes[0].slot);
    if (pixel_count > 0u && pixel_count < 2073600u) return amd_slot_mask(g_amd.fusion_nodes[0].slot);
    return g_amd.fusion.fusion_mask;
  }
  if (g_amd.fusion.mode == IX_GP_FUSION_MODE_COMPUTE || g_amd.fusion.mode == IX_GP_FUSION_MODE_MIXED)
    return g_amd.fusion.fusion_mask;
  if (g_amd.fusion.mode == IX_GP_FUSION_MODE_ALT_FRAME) {
    ix_u8 idx = g_amd.fusion_rr++;
    if (idx >= g_amd.fusion.node_count) { idx = 0u; g_amd.fusion_rr = 1u; }
    return amd_slot_mask(g_amd.fusion_nodes[idx].slot);
  }
  return amd_slot_mask(g_amd.fusion_nodes[0].slot);
}

static void amd_ib_push_pkt(ix_u32 op, const ix_u32* data, ix_u32 count);
static void amd_push_pkt(ix_u32 op, const ix_u32* data, ix_u32 count);

static void amd_emit_fusion_select(ix_u64 mask, ix_u8 compute) {
  ix_u32 data[5];
  if (!g_amd.fusion_active) return;
  data[0] = AMD_FUSION_MARKER;
  data[1] = (ix_u32)(mask & 0xFFFFFFFFu);
  data[2] = (ix_u32)(mask >> 32u);
  data[3] = (compute ? 1u : 0u) | ((ix_u32)g_amd.fusion.mode << 8u) | ((ix_u32)g_amd.fusion.frame_pacing << 16u);
  data[4] = ++g_amd.sequence;
  if (g_amd.ib.cpu_ptr && g_amd.ib.state == 1u) amd_ib_push_pkt(AMD_IT_NOP, data, 5u);
  else amd_push_pkt(AMD_IT_NOP, data, 5u);
}

static ix_u8 amd_family_from_device(ix_u16 dev, ix_u8 vendor1022) {
  if (vendor1022) return AMD_FAMILY_RDNA2;
  if ((dev >= 0x7380u && dev <= 0x739Fu) || (dev >= 0x7400u && dev <= 0x741Fu)) return AMD_FAMILY_CDNA;
  if (dev >= 0x7440u && dev <= 0x745Fu) return AMD_FAMILY_CDNA2;
  if (dev >= 0x7460u && dev <= 0x747Fu) return AMD_FAMILY_CDNA3;
  if (dev >= 0x7600u) return AMD_FAMILY_RDNA4;
  if (dev >= 0x7400u) return AMD_FAMILY_RDNA3;
  if (dev >= 0x7300u) return AMD_FAMILY_RDNA2;
  if (dev >= 0x7000u) return AMD_FAMILY_RDNA1;
  if (dev >= 0x6800u) return AMD_FAMILY_VEGA;
  if (dev >= 0x6600u) return AMD_FAMILY_POLARIS;
  if (dev >= 0x6000u) return AMD_FAMILY_GCN;
  if (dev >= 0x5000u) return AMD_FAMILY_EVERGREEN;
  if (dev >= 0x4000u) return AMD_FAMILY_R600;
  return AMD_FAMILY_LEGACY;
}

static void amd_detect_hw_caps(void) {
  amd_device_info_t* info = &g_amd.dev_info;
  memset(info, 0, sizeof(*info));
  info->family = g_amd.family;
  info->chipset = g_ixg_driver_hw.device_id;
  switch (g_amd.family) {
    case AMD_FAMILY_GCN:
      info->se_count = 2; info->sh_per_se = 2; info->cu_per_sh = 8;
      info->simd_per_cu = 4; info->wave_size = 64;
      info->max_waves_per_cu = 40; info->lds_per_cu = 65536;
      info->sgpr_per_cu = 512; info->vgpr_per_cu = 256; break;
    case AMD_FAMILY_POLARIS:
      info->se_count = 4; info->sh_per_se = 2; info->cu_per_sh = 9;
      info->simd_per_cu = 4; info->wave_size = 64;
      info->max_waves_per_cu = 40; info->lds_per_cu = 65536;
      info->sgpr_per_cu = 800; info->vgpr_per_cu = 256; break;
    case AMD_FAMILY_VEGA:
      info->se_count = 4; info->sh_per_se = 2; info->cu_per_sh = 8;
      info->simd_per_cu = 4; info->wave_size = 64;
      info->max_waves_per_cu = 40; info->lds_per_cu = 65536;
      info->sgpr_per_cu = 800; info->vgpr_per_cu = 256;
      info->has_dp_support = 1; break;
    case AMD_FAMILY_RDNA1:
      info->se_count = 2; info->sh_per_se = 2; info->cu_per_sh = 10;
      info->simd_per_cu = 2; info->wave_size = 32;
      info->max_waves_per_cu = 20; info->lds_per_cu = 131072;
      info->sgpr_per_cu = 128; info->vgpr_per_cu = 1024; break;
    case AMD_FAMILY_RDNA2:
      info->se_count = 4; info->sh_per_se = 2; info->cu_per_sh = 10;
      info->simd_per_cu = 2; info->wave_size = 32;
      info->max_waves_per_cu = 32; info->lds_per_cu = 131072;
      info->sgpr_per_cu = 128; info->vgpr_per_cu = 1536;
      info->has_rt_accelerator = 1; info->infinity_cache_mb = 128; break;
    case AMD_FAMILY_RDNA3:
      info->se_count = 6; info->sh_per_se = 2; info->cu_per_sh = 8;
      info->simd_per_cu = 2; info->wave_size = 32;
      info->max_waves_per_cu = 32; info->lds_per_cu = 131072;
      info->sgpr_per_cu = 128; info->vgpr_per_cu = 1536;
      info->has_rt_accelerator = 1; info->has_matrix_cores = 1;
      info->infinity_cache_mb = 96; break;
    case AMD_FAMILY_RDNA4:
      info->se_count = 4; info->sh_per_se = 2; info->cu_per_sh = 8;
      info->simd_per_cu = 2; info->wave_size = 32;
      info->max_waves_per_cu = 32; info->lds_per_cu = 131072;
      info->sgpr_per_cu = 128; info->vgpr_per_cu = 1536;
      info->has_rt_accelerator = 1; info->has_matrix_cores = 1;
      info->infinity_cache_mb = 64; break;
    case AMD_FAMILY_CDNA:
      info->se_count = 4; info->sh_per_se = 4; info->cu_per_sh = 16;
      info->simd_per_cu = 4; info->wave_size = 64;
      info->max_waves_per_cu = 40; info->lds_per_cu = 65536;
      info->has_matrix_cores = 1; info->has_dp_support = 1;
      info->is_cdna = 1; break;
    case AMD_FAMILY_CDNA2:
      info->se_count = 8; info->sh_per_se = 4; info->cu_per_sh = 14;
      info->simd_per_cu = 4; info->wave_size = 64;
      info->max_waves_per_cu = 40; info->lds_per_cu = 65536;
      info->has_matrix_cores = 1; info->has_dp_support = 1;
      info->is_cdna = 1; break;
    case AMD_FAMILY_CDNA3:
      info->se_count = 12; info->sh_per_se = 4; info->cu_per_sh = 16;
      info->simd_per_cu = 4; info->wave_size = 64;
      info->max_waves_per_cu = 40; info->lds_per_cu = 65536;
      info->has_matrix_cores = 1; info->has_dp_support = 1;
      info->is_cdna = 1; break;
    default:
      info->se_count = 1; info->sh_per_se = 1; info->cu_per_sh = 1;
      info->simd_per_cu = 1; info->wave_size = 64; break;
  }
  info->total_cu = (ix_u32)info->se_count * (ix_u32)info->sh_per_se * (ix_u32)info->cu_per_sh;
  info->total_wgp = info->total_cu / 2u;
  info->vram_mb = ixg_mutant_estimate_vram_mb(0x1002u, g_ixg_driver_hw.device_id, 0);
  if (info->vram_mb == 0u) info->vram_mb = 4096u;
  ixg_driver_detect_pcie_caps(g_ixg_driver_hw.bus, g_ixg_driver_hw.slot, g_ixg_driver_hw.func, &info->pcie_gen, &info->pcie_lanes);
}

static ix_u32 amd_pkt3(ix_u32 op, ix_u32 count) {
  return AMD_PACKET3 | ((op & 0xFFu) << 8u) | ((count ? count - 1u : 0u) & 0x3FFFu);
}

static void amd_ring_write_slot(const ix_u32 words[8]) {
  ix_u32 spins = AMD_FIFO_FREE_SPINS;
  ix_u8 ok = 0u;
  while (spins--) {
    if (ixg_driver_ring_used() < (IXG_DRIVER_CMD_RING_CAP - 2u)) { ok = 1u; break; }
    IXG_INLINE_PAUSE();
  }
  if (!ok) { g_ixg_driver_hw.submit_failed = 1u; return; }
  {
    ix_u32 idx = g_ixg_driver_hw.cmd_tail & (IXG_DRIVER_CMD_RING_CAP - 1u);
    ix_u32* out = (ix_u32*)&g_ixg_driver_cmd_ring[idx];
    ix_u32 i;
    for (i = 0u; i < 8u; i++) out[i] = words[i];
    g_ixg_driver_hw.cmd_tail = (g_ixg_driver_hw.cmd_tail + 1u) & (IXG_DRIVER_CMD_RING_CAP - 1u);
    g_ixg_driver_hw.cmd_seq++;
  }
}

static void amd_push_words(const ix_u32* words, ix_u32 count) {
  ix_u32 i = 0u;
  while (i < count) {
    ix_u32 slot[8] = {0,0,0,0,0,0,0,0};
    ix_u32 n = count - i; ix_u32 k;
    if (n > 8u) n = 8u;
    for (k = 0u; k < n; k++) slot[k] = words[i + k];
    amd_ring_write_slot(slot);
    if (g_ixg_driver_hw.submit_failed) break;
    i += n;
  }
  ixg_driver_kick_ring(0x1002u);
}

static ix_u64 amd_stage(const void* src, ix_u32 bytes) {
  ix_u64 gpu = ixg_driver_dma_stage_upload(src, bytes);
  if (gpu) ix_iommu_assert_safe(IX_IOMMU_DOMAIN_GPU, gpu, bytes, IX_IOMMU_FLAG_READ);
  return gpu ? gpu : (ix_u64)(uintptr_t)src;
}

static int amd_ring_validate_words(const ix_u32* words, ix_u32 count) {
  ix_u32 i = 0u;
  ix_u64 aperture_base;
  ix_u64 aperture_end;
  if (!words || count == 0u) return 0;
  if (count > 4096u) return 0;
  aperture_base = g_ixg_driver_hw.dma_aperture_base;
  aperture_end = aperture_base + (ix_u64)g_ixg_driver_hw.dma_aperture_mb * 1048576u;
  if (aperture_end <= aperture_base) aperture_end = (ix_u64)-1;
  while (i < count) {
    ix_u32 hdr = words[i];
    ix_u32 it_op = (hdr >> 30) & 0x3u;
    ix_u32 op = (hdr >> 8) & 0xFFu;
    ix_u32 dlen = ((hdr >> 16) & 0x3FFFu) + 2u;
    if (dlen == 0u || i + dlen > count) return 0;
    if (it_op == 0u) {
      if (op == 0x37u || op == 0x05u) return 0;
    }
    if (it_op == 3u && dlen >= 3u) {
      ix_u64 addr;
      if (op == 0x3Fu && dlen >= 4u) {
        addr = (ix_u64)words[i + 1u] | ((ix_u64)words[i + 2u] << 32u);
        if (addr < aperture_base || addr >= aperture_end) return 0;
      }
      if (op == 0x50u && dlen >= 9u) {
        addr = (ix_u64)words[i + 4u] | ((ix_u64)words[i + 5u] << 32u);
        if (addr < aperture_base || addr >= aperture_end) return 0;
      }
      if (op == 0x40u && dlen >= 6u) {
        addr = (ix_u64)words[i + 3u] | ((ix_u64)words[i + 4u] << 32u);
        if (addr < aperture_base || addr >= aperture_end) return 0;
      }
      if (op == 0x16u && dlen >= 5u) {
        addr = (ix_u64)words[i + 3u] | ((ix_u64)words[i + 4u] << 32u);
        if (addr < aperture_base || addr >= aperture_end) return 0;
      }
    }
    i += dlen;
  }
  return 1;
}

static void amd_ring_submit_mmio(const ix_u32* words, ix_u32 count) {
  ix_u64 gpu; ix_u32 bytes;
  if (!words || count == 0u || !g_ixg_driver_hw.ready || !g_ixg_driver_hw.mmio) return;
  if (!amd_ring_validate_words(words, count)) {
    g_ixg_driver_hw.submit_failed = 1u; return;
  }
  if (g_ixg_driver_hw.regs.status && ixg_driver_mmio_read(g_ixg_driver_hw.regs.status) == 0xFFFFFFFFu) {
    g_ixg_driver_hw.submit_failed = 1u; return;
  }
  bytes = count * sizeof(ix_u32);
  gpu = amd_stage(words, bytes);
  if (!gpu) return;
  if (g_ixg_driver_hw.regs.ring_base) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_base, (ix_u32)(gpu & 0xFFFFFFFFu));
  if (g_ixg_driver_hw.regs.ring_base_hi) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_base_hi, (ix_u32)(gpu >> 32u));
  if (g_ixg_driver_hw.regs.ring_size) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_size, bytes);
  if (g_ixg_driver_hw.regs.ring_tail) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_tail, g_ixg_driver_hw.ring_tail_is_bytes ? bytes : count);
  if (g_ixg_driver_hw.regs.ring_put) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_put, g_ixg_driver_hw.ring_tail_is_bytes ? bytes : count);
  if (g_ixg_driver_hw.regs.doorbell) ixg_driver_mmio_write(g_ixg_driver_hw.regs.doorbell, ++g_amd.sequence);
  ixg_driver_mmio_flush_posted();
}

static int amd_dma_ib_init(void) {
  ix_u32 handle = 0u; ix_u64 gpu = 0u; void* cpu = 0; ix_u8 tier = 0u;
  if (g_amd.legacy_path) return 0;
  cpu = malloc(AMD_DMA_IB_SIZE);
  if (!cpu) return -1;
  g_amd.ib.pman_handle = 0;
  g_amd.ib.gpu_addr = (ix_u64)(uintptr_t)cpu;
  g_amd.ib.cpu_ptr = (ix_u32*)cpu;
  g_amd.ib.capacity_dwords = AMD_DMA_IB_SIZE / 4u;
  g_amd.ib.state = 0u;
  return 0;
}

static void amd_ib_begin(void) {
  if (!g_amd.ib.cpu_ptr) return;
  g_amd.ib.used_dwords = 0u;
  g_amd.ib.state = 1u;
}

static void amd_ib_push(ix_u32 word) {
  if (!g_amd.ib.cpu_ptr || g_amd.ib.used_dwords >= g_amd.ib.capacity_dwords) return;
  g_amd.ib.cpu_ptr[g_amd.ib.used_dwords++] = word;
}

static void amd_ib_push_pkt(ix_u32 op, const ix_u32* data, ix_u32 count) {
  ix_u32 i;
  amd_ib_push(amd_pkt3(op, count));
  for (i = 0u; i < count; i++) amd_ib_push(data ? data[i] : 0u);
}

static void amd_ib_submit(void) {
  ix_u32 data[4];
  if (!g_amd.ib.cpu_ptr || g_amd.ib.used_dwords == 0u) return;
  data[0] = (ix_u32)(g_amd.ib.gpu_addr & 0xFFFFFFFFu);
  data[1] = (ix_u32)(g_amd.ib.gpu_addr >> 32u);
  data[2] = g_amd.ib.used_dwords;
  {
    ix_u32 buf[4];
    buf[0] = amd_pkt3(AMD_IT_INDIRECT_BUFFER, 3u);
    buf[1] = data[0]; buf[2] = data[1]; buf[3] = data[2];
    amd_push_words(buf, 4u);
    amd_ring_submit_mmio(buf, 4u);
  }
  g_amd.ib.state = 2u;
}

static void amd_push_pkt(ix_u32 op, const ix_u32* data, ix_u32 count) {
  ix_u32 buf[AMD_PUSH_MAX_DWORDS]; ix_u32 i;
  if (count + 1u > AMD_PUSH_MAX_DWORDS) { g_ixg_driver_hw.submit_failed = 1u; return; }
  buf[0] = amd_pkt3(op, count);
  for (i = 0u; i < count; i++) buf[i + 1u] = data ? data[i] : 0u;
  amd_push_words(buf, count + 1u);
  amd_ring_submit_mmio(buf, count + 1u);
}

static void amd_set_reg_packet(ix_u32 op, ix_u32 base, ix_u32 reg, const ix_u32* values, ix_u32 count) {
  ix_u32 data[64]; ix_u32 i;
  if (!values || count == 0u || count + 1u > 64u || reg < base) return;
  data[0] = (reg - base) >> 2u;
  for (i = 0u; i < count; i++) data[i + 1u] = values[i];
  if (g_amd.ib.cpu_ptr && g_amd.ib.state == 1u) amd_ib_push_pkt(op, data, count + 1u);
  else amd_push_pkt(op, data, count + 1u);
}

static void amd_set_context(ix_u32 reg, const ix_u32* values, ix_u32 count) { amd_set_reg_packet(AMD_IT_SET_CONTEXT_REG, 0xA000u, reg, values, count); }
static void amd_set_sh(ix_u32 reg, const ix_u32* values, ix_u32 count) { amd_set_reg_packet(AMD_IT_SET_SH_REG, 0x2C00u, reg, values, count); }
static void amd_set_config(ix_u32 reg, const ix_u32* values, ix_u32 count) { amd_set_reg_packet(AMD_IT_SET_CONFIG_REG, 0x2000u, reg, values, count); }

static amd_shader_slot_t* amd_find_shader(ix_u32 id) {
  ix_u32 i;
  for (i = 0u; i < AMD_SHADER_SLOTS; i++) if (g_amd.shaders[i].id == id) return &g_amd.shaders[i];
  return 0;
}

static amd_shader_slot_t* amd_alloc_shader_slot(ix_u32 id) {
  ix_u32 i;
  for (i = 0u; i < AMD_SHADER_SLOTS; i++) if (g_amd.shaders[i].id == 0u) { g_amd.shaders[i].id = id; return &g_amd.shaders[i]; }
  return 0;
}

static void amd_bind_shader_regs(void) {
  ix_u32 data[4];
  amd_shader_slot_t* vs = amd_find_shader(g_amd.default_vs);
  amd_shader_slot_t* ps = amd_find_shader(g_amd.default_ps);
  if (vs) {
    data[0] = (ix_u32)(vs->gpu >> 8u); data[1] = (ix_u32)(vs->gpu >> 40u);
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_LO_VS, data, 2u);
    data[0] = 0x00C00000u; data[1] = 0x00000010u;
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_RSRC1_VS, data, 2u);
  }
  if (ps) {
    data[0] = (ix_u32)(ps->gpu >> 8u); data[1] = (ix_u32)(ps->gpu >> 40u);
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_LO_PS, data, 2u);
    data[0] = 0x00C00000u; data[1] = 0x00000010u;
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_RSRC1_PS, data, 2u);
  }
}

static void amd_bind_render_target(void) {
  ix_u32 data[8];
  ix_u32 pitch = g_amd.fb_pitch ? g_amd.fb_pitch : 4096u;
  if (g_amd.fb_gpu_addr) ix_iommu_register_dma(IX_IOMMU_DOMAIN_GPU, g_amd.fb_gpu_addr,
      (ix_u32)(pitch * (g_amd.fb_height ? g_amd.fb_height : 768u)), IX_IOMMU_FLAG_WRITE | IX_IOMMU_FLAG_DEVICE);
  data[0] = 0u;
  amd_set_context(AMD_REG_DB_RENDER_CONTROL, data, 1u);
  data[0] = (ix_u32)(g_amd.fb_gpu_addr >> 8u);
  data[1] = ((pitch / 8u) - 1u) & 0x3FFFu;
  data[2] = ((pitch * (g_amd.fb_height ? g_amd.fb_height : 768u)) / 64u) & 0x3FFFFFu;
  data[3] = 0u;
  data[4] = g_amd.family >= AMD_FAMILY_RDNA2 ? 0x00001A00u : 0x1A000000u;
  data[5] = 0u; data[6] = 0u; data[7] = 0u;
  amd_set_context(AMD_REG_CB_COLOR0_BASE, data, 8u);
  data[0] = 0xFu; data[1] = 0xFu;
  amd_set_context(AMD_REG_CB_TARGET_MASK, data, 2u);
}

static void amd_bind_objects(void) {
  ix_u32 data[4];
  if (g_amd.bound_objects) return;
  amd_refresh_fusion();
  ixg_driver_program_dma_registers();
  ixg_driver_program_ring_registers();
  data[0] = 0xFFFFFFFFu;
  amd_set_config(AMD_REG_GRBM_GFX_INDEX, data, 1u);
  data[0] = 0x80000000u; data[1] = 0x80000000u;
  amd_push_pkt(AMD_IT_CONTEXT_CONTROL, data, 2u);
  amd_bind_render_target();
  amd_bind_shader_regs();
  g_amd.bound_objects = 1u;
}

static void amd_emit_scissor(int x, int y, int w, int h) {
  ix_u32 data[2];
  data[0] = ((ix_u32)(ix_u16)y << 16u) | (ix_u32)(ix_u16)x;
  data[1] = ((ix_u32)(ix_u16)(y + h) << 16u) | (ix_u32)(ix_u16)(x + w);
  amd_set_context(AMD_REG_PA_SC_WINDOW_SCISSOR_TL, data, 2u);
  data[0] = 0u;
  data[1] = ((g_amd.fb_height ? g_amd.fb_height : 768u) << 16u) | (g_amd.fb_width ? g_amd.fb_width : 1024u);
  amd_set_context(AMD_REG_PA_SC_SCREEN_SCISSOR_TL, data, 2u);
}

static void amd_flush(void) {
  ix_u32 data[5];
  amd_bind_objects();
  data[0] = 0x0000000Au; data[1] = 0xFFFFFFFFu; data[2] = 0xFFFFFFFFu; data[3] = 0u; data[4] = 0u;
  amd_push_pkt(g_amd.family >= AMD_FAMILY_RDNA2 ? AMD_IT_ACQUIRE_MEM : AMD_IT_SURFACE_SYNC, data, 5u);
  data[0] = ++g_amd.sequence;
  amd_push_pkt(AMD_IT_EVENT_WRITE, data, 1u);
  (void)ixg_driver_wait_idle(IXG_DRIVER_MMIO_POLL_SPINS);
}

static void amd_draw_rect(int x, int y, int w, int h, ix_u32 color) {
  ix_u32 data[8];
  if (w <= 0 || h <= 0) return;
  amd_bind_objects();
  amd_emit_fusion_select(amd_select_fusion_mask(0u, (ix_u32)(w * h)), 0u);
  amd_emit_scissor(x, y, w, h);
  if (g_amd.ib.cpu_ptr && !g_amd.legacy_path) {
    amd_ib_begin();
    data[0] = color; data[1] = amd_int_to_flt(x); data[2] = amd_int_to_flt(y); data[3] = amd_int_to_flt(w);
    amd_set_context(AMD_REG_SQ_THREAD_TRACE_USERDATA_0, data, 4u);
    data[0] = 4u; amd_set_context(AMD_REG_VGT_PRIMITIVE_TYPE, data, 1u);
    data[0] = 3u; amd_ib_push_pkt(AMD_IT_NUM_INSTANCES, data, 1u);
    data[0] = 3u; amd_ib_push_pkt(AMD_IT_DRAW_INDEX_AUTO, data, 1u);
    amd_ib_submit();
    return;
  }
  data[0] = color; data[1] = amd_int_to_flt(x); data[2] = amd_int_to_flt(y); data[3] = amd_int_to_flt(w);
  amd_set_context(AMD_REG_SQ_THREAD_TRACE_USERDATA_0, data, 4u);
  data[0] = 4u; amd_set_context(AMD_REG_VGT_PRIMITIVE_TYPE, data, 1u);
  data[0] = 3u; amd_push_pkt(AMD_IT_NUM_INSTANCES, data, 1u);
  data[0] = 3u; amd_push_pkt(AMD_IT_DRAW_INDEX_AUTO, data, 1u);
}

static void amd_bitblt(int dx, int dy, int w, int h, void* src, int sx, int sy) {
  ix_u64 gpu; ix_u64 dst; ix_u32 data[8]; ix_u32 src_pitch; ix_u32 dst_pitch;
  if (!src || w <= 0 || h <= 0 || sx < 0 || sy < 0) return;
  amd_bind_objects();
  amd_emit_fusion_select(amd_select_fusion_mask(0u, (ix_u32)(w * h)), 0u);
  src_pitch = (ix_u32)w * 4u;
  dst_pitch = g_amd.fb_pitch ? g_amd.fb_pitch : src_pitch;
  gpu = amd_stage(src, src_pitch * (ix_u32)h);
  dst = g_amd.fb_gpu_addr + (ix_u64)(ix_u32)dy * dst_pitch + (ix_u64)(ix_u32)dx * 4u;
  data[0] = (ix_u32)(gpu & 0xFFFFFFFFu); data[1] = (ix_u32)(gpu >> 32u);
  data[2] = (ix_u32)(dst & 0xFFFFFFFFu); data[3] = (ix_u32)(dst >> 32u);
  data[4] = src_pitch; data[5] = (ix_u32)h; data[6] = 0x00000010u; data[7] = dst_pitch;
  if (g_amd.ib.cpu_ptr && !g_amd.legacy_path) {
    amd_ib_begin();
    amd_ib_push_pkt(AMD_IT_DMA_DATA, data, 8u);
    amd_ib_submit();
    return;
  }
  amd_push_pkt(AMD_IT_DMA_DATA, data, 8u);
}

static int amd_rect_blend(int x, int y, int w, int h, ix_u32 color, ix_u8 premultiplied) {
  (void)premultiplied;
  if (w <= 0 || h <= 0) return -1;
  amd_draw_rect(x, y, w, h, color);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int amd_bitblt_alpha(int dx, int dy, int w, int h, const void* src, ix_u32 stride_bytes, ix_u8 premultiplied) {
  (void)stride_bytes; (void)premultiplied;
  if (!src || w <= 0 || h <= 0) return -1;
  amd_bitblt(dx, dy, w, h, (void*)src, 0, 0);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int amd_emit_triangles(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture) {
  ix_u64 gpu; ix_u32 total; ix_u32 bytes; ix_u32 data[8];
  if (!vertices || triangle_count == 0u) return -1;
  total = triangle_count * 3u;
  bytes = total * (ix_u32)sizeof(ixg_hw_vertex_t);
  amd_bind_objects();
  amd_emit_fusion_select(amd_select_fusion_mask(0u, 0u), 0u);
  if (g_amd.ib.cpu_ptr && !g_amd.legacy_path) {
    void* vbo_cpu = malloc(bytes);
    if (vbo_cpu) {
      memcpy(vbo_cpu, vertices, bytes);
      ix_u64 vbo_gpu = (ix_u64)(uintptr_t)vbo_cpu;
      amd_ib_begin();
      data[0] = (ix_u32)(vbo_gpu & 0xFFFFFFFFu); data[1] = (ix_u32)(vbo_gpu >> 32u);
      data[2] = bytes; data[3] = sizeof(ixg_hw_vertex_t); data[4] = texture ? 1u : 0u;
      amd_set_sh(AMD_REG_COMPUTE_USER_DATA_0, data, 5u);
      data[0] = 4u; amd_set_context(AMD_REG_VGT_PRIMITIVE_TYPE, data, 1u);
      data[0] = 0u; amd_ib_push_pkt(AMD_IT_INDEX_TYPE, data, 1u);
      data[0] = total; amd_ib_push_pkt(AMD_IT_NUM_INSTANCES, data, 1u);
      data[0] = total; amd_ib_push_pkt(AMD_IT_DRAW_INDEX_AUTO, data, 1u);
      amd_ib_submit();
      free(vbo_cpu);
      return g_ixg_driver_hw.submit_failed ? -1 : 0;
    }
  }
  gpu = amd_stage(vertices, bytes);
  data[0] = (ix_u32)(gpu & 0xFFFFFFFFu); data[1] = (ix_u32)(gpu >> 32u);
  data[2] = bytes; data[3] = sizeof(ixg_hw_vertex_t); data[4] = texture ? 1u : 0u;
  amd_set_sh(AMD_REG_COMPUTE_USER_DATA_0, data, 5u);
  if (texture && texture->pixels && texture->width > 0u && texture->height > 0u) {
    ix_u32 stride = texture->stride ? texture->stride : texture->width;
    ix_u64 tex_gpu = amd_stage(texture->pixels, stride * (ix_u32)texture->height * 4u);
    data[0] = (ix_u32)(tex_gpu & 0xFFFFFFFFu); data[1] = (ix_u32)(tex_gpu >> 32u);
    data[2] = ((ix_u32)texture->width << 16u) | texture->height;
    data[3] = stride * 4u; data[4] = texture->flags; data[5] = texture->mip_count;
    amd_set_sh(AMD_REG_COMPUTE_USER_DATA_0 + 0x20u, data, 6u);
  }
  data[0] = 4u; amd_set_context(AMD_REG_VGT_PRIMITIVE_TYPE, data, 1u);
  data[0] = 0u; amd_push_pkt(AMD_IT_INDEX_TYPE, data, 1u);
  data[0] = total; amd_push_pkt(AMD_IT_NUM_INSTANCES, data, 1u);
  data[0] = total; amd_push_pkt(AMD_IT_DRAW_INDEX_AUTO, data, 1u);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int amd_triangles(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count) { return amd_emit_triangles(vertices, triangle_count, 0); }
static int amd_triangles_tex(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture) {
  if (!texture) return -1;
  return amd_emit_triangles(vertices, triangle_count, texture);
}

static int amd_compute(const void* dispatch_desc) {
  const ixg_compute_dispatch_desc_t* desc = (const ixg_compute_dispatch_desc_t*)dispatch_desc;
  ix_u64 gpu; ix_u32 data[16]; amd_shader_slot_t* shader;
  if (!desc) return -1;
  amd_bind_objects();
  amd_emit_fusion_select(amd_select_fusion_mask(1u, 0u), 1u);
  if (!g_amd.compute_path) return -1;
  gpu = amd_stage(desc, sizeof(ixg_compute_dispatch_desc_t));
  shader = amd_find_shader(desc->kernel ? desc->kernel : g_amd.default_cs);
  if (g_amd.ib.cpu_ptr && !g_amd.legacy_path) {
    amd_ib_begin();
    if (shader) {
      data[0] = (ix_u32)(shader->gpu >> 8u); data[1] = (ix_u32)(shader->gpu >> 40u);
      amd_set_sh(AMD_REG_COMPUTE_PGM_LO, data, 2u);
    }
    data[0] = 0x00C00000u; data[1] = 0x00000090u; data[2] = 0x000001FFu;
    amd_set_sh(AMD_REG_COMPUTE_PGM_RSRC1, data, 3u);
    data[0] = (ix_u32)(gpu & 0xFFFFFFFFu); data[1] = (ix_u32)(gpu >> 32u);
    data[2] = (ix_u32)(desc->device_mask & 0xFFFFFFFFu); data[3] = (ix_u32)(desc->device_mask >> 32u);
    data[4] = (ix_u32)(uintptr_t)desc->dst; data[5] = (ix_u32)(uintptr_t)desc->src0;
    data[6] = (ix_u32)(uintptr_t)desc->src1; data[7] = (ix_u32)(uintptr_t)desc->src2;
    amd_set_sh(AMD_REG_COMPUTE_USER_DATA_0, data, 8u);
    data[0] = 64u; data[1] = 1u; data[2] = 1u;
    amd_set_sh(AMD_REG_COMPUTE_NUM_THREAD_X, data, 3u);
    data[0] = desc->x ? desc->x : 1u; data[1] = desc->y ? desc->y : 1u; data[2] = desc->z ? desc->z : 1u;
    amd_ib_push_pkt(AMD_IT_DISPATCH_DIRECT, data, 3u);
    amd_ib_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  if (shader) {
    data[0] = (ix_u32)(shader->gpu >> 8u); data[1] = (ix_u32)(shader->gpu >> 40u);
    amd_set_sh(AMD_REG_COMPUTE_PGM_LO, data, 2u);
  }
  data[0] = 0x00C00000u; data[1] = 0x00000090u; data[2] = 0x000001FFu;
  amd_set_sh(AMD_REG_COMPUTE_PGM_RSRC1, data, 3u);
  data[0] = (ix_u32)(gpu & 0xFFFFFFFFu); data[1] = (ix_u32)(gpu >> 32u);
  data[2] = (ix_u32)(desc->device_mask & 0xFFFFFFFFu); data[3] = (ix_u32)(desc->device_mask >> 32u);
  data[4] = (ix_u32)(uintptr_t)desc->dst; data[5] = (ix_u32)(uintptr_t)desc->src0;
  data[6] = (ix_u32)(uintptr_t)desc->src1; data[7] = (ix_u32)(uintptr_t)desc->src2;
  amd_set_sh(AMD_REG_COMPUTE_USER_DATA_0, data, 8u);
  data[0] = 64u; data[1] = 1u; data[2] = 1u;
  amd_set_sh(AMD_REG_COMPUTE_NUM_THREAD_X, data, 3u);
  data[0] = desc->x ? desc->x : 1u; data[1] = desc->y ? desc->y : 1u; data[2] = desc->z ? desc->z : 1u;
  amd_push_pkt(AMD_IT_DISPATCH_DIRECT, data, 3u);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int amd_trace(const void* trace_desc) { return amd_compute(trace_desc); }
static int amd_control(const void* control_desc, void* control_result) { (void)control_result; return amd_compute(control_desc); }

static void amd_seed_shader(ix_u32 id, ix_u32 tag) {
  ix_u32 code[16]; amd_shader_slot_t* slot; ix_u32 i;
  for (i = 0u; i < 16u; i++) code[i] = AMD_SHADER_MAGIC ^ tag ^ (i * 0x9E3779B9u);
  slot = amd_alloc_shader_slot(id);
  if (slot) { slot->size = sizeof(code); slot->hash = amd_hash_blob((const ix_u8*)code, sizeof(code)); slot->gpu = amd_stage(code, sizeof(code)); }
}

int ixg_backend_amd_get_device_info(amd_device_info_t* out_info) {
  if (!out_info) return -1;
  *out_info = g_amd.dev_info;
  return 0;
}

int ixg_backend_amd_create_vbo(const void* vertex_data, ix_u32 size_bytes, ix_u32 stride, ix_u32* out_vbo_id) {
  amd_vbo_slot_t* vbo;
  if (!vertex_data || size_bytes == 0u || !out_vbo_id || stride == 0u) return -1;
  if (g_amd.vbo_count >= AMD_VBO_SLOT_MAX) return -1;
  vbo = &g_amd.vbos[g_amd.vbo_count];
  memset(vbo, 0, sizeof(*vbo));
  vbo->cpu_ptr = malloc(size_bytes);
  if (!vbo->cpu_ptr) return -1;
  memcpy(vbo->cpu_ptr, vertex_data, size_bytes);
  vbo->size_bytes = size_bytes; vbo->stride_bytes = stride;
  vbo->vertex_count = size_bytes / stride; vbo->bound = 1u;
  g_amd.vbo_count++;
  *out_vbo_id = g_amd.vbo_count;
  return 0;
}

void ixg_backend_amd_destroy_vbo(ix_u32 vbo_id) {
  amd_vbo_slot_t* vbo;
  if (vbo_id == 0u || vbo_id > g_amd.vbo_count) return;
  vbo = &g_amd.vbos[vbo_id - 1u];
  if (!vbo->bound) return;
  if (vbo->cpu_ptr) free(vbo->cpu_ptr);
  memset(vbo, 0, sizeof(*vbo));
}

int ixg_backend_amd_draw_vbo(ix_u32 vbo_id, ix_u32 first, ix_u32 count, ix_u32 topology) {
  amd_vbo_slot_t* vbo; ix_u32 data[8];
  if (vbo_id == 0u || vbo_id > g_amd.vbo_count) return -1;
  vbo = &g_amd.vbos[vbo_id - 1u];
  if (!vbo->bound || first + count > vbo->vertex_count) return -1;
  amd_bind_objects();
  if (g_amd.ib.cpu_ptr && !g_amd.legacy_path) {
    amd_ib_begin();
    data[0] = (ix_u32)(vbo->gpu_addr & 0xFFFFFFFFu); data[1] = (ix_u32)(vbo->gpu_addr >> 32u);
    data[2] = vbo->size_bytes; data[3] = vbo->stride_bytes; data[4] = 0u;
    amd_set_sh(AMD_REG_COMPUTE_USER_DATA_0, data, 5u);
    data[0] = topology; amd_set_context(AMD_REG_VGT_PRIMITIVE_TYPE, data, 1u);
    data[0] = count; amd_ib_push_pkt(AMD_IT_NUM_INSTANCES, data, 1u);
    data[0] = count; amd_ib_push_pkt(AMD_IT_DRAW_INDEX_AUTO, data, 1u);
    amd_ib_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  return -1;
}

int ixg_backend_amd_load_pm_firmware(const void* fw_data, ix_u32 fw_size) {
  ix_u32 id;
  if (!fw_data || fw_size < 0x100u || fw_size > (4u * 1024u * 1024u)) return -1;
  if (ixg_backend_amd_upload_shader((const ix_u8*)fw_data, fw_size, &id) != 0) return -1;
  g_amd.default_cs = id;
  g_amd.bound_objects = 0u;
  return 0;
}

void ixg_backend_amd_post_init(void) {
  memset(&g_amd, 0, sizeof(g_amd));
  g_amd.initialized = 1u;
  g_amd.family = amd_family_from_device(g_ixg_driver_hw.device_id, g_ixg_driver_hw.vendor_id == 0x1022u);
  g_amd.legacy_path = g_amd.family < AMD_FAMILY_GCN ? 1u : 0u;
  g_amd.compute_path = g_amd.family >= AMD_FAMILY_GCN ? 1u : 0u;
  g_amd.fb_width = 1024u; g_amd.fb_height = 768u; g_amd.fb_pitch = 4096u; g_amd.fb_gpu_addr = 0u;
  g_amd.shader_next = 4u; g_amd.default_vs = 1u; g_amd.default_ps = 2u; g_amd.default_cs = 3u;
  amd_detect_hw_caps();
  if (!g_amd.legacy_path) (void)amd_dma_ib_init();
  amd_seed_shader(g_amd.default_vs, 0x56532020u);
  amd_seed_shader(g_amd.default_ps, 0x50532020u);
  amd_seed_shader(g_amd.default_cs, 0x43532020u);
  {
    ix_u32 data[8];
    ixg_driver_program_dma_registers();
    ixg_driver_program_ring_registers();
    data[0] = 0u; amd_push_pkt(AMD_IT_NOP, data, 1u);
    data[0] = ++g_amd.sequence; amd_push_pkt(AMD_IT_EVENT_WRITE, data, 1u);
  }
  amd_bind_objects();
}

void ixg_backend_amd_shutdown(void) {
  ix_u32 i;
  for (i = 0u; i < g_amd.vbo_count; i++) {
    if (g_amd.vbos[i].bound && g_amd.vbos[i].cpu_ptr) free(g_amd.vbos[i].cpu_ptr);
  }
  if (g_amd.ib.cpu_ptr) free(g_amd.ib.cpu_ptr);
  memset(&g_amd, 0, sizeof(g_amd));
}

int ixg_backend_amd_upload_shader(const ix_u8* binary, ix_u32 binary_size, ix_u32* out_shader_id) {
  ix_u32 id; amd_shader_slot_t* slot;
  if (!binary || binary_size == 0u || !out_shader_id) return -1;
  id = g_amd.shader_next++;
  if (id == 0u) id = g_amd.shader_next++;
  slot = amd_alloc_shader_slot(id);
  if (!slot) return -1;
  slot->size = binary_size;
  slot->hash = amd_hash_blob(binary, binary_size);
  slot->gpu = amd_stage(binary, binary_size);
  *out_shader_id = id;
  return 0;
}

void ixg_backend_amd_delete_shader(ix_u32 shader_id) {
  amd_shader_slot_t* slot = amd_find_shader(shader_id);
  if (!slot) return;
  memset(slot, 0, sizeof(*slot));
}

static ix_u32 amd_isa_target(void) {
  switch (g_amd.family) {
    case AMD_FAMILY_RDNA4:
    case AMD_FAMILY_RDNA3: return IXG_ISA_AMD_RDNA3;
    case AMD_FAMILY_RDNA2: return IXG_ISA_AMD_RDNA2;
    case AMD_FAMILY_RDNA1: return IXG_ISA_AMD_RDNA1;
    default:               return IXG_ISA_AMD_GCN;
  }
}

static int amd_compile_ir_stage(const void* ir_bytecode, ix_u32 ir_size,
                                ix_u32 stage_mask, ix_u8 shader_type,
                                ix_u64* out_gpu, ix_u32* out_size,
                                ix_u32* out_validated) {
  static ixg_shader_program_t prog;
  static ixg_isa_binary_t bin;
  const ixg_shader_inst_t* src;
  ix_u32 inst_count, i;
  ix_u64 gpu;
  if (out_validated) *out_validated = 0u;
  if (!ir_bytecode || ir_size == 0u || !out_gpu) return -1;
  if ((ir_size % (ix_u32)sizeof(ixg_shader_inst_t)) != 0u) return -1;
  inst_count = ir_size / (ix_u32)sizeof(ixg_shader_inst_t);
  if (inst_count == 0u || inst_count > IXG_SHADER_MAX_INSTRUCTIONS) return -1;
  memset(&prog, 0, sizeof(prog));
  prog.stage_mask = stage_mask;
  prog.instruction_count = inst_count;
  src = (const ixg_shader_inst_t*)ir_bytecode;
  for (i = 0u; i < inst_count; i++) {
    prog.instructions[i].opcode = src[i].op;
    prog.instructions[i].dst = src[i].dst;
    prog.instructions[i].src[0] = src[i].src0;
    prog.instructions[i].src[1] = src[i].src1;
    prog.instructions[i].src[2] = src[i].src2;
    prog.instructions[i].write_mask = src[i].write_mask;
    prog.instructions[i].flags = src[i].flags;
    prog.instructions[i].imm = src[i].imm;
  }
  if (ixg_isa_encode(&prog, IXG_ISA_VENDOR_AMD, shader_type, amd_isa_target(), &bin) != 0) return -2;
  if (bin.byte_len == 0u) return -3;
  gpu = amd_stage(bin.bytes, bin.byte_len);
  if (!gpu) return -4;
  *out_gpu = gpu;
  if (out_size) *out_size = bin.byte_len;
  if (out_validated) *out_validated = bin.validated;
  return 0;
}

static int amd_bind_shaders(const ixg_hw_shader_bind_t* bind) {
  ix_u64 vs_gpu = 0u, ps_gpu = 0u;
  ix_u32 vs_valid = 0u, ps_valid = 0u;
  amd_shader_slot_t* vs;
  amd_shader_slot_t* ps;
  if (!bind || !bind->vs_bytecode || bind->vs_size == 0u ||
      !bind->ps_bytecode || bind->ps_size == 0u) return IXG_BIND_ERROR;
  amd_bind_objects();
  if (g_ixg_driver_hw.submit_failed) return IXG_BIND_ERROR;
  if (amd_compile_ir_stage(bind->vs_bytecode, bind->vs_size,
                           (ix_u32)IXG_SHADER_STAGE_3D_VERTEX,
                           (ix_u8)0u, &vs_gpu, 0, &vs_valid) != 0 ||
      amd_compile_ir_stage(bind->ps_bytecode, bind->ps_size,
                           (ix_u32)IXG_SHADER_STAGE_3D_PIXEL,
                           (ix_u8)1u, &ps_gpu, 0, &ps_valid) != 0) {
    return IXG_BIND_SOFTWARE_FALLBACK;
  }
  if (!vs_valid || !ps_valid) return IXG_BIND_SOFTWARE_FALLBACK;
  if (!vs_gpu || !ps_gpu) return IXG_BIND_ERROR;
  vs = amd_find_shader(g_amd.default_vs);
  ps = amd_find_shader(g_amd.default_ps);
  if (vs) { vs->gpu = vs_gpu; }
  if (ps) { ps->gpu = ps_gpu; }
  {
    ix_u32 data[4];
    data[0] = (ix_u32)((vs_gpu >> 8) & 0xFFFFFFFFu);
    data[1] = (ix_u32)((vs_gpu >> 40) & 0xFFu);
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_LO_VS, data, 2u);
    data[0] = 0x00C00000u; data[1] = 0x00000010u;
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_RSRC1_VS, data, 2u);
    data[0] = (ix_u32)((ps_gpu >> 8) & 0xFFFFFFFFu);
    data[1] = (ix_u32)((ps_gpu >> 40) & 0xFFu);
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_LO_PS, data, 2u);
    data[0] = 0x00C00000u; data[1] = 0x00000010u;
    amd_set_sh(AMD_REG_SPI_SHADER_PGM_RSRC1_PS, data, 2u);
  }
  ixg_driver_kick_ring(0x1002u);
  return g_ixg_driver_hw.submit_failed ? IXG_BIND_ERROR : IXG_BIND_OK;
}

void ixg_backend_amd_init_ops(ixg_hw_ops_t* out_ops) {
  if (!out_ops) return;
  memset(out_ops, 0, sizeof(*out_ops));
  out_ops->hw_draw_rect = amd_draw_rect;
  out_ops->hw_bitblt = amd_bitblt;
  out_ops->hw_draw_rect_blend = amd_rect_blend;
  out_ops->hw_bitblt_alpha = amd_bitblt_alpha;
  out_ops->hw_flush = amd_flush;
  out_ops->hw_draw_triangles = amd_triangles;
  out_ops->hw_draw_triangles_textured = amd_triangles_tex;
  out_ops->hw_compute_dispatch = amd_compute;
  out_ops->hw_trace_rays = amd_trace;
  out_ops->hw_control_dispatch = amd_control;
  out_ops->hw_bind_shaders = amd_bind_shaders;
}