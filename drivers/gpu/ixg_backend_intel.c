#include "ixg_backend_intel.h"
#include "ixg_compat.h"
#include "ixg_compute.h"
#include "ixg_mutants.h"
#include "ixg_isa.h"
#include "ixg_isa_encode.h"
#include "ixg_pman.h"
#include <string.h>

#define INTEL_FIFO_FREE_SPINS 20000u
#define INTEL_PUSH_MAX_DWORDS 256u
#define INTEL_MI_NOOP 0x00000000u
#define INTEL_MI_BATCH_BUFFER_END 0x05000000u
#define INTEL_MI_FLUSH_DW 0x26000000u
#define INTEL_MI_STORE_DWORD_IMM 0x20000000u
#define INTEL_MI_LOAD_REGISTER_IMM 0x11000000u
#define INTEL_MI_SEMAPHORE_MBOX 0x16000000u
#define INTEL_MI_USER_INTERRUPT 0x02000000u
#define INTEL_PIPE_CONTROL 0x7A000000u
#define INTEL_STATE_BASE_ADDRESS 0x61010000u
#define INTEL_3DSTATE_VF_STATISTICS 0x780B0000u
#define INTEL_3DSTATE_VERTEX_BUFFERS 0x78080000u
#define INTEL_3DSTATE_VERTEX_ELEMENTS 0x78090000u
#define INTEL_3DSTATE_INDEX_BUFFER 0x780A0000u
#define INTEL_3DSTATE_VS 0x78100000u
#define INTEL_3DSTATE_HS 0x781B0000u
#define INTEL_3DSTATE_TE 0x781C0000u
#define INTEL_3DSTATE_DS 0x781D0000u
#define INTEL_3DSTATE_GS 0x78110000u
#define INTEL_3DSTATE_PS 0x78200000u
#define INTEL_3DSTATE_STREAMOUT 0x781E0000u
#define INTEL_3DSTATE_BINDING_TABLE_POINTERS_PS 0x78290000u
#define INTEL_3DSTATE_SAMPLER_STATE_POINTERS_PS 0x782F0000u
#define INTEL_3DSTATE_VIEWPORT_STATE_POINTERS_CC 0x78230000u
#define INTEL_3DSTATE_SCISSOR_STATE_POINTERS 0x78150000u
#define INTEL_3DSTATE_BLEND_STATE_POINTERS 0x78240000u
#define INTEL_3DSTATE_DEPTH_STENCIL_STATE_POINTERS 0x78250000u
#define INTEL_3DSTATE_WM 0x78140000u
#define INTEL_3DSTATE_PS_BLEND 0x78200000u
#define INTEL_3DSTATE_SF 0x78130000u
#define INTEL_3DSTATE_RASTER 0x78500000u
#define INTEL_3DSTATE_CLIP 0x78120000u
#define INTEL_3DSTATE_DRAWING_RECTANGLE 0x79000000u
#define INTEL_3DSTATE_CONSTANT_VS 0x78150000u
#define INTEL_3DSTATE_CONSTANT_PS 0x78170000u
#define INTEL_3DPRIMITIVE 0x7B000000u
#define INTEL_MEDIA_VFE_STATE 0x70000000u
#define INTEL_MEDIA_INTERFACE_DESCRIPTOR_LOAD 0x70020000u
#define INTEL_GPGPU_WALKER 0x71050000u
#define INTEL_COMPUTE_WALKER 0x72050000u
#define INTEL_XY_COLOR_BLT 0x54000000u
#define INTEL_XY_SRC_COPY_BLT 0x53000000u
#define INTEL_XY_SETUP_BLT 0x01000000u
#define INTEL_MEM_FENCE 0x00002000u
#define INTEL_FUSION_MARKER 0x49584946u
#define INTEL_SHADER_MAGIC 0x49584953u
#define INTEL_KERNEL_PID 1u
#define INTEL_MI_BATCH_BUFFER_START 0x18800000u
#define INTEL_FORCEWAKE_MT 0x0A188u
#define INTEL_FORCEWAKE_ACK_MT 0x130044u
#define INTEL_FORCEWAKE_RENDER 0x0A278u
#define INTEL_FORCEWAKE_MEDIA 0x0A270u
#define INTEL_GT_THREAD_STATUS 0x13805Cu
#define INTEL_PIPELINE_SELECT_CMD 0x69040000u
#define INTEL_3DSTATE_VE_FMT_R32G32B32A32_FLOAT 0x000u
#define INTEL_3DSTATE_VE_FMT_R32G32_FLOAT 0x085u
#define INTEL_3DSTATE_VE_FMT_R8G8B8A8_UNORM 0x0C7u
#define INTEL_3DSTATE_VE_FMT_R32_UINT 0x0E5u
#define INTEL_VE_COMP_STORE_SRC 1u
#define INTEL_VE_COMP_STORE_0 2u
#define INTEL_VE_COMP_STORE_1_FP 3u
#define INTEL_COLD_START_FORCEWAKE_SPINS 5000u
#define INTEL_COLD_START_STATUS_SPINS 10000u

typedef struct {
  ix_u32 id;
  ix_u32 size;
  ix_u32 hash;
  ix_u64 gpu;
  ix_u32 pman_handle;
} intel_shader_slot_t;

typedef struct {
  ix_u8  initialized;
  ix_u8  gen;
  ix_u8  bound_objects;
  ix_u8  legacy_blt;
  ix_u8  compute_path;
  ix_u8  fusion_active;
  ix_u8  fusion_rr;
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
  intel_shader_slot_t shaders[INTEL_SHADER_SLOTS];
  intel_device_info_t dev_info;
  intel_vbo_slot_t vbos[INTEL_VBO_SLOT_MAX];
  ix_u32 vbo_count;
  intel_batch_buffer_t bb;
  ix_gpman_fusion_state_t fusion;
  ix_gpman_fusion_node_t fusion_nodes[IXGP_FUSION_MAX_NODES];
} intel_state_t;

static intel_state_t g_intel;

static ix_u32 intel_float_bits(float v) {
  ix_u32 out;
  memcpy(&out, &v, sizeof(out));
  return out;
}

static ix_u32 intel_hash_blob(const ix_u8* data, ix_u32 size) {
  ix_u32 h = 2166136261u; ix_u32 i;
  for (i = 0u; i < size; i++) { h ^= data[i]; h *= 16777619u; }
  return h ? h : INTEL_SHADER_MAGIC;
}

static ix_u64 intel_slot_mask(ix_u8 slot) {
  if (slot == 1u) return IXGP_MASK_IGPU;
  if (slot >= 2u && slot < 32u) return IXGP_MASK_DGPU((ix_u64)(slot - 2u));
  if (slot >= 32u) return IXGP_MASK_MUTANT((ix_u64)(slot - 32u));
  return IXGP_MASK_CPU;
}

static void intel_refresh_fusion(void) {
  ix_u8 i;
  ix_memset(&g_intel.fusion, 0, sizeof(g_intel.fusion));
  ix_memset(g_intel.fusion_nodes, 0, sizeof(g_intel.fusion_nodes));
  g_intel.fusion_active = 0u;
  if (ixg_pman_fusion_get(&g_intel.fusion) != 0 || !g_intel.fusion.enabled || g_intel.fusion.node_count == 0u) return;
  if (g_intel.fusion.node_count > IXGP_FUSION_MAX_NODES) g_intel.fusion.node_count = IXGP_FUSION_MAX_NODES;
  for (i = 0u; i < g_intel.fusion.node_count; i++) (void)ixg_pman_fusion_get_node(i, &g_intel.fusion_nodes[i]);
  g_intel.fusion_active = 1u;
}

static ix_u64 intel_select_fusion_mask(ix_u8 compute, ix_u32 pixel_count) {
  if (!g_intel.fusion_active || g_intel.fusion.node_count == 0u)
    return g_intel.fusion.fusion_mask ? g_intel.fusion.fusion_mask : IXGP_MASK_CPU;
  if (g_intel.fusion.mode == IX_GP_FUSION_MODE_COMPUTE && !compute) return intel_slot_mask(g_intel.fusion_nodes[0].slot);
  if (g_intel.fusion.mode == IX_GP_FUSION_MODE_SPLIT_FRAME) {
    if (compute) return g_intel.fusion.fusion_mask;
    if (g_intel.dev_info.pcie_gen < 3u || g_intel.dev_info.pcie_lanes < 8u) return intel_slot_mask(g_intel.fusion_nodes[0].slot);
    if (pixel_count > 0u && pixel_count < 2073600u) return intel_slot_mask(g_intel.fusion_nodes[0].slot);
    return g_intel.fusion.fusion_mask;
  }
  if (g_intel.fusion.mode == IX_GP_FUSION_MODE_COMPUTE || g_intel.fusion.mode == IX_GP_FUSION_MODE_MIXED)
    return g_intel.fusion.fusion_mask;
  if (g_intel.fusion.mode == IX_GP_FUSION_MODE_ALT_FRAME) {
    ix_u8 idx = g_intel.fusion_rr++;
    if (idx >= g_intel.fusion.node_count) { idx = 0u; g_intel.fusion_rr = 1u; }
    return intel_slot_mask(g_intel.fusion_nodes[idx].slot);
  }
  return intel_slot_mask(g_intel.fusion_nodes[0].slot);
}

static void intel_push_batch(const ix_u32* data, ix_u32 count);

static void intel_emit_fusion_select(ix_u64 mask, ix_u8 compute) {
  ix_u32 data[8];
  if (!g_intel.fusion_active) return;
  data[0] = INTEL_MI_NOOP;
  data[1] = INTEL_FUSION_MARKER;
  data[2] = (ix_u32)(mask & 0xFFFFFFFFu);
  data[3] = (ix_u32)(mask >> 32u);
  data[4] = (compute ? 1u : 0u) | ((ix_u32)g_intel.fusion.mode << 8u) | ((ix_u32)g_intel.fusion.frame_pacing << 16u);
  data[5] = ++g_intel.sequence;
  intel_push_batch(data, 6u);
}

static ix_u8 intel_gen_from_device(ix_u16 dev) {
  if (dev >= 0xE200u || (dev >= 0xD200u && dev <= 0xDFFFu)) return INTEL_GEN_XE3;
  if (dev >= 0xB500u && dev <= 0xBFFFu) return INTEL_GEN_XE2;
  if ((dev >= 0x5600u && dev <= 0x56FFu) || (dev >= 0x4F80u && dev <= 0x4FFFu)) return INTEL_GEN_XE_HPG;
  if ((dev >= 0x7D00u && dev <= 0xA7FFu) || dev >= 0x4900u) return INTEL_GEN_XE_LP;
  if (dev >= 0x1900u) return INTEL_GEN_SKYLAKE;
  if (dev >= 0x1600u) return INTEL_GEN_BROADWELL;
  if (dev >= 0x0400u) return INTEL_GEN_HASWELL;
  if (dev >= 0x0100u) return INTEL_GEN_I965;
  return INTEL_GEN_LEGACY;
}

static void intel_detect_hw_caps(void) {
  intel_device_info_t* info = &g_intel.dev_info;
  memset(info, 0, sizeof(*info));
  info->gen = g_intel.gen;
  info->chipset = g_ixg_driver_hw.device_id;
  switch (g_intel.gen) {
    case INTEL_GEN_SKYLAKE:
      info->slice_count = 1; info->subslice_per_slice = 3; info->eu_per_subslice = 8;
      info->threads_per_eu = 7; info->shared_local_mem = 65536; break;
    case INTEL_GEN_XE_LP:
      info->slice_count = 1; info->subslice_per_slice = 6; info->eu_per_subslice = 16;
      info->threads_per_eu = 7; info->shared_local_mem = 65536;
      info->l3_cache_bytes = 3145728u; break;
    case INTEL_GEN_XE_HPG:
      info->slice_count = 4; info->subslice_per_slice = 4; info->eu_per_subslice = 16;
      info->threads_per_eu = 8; info->shared_local_mem = 65536;
      info->has_ray_tracing = 1; info->has_xe_matrix = 1; info->is_dgpu = 1;
      info->l3_cache_bytes = 16777216u; info->xe_core_count = 32u; break;
    case INTEL_GEN_XE2:
      info->slice_count = 4; info->subslice_per_slice = 8; info->eu_per_subslice = 16;
      info->threads_per_eu = 8; info->shared_local_mem = 131072;
      info->has_ray_tracing = 1; info->has_xe_matrix = 1; info->is_dgpu = 1;
      info->l3_cache_bytes = 36700160u; info->xe_core_count = 64u; break;
    case INTEL_GEN_XE3:
      info->slice_count = 8; info->subslice_per_slice = 8; info->eu_per_subslice = 16;
      info->threads_per_eu = 8; info->shared_local_mem = 131072;
      info->has_ray_tracing = 1; info->has_xe_matrix = 1; info->has_npu = 1; info->npu_tops = 40u;
      info->is_dgpu = 1; info->l3_cache_bytes = 36700160u; info->xe_core_count = 128u; break;
    default:
      info->slice_count = 1; info->subslice_per_slice = 2; info->eu_per_subslice = 8;
      info->threads_per_eu = 7; info->shared_local_mem = 32768; break;
  }
  info->total_eu = (ix_u32)info->slice_count * (ix_u32)info->subslice_per_slice * (ix_u32)info->eu_per_subslice;
  info->total_threads = info->total_eu * (ix_u32)info->threads_per_eu;
  info->vram_mb = ixg_mutant_estimate_vram_mb(0x8086u, g_ixg_driver_hw.device_id, 0);
  if (info->vram_mb == 0u) info->vram_mb = info->is_dgpu ? 8192u : 256u;
  ixg_driver_detect_pcie_caps(g_ixg_driver_hw.bus, g_ixg_driver_hw.slot, g_ixg_driver_hw.func, &info->pcie_gen, &info->pcie_lanes);
}

static void intel_ring_write_slot(const ix_u32 words[8]) {
  ix_u32 spins = INTEL_FIFO_FREE_SPINS;
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

static void intel_push_words(const ix_u32* words, ix_u32 count) {
  ix_u32 i = 0u;
  while (i < count) {
    ix_u32 slot[8] = {0,0,0,0,0,0,0,0};
    ix_u32 n = count - i; ix_u32 k;
    if (n > 8u) n = 8u;
    for (k = 0u; k < n; k++) slot[k] = words[i + k];
    intel_ring_write_slot(slot);
    if (g_ixg_driver_hw.submit_failed) break;
    i += n;
  }
  ixg_driver_kick_ring(0x8086u);
}

static ix_u64 intel_stage(const void* src, ix_u32 bytes) {
  ix_u64 gpu = ixg_driver_dma_stage_upload(src, bytes);
  if (gpu) ix_iommu_assert_safe(IX_IOMMU_DOMAIN_GPU, gpu, bytes, IX_IOMMU_FLAG_READ);
  return gpu ? gpu : (ix_u64)(uintptr_t)src;
}

static int intel_ring_validate_words(const ix_u32* words, ix_u32 count) {
  ix_u32 i = 0u;
  ix_u64 aperture_base;
  ix_u64 aperture_end;
  if (!words || count == 0u) return 0;
  if (count > 4096u) return 0;
  aperture_base = g_ixg_driver_hw.dma_aperture_base;
  aperture_end = aperture_base + (ix_u64)g_ixg_driver_hw.dma_aperture_mb * 1048576u;
  if (aperture_end <= aperture_base) aperture_end = (ix_u64)-1;
  while (i < count) {
    ix_u32 w = words[i];
    ix_u32 cmd_type = (w >> 29) & 0x7u;
    ix_u32 op = (w >> 23) & 0x3Fu;
    ix_u32 dlen;
    if (cmd_type == 0u) {
      dlen = 1u;
    } else if (cmd_type == 2u) {
      dlen = (w & 0xFFu) + 2u;
      if (op == 0x22u) return 0;
      if (op == 0x14u) return 0;
      if (dlen >= 4u && i + 3u < count) {
        ix_u64 addr;
        if (op == 0x20u) {
          addr = (ix_u64)words[i + 1u] | ((ix_u64)words[i + 2u] << 32u);
          if (addr < aperture_base || addr >= aperture_end) return 0;
        }
        if (op == 0x00u && dlen >= 5u && i + 4u < count) {
          addr = (ix_u64)words[i + 4u] | ((ix_u64)words[i + 5u] << 32u);
          if (addr < aperture_base || addr >= aperture_end) return 0;
        }
      }
    } else if (cmd_type == 3u) {
      dlen = (w & 0xFFu) + 2u;
      if (op == 0x22u) return 0;
      if (op == 0x14u) return 0;
    } else {
      dlen = 1u;
    }
    if (dlen == 0u || i + dlen > count) return 0;
    i += dlen;
  }
  return 1;
}

static void intel_ring_submit_mmio(const ix_u32* words, ix_u32 count) {
  ix_u64 gpu; ix_u32 bytes;
  if (!words || count == 0u || !g_ixg_driver_hw.ready || !g_ixg_driver_hw.mmio) return;
  if (!intel_ring_validate_words(words, count)) {
    g_ixg_driver_hw.submit_failed = 1u; return;
  }
  if (g_ixg_driver_hw.regs.status && ixg_driver_mmio_read(g_ixg_driver_hw.regs.status) == 0xFFFFFFFFu) {
    g_ixg_driver_hw.submit_failed = 1u; return;
  }
  bytes = count * sizeof(ix_u32);
  gpu = intel_stage(words, bytes);
  if (!gpu) return;
  if (g_ixg_driver_hw.regs.ring_base) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_base, (ix_u32)(gpu & 0xFFFFFFFFu));
  if (g_ixg_driver_hw.regs.ring_base_hi) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_base_hi, (ix_u32)(gpu >> 32u));
  if (g_ixg_driver_hw.regs.ring_size) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_size, bytes);
  if (g_ixg_driver_hw.regs.ring_tail) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_tail, g_ixg_driver_hw.ring_tail_is_bytes ? bytes : count);
  if (g_ixg_driver_hw.regs.ring_put) ixg_driver_mmio_write(g_ixg_driver_hw.regs.ring_put, g_ixg_driver_hw.ring_tail_is_bytes ? bytes : count);
  if (g_ixg_driver_hw.regs.doorbell) ixg_driver_mmio_write(g_ixg_driver_hw.regs.doorbell, ++g_intel.sequence);
  ixg_driver_mmio_flush_posted();
}

static int intel_bb_init(void) {
  void* cpu = malloc(INTEL_BB_SIZE);
  if (!cpu) return -1;
  g_intel.bb.pman_handle = 0;
  g_intel.bb.gpu_addr = (ix_u64)(uintptr_t)cpu;
  g_intel.bb.cpu_ptr = (ix_u32*)cpu;
  g_intel.bb.capacity_dwords = INTEL_BB_SIZE / 4u;
  g_intel.bb.state = 0u;
  return 0;
}

static void intel_bb_begin(void) {
  if (!g_intel.bb.cpu_ptr) return;
  g_intel.bb.used_dwords = 0u;
  g_intel.bb.state = 1u;
}

static void intel_bb_push(ix_u32 word) {
  if (!g_intel.bb.cpu_ptr || g_intel.bb.used_dwords >= g_intel.bb.capacity_dwords) return;
  g_intel.bb.cpu_ptr[g_intel.bb.used_dwords++] = word;
}

static void intel_bb_push_n(const ix_u32* data, ix_u32 count) {
  ix_u32 i;
  for (i = 0u; i < count; i++) intel_bb_push(data[i]);
}

static void intel_bb_submit(void) {
  ix_u32 start[4];
  if (!g_intel.bb.cpu_ptr || g_intel.bb.used_dwords == 0u) return;
  intel_bb_push(INTEL_MI_BATCH_BUFFER_END);
  if (g_intel.bb.used_dwords & 1u) intel_bb_push(INTEL_MI_NOOP);
  IXG_INLINE_SFENCE();
  g_intel.bb.state = 2u;
  start[0] = INTEL_MI_BATCH_BUFFER_START | 1u;
  start[1] = (ix_u32)(g_intel.bb.gpu_addr & 0xFFFFFFFCu);
  start[2] = (ix_u32)(g_intel.bb.gpu_addr >> 32u);
  start[3] = INTEL_MI_NOOP;
  intel_push_words(start, 4u);
  intel_ring_submit_mmio(start, 4u);
}

static void intel_push_batch(const ix_u32* data, ix_u32 count) {
  if (!data || count == 0u || count > INTEL_PUSH_MAX_DWORDS) return;
  if (g_intel.bb.cpu_ptr && g_intel.bb.state == 1u) {
    intel_bb_push_n(data, count);
  } else {
    intel_push_words(data, count);
    intel_ring_submit_mmio(data, count);
  }
}

static intel_shader_slot_t* intel_find_shader(ix_u32 id) {
  ix_u32 i;
  for (i = 0u; i < INTEL_SHADER_SLOTS; i++) if (g_intel.shaders[i].id == id) return &g_intel.shaders[i];
  return 0;
}

static intel_shader_slot_t* intel_alloc_shader_slot(ix_u32 id) {
  ix_u32 i;
  for (i = 0u; i < INTEL_SHADER_SLOTS; i++) if (g_intel.shaders[i].id == 0u) { g_intel.shaders[i].id = id; return &g_intel.shaders[i]; }
  return 0;
}

static void intel_emit_pipe_control(ix_u32 flags) {
  ix_u32 data[6];
  data[0] = INTEL_PIPE_CONTROL | 4u;
  data[1] = flags | 0x00000020u;
  data[2] = 0u; data[3] = 0u; data[4] = ++g_intel.sequence; data[5] = 0u;
  intel_push_batch(data, 6u);
}

static void intel_bind_state_base(void) {
  ix_u32 data[12];
  data[0] = INTEL_STATE_BASE_ADDRESS | 10u;
  data[1] = 1u; data[2] = 1u; data[3] = 1u; data[4] = 1u;
  data[5] = 0xFFFFF001u; data[6] = 0xFFFFF001u; data[7] = 0xFFFFF001u; data[8] = 0xFFFFF001u;
  data[9] = 0u; data[10] = 0u;
  intel_push_batch(data, 11u);
}

static void intel_bind_shader_state(void) {
  ix_u32 data[16];
  intel_shader_slot_t* vs = intel_find_shader(g_intel.default_vs);
  intel_shader_slot_t* ps = intel_find_shader(g_intel.default_ps);
  data[0] = INTEL_3DSTATE_VS | 5u;
  data[1] = vs ? (ix_u32)(vs->gpu & 0xFFFFFFFFu) : 0u;
  data[2] = vs ? (ix_u32)(vs->gpu >> 32u) : 0u;
  data[3] = 0u; data[4] = 0u; data[5] = 0u;
  intel_push_batch(data, 6u);
  data[0] = INTEL_3DSTATE_HS | 2u; data[1] = 0u; data[2] = 0u; intel_push_batch(data, 3u);
  data[0] = INTEL_3DSTATE_TE | 1u; data[1] = 0u; intel_push_batch(data, 2u);
  data[0] = INTEL_3DSTATE_DS | 2u; data[1] = 0u; data[2] = 0u; intel_push_batch(data, 3u);
  data[0] = INTEL_3DSTATE_GS | 2u; data[1] = 0u; data[2] = 0u; intel_push_batch(data, 3u);
  data[0] = INTEL_3DSTATE_PS | 7u;
  data[1] = ps ? (ix_u32)(ps->gpu & 0xFFFFFFFFu) : 0u;
  data[2] = ps ? (ix_u32)(ps->gpu >> 32u) : 0u;
  data[3] = 0u; data[4] = 0u; data[5] = 0u; data[6] = 0u; data[7] = 0u;
  intel_push_batch(data, 8u);
}

static void intel_emit_vertex_elements(void) {
  ix_u32 data[7];
  data[0] = INTEL_3DSTATE_VERTEX_ELEMENTS | 5u;
  data[1] = (0u << 27) | (1u << 26) | (INTEL_3DSTATE_VE_FMT_R32G32B32A32_FLOAT << 16) | 0u;
  data[2] = (INTEL_VE_COMP_STORE_SRC << 28) | (INTEL_VE_COMP_STORE_SRC << 24) |
            (INTEL_VE_COMP_STORE_SRC << 20) | (INTEL_VE_COMP_STORE_SRC << 16);
  data[3] = (0u << 27) | (1u << 26) | (INTEL_3DSTATE_VE_FMT_R32_UINT << 16) | 16u;
  data[4] = (INTEL_VE_COMP_STORE_SRC << 28) | (INTEL_VE_COMP_STORE_0 << 24) |
            (INTEL_VE_COMP_STORE_0 << 20) | (INTEL_VE_COMP_STORE_1_FP << 16);
  data[5] = (0u << 27) | (1u << 26) | (INTEL_3DSTATE_VE_FMT_R32G32_FLOAT << 16) | 20u;
  data[6] = (INTEL_VE_COMP_STORE_SRC << 28) | (INTEL_VE_COMP_STORE_SRC << 24) |
            (INTEL_VE_COMP_STORE_0 << 20) | (INTEL_VE_COMP_STORE_1_FP << 16);
  intel_push_batch(data, 7u);
}

static void intel_emit_drawing_rectangle(void) {
  ix_u32 data[4];
  ix_u32 w = g_intel.fb_width ? g_intel.fb_width : 1024u;
  ix_u32 h = g_intel.fb_height ? g_intel.fb_height : 768u;
  data[0] = INTEL_3DSTATE_DRAWING_RECTANGLE | 2u;
  data[1] = 0u;
  data[2] = ((h - 1u) << 16u) | (w - 1u);
  data[3] = 0u;
  intel_push_batch(data, 4u);
}

static void intel_emit_pipeline_select_3d(void) {
  ix_u32 data[2];
  data[0] = INTEL_PIPELINE_SELECT_CMD;
  data[1] = 0u;
  intel_push_batch(data, 1u);
}

static void intel_force_wake(void) {
  ix_u32 spins;
  if (!g_ixg_driver_hw.ready || !g_ixg_driver_hw.mmio) return;
  if (g_intel.gen < INTEL_GEN_SKYLAKE) return;
  ixg_driver_mmio_write(INTEL_FORCEWAKE_MT, 0x00010001u);
  IXG_INLINE_MFENCE();
  for (spins = 0u; spins < INTEL_COLD_START_FORCEWAKE_SPINS; spins++) {
    ix_u32 ack = ixg_driver_mmio_read(INTEL_FORCEWAKE_ACK_MT);
    if (ack & 1u) break;
    IXG_INLINE_PAUSE();
  }
  if (g_intel.gen >= INTEL_GEN_XE_LP) {
    ixg_driver_mmio_write(INTEL_FORCEWAKE_RENDER, 0x00010001u);
    ixg_driver_mmio_write(INTEL_FORCEWAKE_MEDIA, 0x00010001u);
    IXG_INLINE_MFENCE();
    for (spins = 0u; spins < INTEL_COLD_START_FORCEWAKE_SPINS; spins++) IXG_INLINE_PAUSE();
  }
}

static ix_u8 intel_poll_status_ready(void) {
  ix_u32 spins;
  if (!g_ixg_driver_hw.ready || !g_ixg_driver_hw.mmio) return 0u;
  if (!g_ixg_driver_hw.regs.status) return 1u;
  for (spins = 0u; spins < INTEL_COLD_START_STATUS_SPINS; spins++) {
    ix_u32 s = ixg_driver_mmio_read(g_ixg_driver_hw.regs.status);
    if (s != 0xFFFFFFFFu && s != 0u) return 1u;
    IXG_INLINE_PAUSE();
  }
  return 0u;
}

static void intel_bind_render_state(void) {
  ix_u32 data[16];
  intel_emit_pipeline_select_3d();
  data[0] = INTEL_3DSTATE_VF_STATISTICS | 1u; data[1] = 1u; intel_push_batch(data, 2u);
  data[0] = INTEL_3DSTATE_WM | 2u; data[1] = 0u; data[2] = 0u; intel_push_batch(data, 3u);
  data[0] = INTEL_3DSTATE_SF | 6u; data[1] = 0u; data[2] = 0u; data[3] = 0u; data[4] = 0u; data[5] = 0u; data[6] = 0u; intel_push_batch(data, 7u);
  data[0] = INTEL_3DSTATE_RASTER | 4u; data[1] = 0u; data[2] = 0u; data[3] = 0u; data[4] = 0u; intel_push_batch(data, 5u);
  data[0] = INTEL_3DSTATE_CLIP | 3u; data[1] = 0u; data[2] = 0u; data[3] = 0u; intel_push_batch(data, 4u);
  data[0] = INTEL_3DSTATE_PS_BLEND | 2u; data[1] = 0u; data[2] = 0u; intel_push_batch(data, 3u);
  intel_emit_vertex_elements();
  intel_emit_drawing_rectangle();
}

static void intel_bind_objects(void) {
  if (g_intel.bound_objects) return;
  intel_refresh_fusion();
  intel_force_wake();
  if (!intel_poll_status_ready()) {
    g_ixg_driver_hw.submit_failed = 1u;
    return;
  }
  ixg_driver_program_dma_registers();
  ixg_driver_program_ring_registers();
  if (g_intel.bb.cpu_ptr) intel_bb_begin();
  intel_bind_state_base();
  intel_bind_shader_state();
  intel_bind_render_state();
  if (g_intel.bb.cpu_ptr) intel_bb_submit();
  g_intel.bound_objects = 1u;
}

static void intel_flush(void) {
  ix_u32 data[4];
  intel_bind_objects();
  if (g_intel.bb.cpu_ptr) {
    intel_bb_begin();
    intel_emit_pipe_control(0x00101000u);
    data[0] = INTEL_MI_FLUSH_DW | 2u; data[1] = ++g_intel.sequence; data[2] = 0u;
    intel_push_batch(data, 3u);
    intel_bb_submit();
  } else {
    intel_emit_pipe_control(0x00101000u);
    data[0] = INTEL_MI_FLUSH_DW | 2u; data[1] = ++g_intel.sequence; data[2] = 0u; data[3] = INTEL_MI_BATCH_BUFFER_END;
    intel_push_batch(data, 4u);
  }
  (void)ixg_driver_wait_idle(IXG_DRIVER_MMIO_POLL_SPINS);
}

static void intel_draw_rect(int x, int y, int w, int h, ix_u32 color) {
  ix_u32 data[12]; ix_u32 pitch;
  if (w <= 0 || h <= 0) return;
  intel_bind_objects();
  intel_emit_fusion_select(intel_select_fusion_mask(0u, (ix_u32)(w * h)), 0u);
  pitch = g_intel.fb_pitch ? g_intel.fb_pitch : 4096u;
  if (g_intel.bb.cpu_ptr) {
    intel_bb_begin();
    data[0] = INTEL_XY_COLOR_BLT | 5u;
    data[1] = 0xF0u | (pitch & 0xFFFFu) | INTEL_MEM_FENCE;
    data[2] = ((ix_u32)(ix_u16)y << 16u) | (ix_u32)(ix_u16)x;
    data[3] = ((ix_u32)(ix_u16)(y + h) << 16u) | (ix_u32)(ix_u16)(x + w);
    data[4] = (ix_u32)(g_intel.fb_gpu_addr & 0xFFFFFFFFu);
    data[5] = color;
    intel_push_batch(data, 6u);
    intel_bb_submit();
    return;
  }
  data[0] = INTEL_XY_COLOR_BLT | 5u;
  data[1] = 0xF0u | (pitch & 0xFFFFu) | INTEL_MEM_FENCE;
  data[2] = ((ix_u32)(ix_u16)y << 16u) | (ix_u32)(ix_u16)x;
  data[3] = ((ix_u32)(ix_u16)(y + h) << 16u) | (ix_u32)(ix_u16)(x + w);
  data[4] = (ix_u32)(g_intel.fb_gpu_addr & 0xFFFFFFFFu);
  data[5] = color;
  intel_push_batch(data, 6u);
}

static void intel_bitblt(int dx, int dy, int w, int h, void* src, int sx, int sy) {
  ix_u64 gpu; ix_u32 data[12]; ix_u32 src_pitch; ix_u32 dst_pitch;
  if (!src || w <= 0 || h <= 0 || sx < 0 || sy < 0) return;
  intel_bind_objects();
  intel_emit_fusion_select(intel_select_fusion_mask(0u, (ix_u32)(w * h)), 0u);
  src_pitch = (ix_u32)w * 4u;
  dst_pitch = g_intel.fb_pitch ? g_intel.fb_pitch : src_pitch;
  gpu = intel_stage(src, src_pitch * (ix_u32)h);
  if (g_intel.bb.cpu_ptr) {
    intel_bb_begin();
    data[0] = INTEL_XY_SRC_COPY_BLT | 6u;
    data[1] = 0xCCu | (dst_pitch & 0xFFFFu) | INTEL_MEM_FENCE;
    data[2] = ((ix_u32)(ix_u16)dy << 16u) | (ix_u32)(ix_u16)dx;
    data[3] = ((ix_u32)(ix_u16)(dy + h) << 16u) | (ix_u32)(ix_u16)(dx + w);
    data[4] = (ix_u32)(g_intel.fb_gpu_addr & 0xFFFFFFFFu);
    data[5] = src_pitch & 0xFFFFu;
    data[6] = (ix_u32)(gpu & 0xFFFFFFFFu);
    intel_push_batch(data, 7u);
    intel_bb_submit();
    return;
  }
  data[0] = INTEL_XY_SRC_COPY_BLT | 6u;
  data[1] = 0xCCu | (dst_pitch & 0xFFFFu) | INTEL_MEM_FENCE;
  data[2] = ((ix_u32)(ix_u16)dy << 16u) | (ix_u32)(ix_u16)dx;
  data[3] = ((ix_u32)(ix_u16)(dy + h) << 16u) | (ix_u32)(ix_u16)(dx + w);
  data[4] = (ix_u32)(g_intel.fb_gpu_addr & 0xFFFFFFFFu);
  data[5] = src_pitch & 0xFFFFu;
  data[6] = (ix_u32)(gpu & 0xFFFFFFFFu);
  intel_push_batch(data, 7u);
}

static int intel_rect_blend(int x, int y, int w, int h, ix_u32 color, ix_u8 premultiplied) {
  (void)premultiplied;
  if (w <= 0 || h <= 0) return -1;
  intel_draw_rect(x, y, w, h, color);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int intel_bitblt_alpha(int dx, int dy, int w, int h, const void* src, ix_u32 stride_bytes, ix_u8 premultiplied) {
  (void)stride_bytes; (void)premultiplied;
  if (!src || w <= 0 || h <= 0) return -1;
  intel_bitblt(dx, dy, w, h, (void*)src, 0, 0);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int intel_emit_triangles(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture) {
  ix_u32 total; ix_u32 bytes; ix_u32 data[16];
  if (!vertices || triangle_count == 0u) return -1;
  if (g_ixg_driver_hw.submit_failed) return -1;
  total = triangle_count * 3u;
  bytes = total * (ix_u32)sizeof(ixg_hw_vertex_t);
  intel_bind_objects();
  intel_emit_fusion_select(intel_select_fusion_mask(0u, 0u), 0u);
  if (g_ixg_driver_hw.submit_failed) return -1;
  if (g_intel.bb.cpu_ptr) {
    void* vbo_cpu = malloc(bytes);
    if (vbo_cpu) {
      memcpy(vbo_cpu, vertices, bytes);
      ix_u64 vbo_gpu = (ix_u64)(uintptr_t)vbo_cpu;
      intel_bb_begin();
      intel_emit_drawing_rectangle();
      intel_emit_vertex_elements();
      data[0] = INTEL_3DSTATE_VERTEX_BUFFERS | 4u;
      data[1] = 0u | ((ix_u32)sizeof(ixg_hw_vertex_t) << 16u);
      data[2] = (ix_u32)(vbo_gpu & 0xFFFFFFFFu);
      data[3] = (ix_u32)((vbo_gpu + bytes - 1u) & 0xFFFFFFFFu);
      data[4] = 0u;
      intel_push_batch(data, 5u);
      if (texture && texture->pixels && texture->width > 0u && texture->height > 0u) {
        ix_u32 stride_t = texture->stride ? texture->stride : texture->width;
        ix_u64 tex_gpu = intel_stage(texture->pixels, stride_t * (ix_u32)texture->height * 4u);
        data[0] = INTEL_3DSTATE_BINDING_TABLE_POINTERS_PS | 1u;
        data[1] = (ix_u32)(tex_gpu & 0xFFFFFFFFu);
        intel_push_batch(data, 2u);
        data[0] = INTEL_3DSTATE_SAMPLER_STATE_POINTERS_PS | 1u;
        data[1] = ((ix_u32)texture->width << 16u) | texture->height;
        intel_push_batch(data, 2u);
      }
      data[0] = INTEL_3DPRIMITIVE | 6u;
      data[1] = 4u; data[2] = total; data[3] = 0u; data[4] = 1u; data[5] = 0u; data[6] = 0u;
      intel_push_batch(data, 7u);
      intel_bb_submit();
      free(vbo_cpu);
      return g_ixg_driver_hw.submit_failed ? -1 : 0;
    }
  }
  {
    ix_u64 gpu = intel_stage(vertices, bytes);
    intel_emit_drawing_rectangle();
    intel_emit_vertex_elements();
    data[0] = INTEL_3DSTATE_VERTEX_BUFFERS | 4u;
    data[1] = 0u | ((ix_u32)sizeof(ixg_hw_vertex_t) << 16u);
    data[2] = (ix_u32)(gpu & 0xFFFFFFFFu);
    data[3] = (ix_u32)((gpu + bytes - 1u) & 0xFFFFFFFFu);
    data[4] = 0u;
    intel_push_batch(data, 5u);
    if (texture && texture->pixels && texture->width > 0u && texture->height > 0u) {
      ix_u32 stride_t = texture->stride ? texture->stride : texture->width;
      ix_u64 tex_gpu = intel_stage(texture->pixels, stride_t * (ix_u32)texture->height * 4u);
      data[0] = INTEL_3DSTATE_BINDING_TABLE_POINTERS_PS | 1u;
      data[1] = (ix_u32)(tex_gpu & 0xFFFFFFFFu);
      intel_push_batch(data, 2u);
      data[0] = INTEL_3DSTATE_SAMPLER_STATE_POINTERS_PS | 1u;
      data[1] = ((ix_u32)texture->width << 16u) | texture->height;
      intel_push_batch(data, 2u);
    }
    data[0] = INTEL_3DPRIMITIVE | 6u;
    data[1] = 4u; data[2] = total; data[3] = 0u; data[4] = 1u; data[5] = 0u; data[6] = 0u;
    intel_push_batch(data, 7u);
  }
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int intel_triangles(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count) { return intel_emit_triangles(vertices, triangle_count, 0); }
static int intel_triangles_tex(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture) {
  if (!texture) return -1;
  return intel_emit_triangles(vertices, triangle_count, texture);
}

/*
 * CPU-visible render target: кадр рендерится в буфер системной
 * RAM, отображённый в GTT/апертуру ixg (ixg_driver_render_target_*).
 * 3D-конвейер и BLT-операции целятся в него через g_intel.fb_*,
 * после flush CPU читает кадр обратно в canvas окна -- это и
 * есть видимый GPU-вывод AxGL (режим AUTO ядре).
 */
static int intel_set_render_target(ix_u32 w, ix_u32 h) {
  if (ixg_driver_render_target_setup(w, h) != 0) return -1;
  g_intel.fb_width    = w;
  g_intel.fb_height   = h;
  g_intel.fb_pitch    = ixg_driver_render_target_stride_px() * 4u;
  g_intel.fb_gpu_addr = ixg_driver_render_target_gpu_addr();
  g_intel.bound_objects = 0u;   /* перевыпустить drawing rectangle */
  return 0;
}

static int intel_readback_rect(ix_u32* dst, ix_u32 w, ix_u32 h) {
  const ix_u8* src;
  ix_u32 stride_px, y;
  if (!dst || !w || !h) return -1;
  src = (const ix_u8*)ixg_driver_render_target_cpu();
  if (!src) return -1;
  stride_px = ixg_driver_render_target_stride_px();
  if (w > stride_px) w = stride_px;
  if (h > ixg_driver_render_target_height()) h = ixg_driver_render_target_height();
  for (y = 0u; y < h; y++) {
    memcpy(dst + (ix_u64)y * w,
           src + (ix_u64)y * (ix_u64)stride_px * 4u,
           (ix_u64)w * 4u);
  }
  return 0;
}

static int intel_compute(const void* dispatch_desc) {
  const ixg_compute_dispatch_desc_t* desc = (const ixg_compute_dispatch_desc_t*)dispatch_desc;
  ix_u64 gpu; ix_u32 data[16]; intel_shader_slot_t* shader;
  ix_u32 walker_cmd;
  if (!desc) return -1;
  intel_bind_objects();
  intel_emit_fusion_select(intel_select_fusion_mask(1u, 0u), 1u);
  if (!g_intel.compute_path) return -1;
  gpu = intel_stage(desc, sizeof(ixg_compute_dispatch_desc_t));
  shader = intel_find_shader(desc->kernel ? desc->kernel : g_intel.default_cs);
  walker_cmd = (g_intel.gen >= INTEL_GEN_XE2) ? INTEL_COMPUTE_WALKER : INTEL_GPGPU_WALKER;
  if (g_intel.bb.cpu_ptr) {
    intel_bb_begin();
    data[0] = INTEL_MEDIA_VFE_STATE | 7u;
    data[1] = 0u; data[2] = 64u; data[3] = 0u; data[4] = 0u; data[5] = 0u; data[6] = 0u; data[7] = 0u;
    intel_push_batch(data, 8u);
    data[0] = INTEL_MEDIA_INTERFACE_DESCRIPTOR_LOAD | 3u;
    data[1] = 0u; data[2] = shader ? (ix_u32)(shader->gpu & 0xFFFFFFFFu) : 0u; data[3] = 32u;
    intel_push_batch(data, 4u);
    data[0] = walker_cmd | 9u;
    data[1] = 0u;
    data[2] = desc->x ? desc->x : 1u; data[3] = 0u;
    data[4] = desc->y ? desc->y : 1u; data[5] = 0u;
    data[6] = desc->z ? desc->z : 1u;
    data[7] = (ix_u32)(gpu & 0xFFFFFFFFu); data[8] = (ix_u32)(gpu >> 32u);
    data[9] = ++g_intel.sequence;
    intel_push_batch(data, 10u);
    intel_bb_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  data[0] = INTEL_MEDIA_VFE_STATE | 7u;
  data[1] = 0u; data[2] = 64u; data[3] = 0u; data[4] = 0u; data[5] = 0u; data[6] = 0u; data[7] = 0u;
  intel_push_batch(data, 8u);
  data[0] = INTEL_MEDIA_INTERFACE_DESCRIPTOR_LOAD | 3u;
  data[1] = 0u; data[2] = shader ? (ix_u32)(shader->gpu & 0xFFFFFFFFu) : 0u; data[3] = 32u;
  intel_push_batch(data, 4u);
  data[0] = walker_cmd | 9u;
  data[1] = intel_float_bits(0.0f);
  data[2] = desc->x ? desc->x : 1u; data[3] = 0u;
  data[4] = desc->y ? desc->y : 1u; data[5] = 0u;
  data[6] = desc->z ? desc->z : 1u;
  data[7] = (ix_u32)(gpu & 0xFFFFFFFFu); data[8] = (ix_u32)(gpu >> 32u);
  data[9] = ++g_intel.sequence;
  intel_push_batch(data, 10u);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int intel_trace(const void* trace_desc) { return intel_compute(trace_desc); }
static int intel_control(const void* control_desc, void* control_result) { (void)control_result; return intel_compute(control_desc); }

static void intel_seed_shader(ix_u32 id, ix_u32 tag) {
  ix_u32 code[16]; intel_shader_slot_t* slot; ix_u32 i;
  for (i = 0u; i < 16u; i++) code[i] = INTEL_SHADER_MAGIC ^ tag ^ (i * 0x9E3779B9u);
  slot = intel_alloc_shader_slot(id);
  if (slot) { slot->size = sizeof(code); slot->hash = intel_hash_blob((const ix_u8*)code, sizeof(code)); slot->gpu = intel_stage(code, sizeof(code)); }
}

int ixg_backend_intel_get_device_info(intel_device_info_t* out_info) {
  if (!out_info) return -1;
  *out_info = g_intel.dev_info;
  return 0;
}

int ixg_backend_intel_create_vbo(const void* vertex_data, ix_u32 size_bytes, ix_u32 stride, ix_u32* out_vbo_id) {
  intel_vbo_slot_t* vbo;
  if (!vertex_data || size_bytes == 0u || !out_vbo_id || stride == 0u) return -1;
  if (g_intel.vbo_count >= INTEL_VBO_SLOT_MAX) return -1;
  vbo = &g_intel.vbos[g_intel.vbo_count];
  memset(vbo, 0, sizeof(*vbo));
  vbo->cpu_ptr = malloc(size_bytes);
  if (!vbo->cpu_ptr) return -1;
  memcpy(vbo->cpu_ptr, vertex_data, size_bytes);
  vbo->size_bytes = size_bytes; vbo->stride_bytes = stride;
  vbo->vertex_count = size_bytes / stride; vbo->bound = 1u;
  g_intel.vbo_count++;
  *out_vbo_id = g_intel.vbo_count;
  return 0;
}

void ixg_backend_intel_destroy_vbo(ix_u32 vbo_id) {
  intel_vbo_slot_t* vbo;
  if (vbo_id == 0u || vbo_id > g_intel.vbo_count) return;
  vbo = &g_intel.vbos[vbo_id - 1u];
  if (!vbo->bound) return;
  if (vbo->cpu_ptr) free(vbo->cpu_ptr);
  memset(vbo, 0, sizeof(*vbo));
}

int ixg_backend_intel_draw_vbo(ix_u32 vbo_id, ix_u32 first, ix_u32 count, ix_u32 topology) {
  intel_vbo_slot_t* vbo; ix_u32 data[16];
  if (vbo_id == 0u || vbo_id > g_intel.vbo_count) return -1;
  vbo = &g_intel.vbos[vbo_id - 1u];
  if (!vbo->bound || first + count > vbo->vertex_count) return -1;
  intel_bind_objects();
  if (g_intel.bb.cpu_ptr) {
    intel_bb_begin();
    data[0] = INTEL_3DSTATE_VERTEX_BUFFERS | 4u;
    data[1] = 0u | (vbo->stride_bytes << 16u);
    data[2] = (ix_u32)(vbo->gpu_addr & 0xFFFFFFFFu);
    data[3] = (ix_u32)((vbo->gpu_addr + vbo->size_bytes - 1u) & 0xFFFFFFFFu);
    data[4] = 0u;
    intel_push_batch(data, 5u);
    data[0] = INTEL_3DPRIMITIVE | 6u;
    data[1] = topology; data[2] = count; data[3] = first; data[4] = 1u; data[5] = 0u; data[6] = 0u;
    intel_push_batch(data, 7u);
    intel_bb_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  return -1;
}

int ixg_backend_intel_load_mcl_firmware(const void* fw_data, ix_u32 fw_size) {
  ix_u32 id;
  if (!fw_data || fw_size < 0x80u || fw_size > (2u * 1024u * 1024u)) return -1;
  if (ixg_backend_intel_upload_shader((const ix_u8*)fw_data, fw_size, &id) != 0) return -1;
  g_intel.default_cs = id;
  g_intel.bound_objects = 0u;
  return 0;
}

void ixg_backend_intel_post_init(void) {
  memset(&g_intel, 0, sizeof(g_intel));
  g_intel.initialized = 1u;
  g_intel.gen = intel_gen_from_device(g_ixg_driver_hw.device_id);
  g_intel.legacy_blt = g_intel.gen < INTEL_GEN_SKYLAKE ? 1u : 0u;
  g_intel.compute_path = g_intel.gen >= INTEL_GEN_SKYLAKE ? 1u : 0u;
  g_intel.fb_width = 1024u; g_intel.fb_height = 768u;
  g_intel.fb_pitch = g_intel.fb_width * 4u; g_intel.fb_gpu_addr = 0u;
  g_intel.shader_next = 4u; g_intel.default_vs = 1u; g_intel.default_ps = 2u; g_intel.default_cs = 3u;
  intel_detect_hw_caps();
  if (!g_intel.legacy_blt) (void)intel_bb_init();
  intel_seed_shader(g_intel.default_vs, 0x56532020u);
  intel_seed_shader(g_intel.default_ps, 0x50532020u);
  intel_seed_shader(g_intel.default_cs, 0x43532020u);
  intel_force_wake();
  {
    ix_u32 data[8];
    ixg_driver_program_dma_registers();
    ixg_driver_program_ring_registers();
    if (!intel_poll_status_ready()) {
      g_ixg_driver_hw.submit_failed = 1u;
      return;
    }
    data[0] = INTEL_MI_NOOP;
    data[1] = INTEL_MI_NOOP;
    data[2] = INTEL_MI_FLUSH_DW | 2u; data[3] = ++g_intel.sequence;
    data[4] = 0u;
    data[5] = INTEL_MI_BATCH_BUFFER_END;
    intel_push_words(data, 6u);
    intel_ring_submit_mmio(data, 6u);
    (void)ixg_driver_wait_idle(IXG_DRIVER_MMIO_POLL_SPINS);
  }
  if (g_ixg_driver_hw.submit_failed) return;
  intel_bind_objects();
}

void ixg_backend_intel_shutdown(void) {
  ix_u32 i;
  for (i = 0u; i < g_intel.vbo_count; i++) {
    if (g_intel.vbos[i].bound && g_intel.vbos[i].cpu_ptr) free(g_intel.vbos[i].cpu_ptr);
  }
  if (g_intel.bb.cpu_ptr) free(g_intel.bb.cpu_ptr);
  memset(&g_intel, 0, sizeof(g_intel));
}

int ixg_backend_intel_upload_shader(const ix_u8* binary, ix_u32 binary_size, ix_u32* out_shader_id) {
  ix_u32 id; intel_shader_slot_t* slot;
  if (!binary || binary_size == 0u || !out_shader_id) return -1;
  id = g_intel.shader_next++;
  if (id == 0u) id = g_intel.shader_next++;
  slot = intel_alloc_shader_slot(id);
  if (!slot) return -1;
  slot->size = binary_size;
  slot->hash = intel_hash_blob(binary, binary_size);
  slot->gpu = intel_stage(binary, binary_size);
  *out_shader_id = id;
  return 0;
}

void ixg_backend_intel_delete_shader(ix_u32 shader_id) {
  intel_shader_slot_t* slot = intel_find_shader(shader_id);
  if (!slot) return;
  memset(slot, 0, sizeof(*slot));
}

static ix_u32 intel_isa_target(void) {
  if (g_intel.gen >= INTEL_GEN_XE2)    return IXG_ISA_INTEL_XE2;
  if (g_intel.gen >= INTEL_GEN_XE_HPG) return IXG_ISA_INTEL_XEHPG;
  if (g_intel.gen >= INTEL_GEN_XE_LP)  return IXG_ISA_INTEL_GEN12;
  return IXG_ISA_INTEL_GEN9;
}

static int intel_compile_ir_stage(const void* ir_bytecode, ix_u32 ir_size,
                                   ix_u32 stage_mask, ix_u8 shader_type,
                                   ix_u64* out_gpu, ix_u32* out_size,
                                   ix_u32* out_validated) {
  static ixg_shader_program_t prog;
  static ixg_isa_binary_t bin;
  const ixg_shader_inst_t* src;
  ix_u32 inst_count, i;
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
  if (ixg_isa_encode(&prog, IXG_ISA_VENDOR_INTEL, shader_type, intel_isa_target(), &bin) != 0) return -2;
  if (bin.byte_len == 0u) return -3;
  *out_gpu = intel_stage(bin.bytes, bin.byte_len);
  if (!*out_gpu) return -4;
  if (out_size) *out_size = bin.byte_len;
  if (out_validated) *out_validated = bin.validated;
  return 0;
}

static int intel_bind_shaders(const ixg_hw_shader_bind_t* bind) {
  ix_u64 vs_gpu = 0u, ps_gpu = 0u;
  ix_u32 vs_valid = 0u, ps_valid = 0u;
  intel_shader_slot_t* vs;
  intel_shader_slot_t* ps;
  if (!bind || !bind->vs_bytecode || bind->vs_size == 0u ||
      !bind->ps_bytecode || bind->ps_size == 0u) return IXG_BIND_ERROR;
  intel_bind_objects();
  if (g_ixg_driver_hw.submit_failed) return IXG_BIND_ERROR;
  if (intel_compile_ir_stage(bind->vs_bytecode, bind->vs_size,
                             (ix_u32)IXG_SHADER_STAGE_3D_VERTEX,
                             (ix_u8)0u, &vs_gpu, 0, &vs_valid) != 0 ||
      intel_compile_ir_stage(bind->ps_bytecode, bind->ps_size,
                             (ix_u32)IXG_SHADER_STAGE_3D_PIXEL,
                             (ix_u8)1u, &ps_gpu, 0, &ps_valid) != 0) {
    return IXG_BIND_SOFTWARE_FALLBACK;
  }
  if (!vs_valid || !ps_valid) return IXG_BIND_SOFTWARE_FALLBACK;
  if (!vs_gpu || !ps_gpu) return IXG_BIND_ERROR;
  vs = intel_find_shader(g_intel.default_vs);
  ps = intel_find_shader(g_intel.default_ps);
  if (vs) { vs->gpu = vs_gpu; if (vs_gpu) vs->size = 0; }
  if (ps) { ps->gpu = ps_gpu; if (ps_gpu) ps->size = 0; }
  return g_ixg_driver_hw.submit_failed ? IXG_BIND_ERROR : IXG_BIND_OK;
}

void ixg_backend_intel_init_ops(ixg_hw_ops_t* out_ops) {
  if (!out_ops) return;
  memset(out_ops, 0, sizeof(*out_ops));
  out_ops->hw_draw_rect = intel_draw_rect;
  out_ops->hw_bitblt = intel_bitblt;
  out_ops->hw_draw_rect_blend = intel_rect_blend;
  out_ops->hw_bitblt_alpha = intel_bitblt_alpha;
  out_ops->hw_flush = intel_flush;
  out_ops->hw_draw_triangles = intel_triangles;
  out_ops->hw_draw_triangles_textured = intel_triangles_tex;
  out_ops->hw_set_render_target = intel_set_render_target;
  out_ops->hw_readback_rect = intel_readback_rect;
  out_ops->hw_compute_dispatch = intel_compute;
  out_ops->hw_trace_rays = intel_trace;
  out_ops->hw_control_dispatch = intel_control;
  out_ops->hw_bind_shaders = intel_bind_shaders;
}