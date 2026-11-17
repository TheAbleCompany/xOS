#include "ixg_backend_nvidia.h"
#include "ixg_compat.h"
#include "ixg_compute.h"
#include "ixg_mutants.h"
#include "ixg_sass.h"
#include "ixg_sass_encode.h"
#include "ixg_pman.h"
#include <string.h>

#define NV_FIFO_FREE_SPINS 20000u
#define NV_PUSH_MAX_DWORDS 256u
#define NV_METHOD_TYPE_INC 0x00000000u
#define NV_METHOD_TYPE_NON_INC 0x40000000u
#define NV_METHOD_TYPE_IMMD 0x80000000u
#define NV_SUBCH_3D 0u
#define NV_SUBCH_2D 1u
#define NV_SUBCH_M2MF 2u
#define NV_SUBCH_CE 3u
#define NV_SUBCH_COMPUTE 4u
#define NV_MTHD_SET_OBJECT 0x0000u
#define NV_MTHD_NO_OPERATION 0x0100u
#define NV_MTHD_FUSION_SELECT 0x0104u
#define NV_MTHD_FUSION_MASK_LO 0x0108u
#define NV_MTHD_FUSION_MASK_HI 0x010Cu
#define NV_MTHD_SEMAPHORE_ADDR_HI 0x0010u
#define NV_MTHD_SEMAPHORE_ADDR_LO 0x0014u
#define NV_MTHD_SEMAPHORE_SEQUENCE 0x0018u
#define NV_MTHD_SEMAPHORE_TRIGGER 0x001Cu
#define NV_MTHD_NOTIFY_ADDR_HI 0x0020u
#define NV_MTHD_NOTIFY_ADDR_LO 0x0024u
#define NV_MTHD_NOTIFY_INTR 0x0028u
#define NV_M2MF_OFFSET_IN_HIGH 0x0238u
#define NV_M2MF_OFFSET_OUT_HIGH 0x023Cu
#define NV_M2MF_OFFSET_IN_LOW 0x030Cu
#define NV_M2MF_OFFSET_OUT_LOW 0x0310u
#define NV_M2MF_PITCH_IN 0x0314u
#define NV_M2MF_PITCH_OUT 0x0318u
#define NV_M2MF_LINE_LENGTH_IN 0x031Cu
#define NV_M2MF_LINE_COUNT 0x0320u
#define NV_M2MF_FORMAT 0x0324u
#define NV_M2MF_EXEC 0x0300u
#define NV_2D_SET_DST_FORMAT 0x0200u
#define NV_2D_SET_DST_PITCH 0x0204u
#define NV_2D_SET_DST_OFFSET 0x0208u
#define NV_2D_SET_COLOR_FORMAT 0x0588u
#define NV_2D_SET_COLOR 0x058Cu
#define NV_2D_RENDER_SOLID_PRIM_MODE 0x05A0u
#define NV_2D_RENDER_SOLID_PRIM_POINT_SET_X 0x05C0u
#define NV_3D_SET_ZETA_ENABLE 0x0E00u
#define NV_3D_SET_RT_ENABLE 0x0200u
#define NV_3D_SET_RT_FORMAT 0x0208u
#define NV_3D_SET_RT_ADDRESS_HIGH 0x0210u
#define NV_3D_SET_RT_ADDRESS_LOW 0x0214u
#define NV_3D_SET_RT_HORIZ 0x0220u
#define NV_3D_SET_RT_VERT 0x0224u
#define NV_3D_VERTEX_BEGIN_GL 0x1614u
#define NV_3D_VERTEX_END_GL 0x1618u
#define NV_3D_VERTEX_DATA 0x1640u
#define NV_3D_DRAW_ARRAYS 0x1610u
#define NV_3D_DRAW_ARRAYS_FIRST 0x1710u
#define NV_3D_DRAW_ARRAYS_COUNT 0x1714u
#define NV_3D_VB_ELEMENT_BASE 0x1644u
#define NV_3D_VB_INSTANCE_BASE 0x1648u
#define NV_3D_VERTEX_BUFFER_FIRST 0x17FCu
#define NV_3D_VERTEX_BUFFER_COUNT 0x1800u
#define NV_3D_VERTEX_ATTRIB_FORMAT(i) (0x1C00u + (i) * 4u)
#define NV_3D_VERTEX_BUFFER_BIND(i) (0x1D00u + (i) * 4u)
#define NV_3D_VERTEX_BUFFER_START_HIGH(i) (0x1F00u + (i) * 8u)
#define NV_3D_VERTEX_BUFFER_START_LOW(i) (0x1F04u + (i) * 8u)
#define NV_3D_VERTEX_BUFFER_LIMIT_HIGH(i) (0x1780u + (i) * 8u)
#define NV_3D_VERTEX_BUFFER_LIMIT_LOW(i) (0x1784u + (i) * 8u)
#define NV_3D_BIND_SHADER_PROGRAM 0x2000u
#define NV_3D_BIND_SHADER_PROGRAM_TYPE 0x2004u
#define NV_3D_SHADER_PROGRAM_ADDR_HI 0x2008u
#define NV_3D_SHADER_PROGRAM_ADDR_LO 0x200Cu
#define NV_3D_SHADER_LOCAL_MEM 0x2010u
#define NV_3D_SHADER_SHARED_MEM 0x2014u
#define NV_COMPUTE_SET_PROGRAM_REGION 0x02A0u
#define NV_COMPUTE_SET_LAUNCH_ID 0x036Cu
#define NV_COMPUTE_LAUNCH 0x0368u
#define NV_COMPUTE_GRIDDIM_X 0x03ACu
#define NV_COMPUTE_GRIDDIM_Y 0x03B0u
#define NV_COMPUTE_GRIDDIM_Z 0x03B4u
#define NV_COMPUTE_BLOCKDIM_X 0x03B8u
#define NV_COMPUTE_BLOCKDIM_Y 0x03BCu
#define NV_COMPUTE_BLOCKDIM_Z 0x03C0u
#define NV_COMPUTE_SHARED_SIZE 0x03C4u
#define NV_COMPUTE_PROGRAM_ADDR_HI 0x03C8u
#define NV_COMPUTE_PROGRAM_ADDR_LO 0x03CCu
#define NV_COMPUTE_TENSOR_DESC_ADDR_HI 0x03D0u
#define NV_COMPUTE_TENSOR_DESC_ADDR_LO 0x03D4u
#define NV_COMPUTE_TENSOR_LAUNCH 0x03D8u
#define NV_COMPUTE_RT_BVH_ADDR_HI 0x03E0u
#define NV_COMPUTE_RT_BVH_ADDR_LO 0x03E4u
#define NV_COMPUTE_RT_RAY_ADDR_HI 0x03E8u
#define NV_COMPUTE_RT_RAY_ADDR_LO 0x03ECu
#define NV_COMPUTE_RT_HIT_ADDR_HI 0x03F0u
#define NV_COMPUTE_RT_HIT_ADDR_LO 0x03F4u
#define NV_COMPUTE_RT_RAY_COUNT 0x03F8u
#define NV_COMPUTE_RT_LAUNCH 0x03FCu
#define NV_GSP_FALCON_CPUCTL 0x100u
#define NV_GSP_FALCON_BOOTVEC 0x104u
#define NV_GSP_FALCON_DMACTL 0x10Cu
#define NV_GSP_FALCON_DMATRFBASE 0x110u
#define NV_GSP_FALCON_DMATRFMOFFS 0x114u
#define NV_GSP_FALCON_DMATRFFBOFFS 0x118u
#define NV_GSP_FALCON_DMATRFCMD 0x11Cu
#define NV_GSP_FALCON_STATUS 0x130u
#define NV_GSP_MMIO_BASE_TURING 0x00110000u
#define NV_GSP_MMIO_BASE_AMPERE 0x00118000u
#define NV_GSP_MMIO_BASE_ADA 0x0011C000u
#define NV_GSP_MMIO_BASE_BLACKWELL 0x00120000u
#define NV_OBJ_LEGACY_RECT 0x0019u
#define NV_OBJ_LEGACY_IMAGE_BLIT 0x005Fu
#define NV_OBJ_LEGACY_DX5_TRIANGLE 0x0094u
#define NV_OBJ_LEGACY_CELSIUS 0x0056u
#define NV_OBJ_LEGACY_KELVIN 0x0097u
#define NV_OBJ_LEGACY_RANKINE 0x0397u
#define NV_OBJ_LEGACY_TESLA 0x8297u
#define NV_OBJ_M2MF 0x9039u
#define NV_OBJ_2D 0x902Du
#define NV_OBJ_3D_KEPLER 0xA097u
#define NV_OBJ_3D_MAXWELL 0xB197u
#define NV_OBJ_3D_PASCAL 0xC097u
#define NV_OBJ_3D_VOLTA 0xC397u
#define NV_OBJ_3D_TURING 0xC597u
#define NV_OBJ_3D_AMPERE 0xC797u
#define NV_OBJ_3D_ADA 0xC997u
#define NV_OBJ_3D_BLACKWELL 0xCB97u
#define NV_OBJ_COMPUTE_KEPLER 0xA0C0u
#define NV_OBJ_COMPUTE_MAXWELL 0xB1C0u
#define NV_OBJ_COMPUTE_PASCAL 0xC0C0u
#define NV_OBJ_COMPUTE_VOLTA 0xC3C0u
#define NV_OBJ_COMPUTE_TURING 0xC5C0u
#define NV_OBJ_COMPUTE_AMPERE 0xC7C0u
#define NV_OBJ_COMPUTE_ADA 0xC9C0u
#define NV_OBJ_COMPUTE_BLACKWELL 0xCBC0u
#define NV_OBJ_CE_KEPLER 0xA0B5u
#define NV_OBJ_CE_MAXWELL 0xB0B5u
#define NV_OBJ_CE_PASCAL 0xC0B5u
#define NV_OBJ_CE_VOLTA 0xC3B5u
#define NV_OBJ_CE_TURING 0xC5B5u
#define NV_OBJ_CE_AMPERE 0xC7B5u
#define NV_OBJ_CE_ADA 0xC9B5u
#define NV_OBJ_CE_BLACKWELL 0xCBB5u
#define NV_KERNEL_PID 1u
#define NV_DMA_ALIGN 256u

typedef struct {
  ix_u8  initialized;
  ix_u8  arch;
  ix_u8  bound_objects;
  ix_u8  legacy_path;
  ix_u32 class_3d;
  ix_u32 class_compute;
  ix_u32 class_ce;
  ix_u32 class_m2mf;
  ix_u32 sequence;
  ix_u32 fb_width;
  ix_u32 fb_height;
  ix_u32 fb_pitch;
  ix_u64 fb_gpu_addr;
  nv_dma_pushbuf_t pushbuf;
  nv_dma_ib_ring_t ib_ring;
  nv_dma_notifier_t notifier;
  nv_dma_semaphore_t semaphore;
  ix_u32 semaphore_seq;
  nv_device_info_t dev_info;
  nv_gsp_state_info_t gsp;
  nv_falcon_state_t falcons[NV_FALCON_MAX_ENGINES];
  nv_shader_slot_t shaders[NV_SHADER_SLOT_MAX];
  ix_u32 shader_count;
  nv_vbo_slot_t vbos[NV_VBO_SLOT_MAX];
  ix_u32 vbo_count;
  ix_u8  fusion_active;
  ix_u8  fusion_rr;
  ix_u16 fusion_reserved;
  ix_gpman_fusion_state_t fusion;
  ix_gpman_fusion_node_t fusion_nodes[IXGP_FUSION_MAX_NODES];
} nv_state_t;

static nv_state_t g_nv;

static ix_u32 nv_float_bits(float v) {
  ix_u32 out;
  memcpy(&out, &v, sizeof(out));
  return out;
}

static ix_u64 nv_slot_mask(ix_u8 slot) {
  if (slot == 1u) return IXGP_MASK_IGPU;
  if (slot >= 2u && slot < 32u) return IXGP_MASK_DGPU((ix_u64)(slot - 2u));
  if (slot >= 32u) return IXGP_MASK_MUTANT((ix_u64)(slot - 32u));
  return IXGP_MASK_CPU;
}

static void nv_refresh_fusion(void) {
  ix_memset(&g_nv.fusion, 0, sizeof(g_nv.fusion));
  ix_memset(g_nv.fusion_nodes, 0, sizeof(g_nv.fusion_nodes));
  g_nv.fusion_active = 0u;
  if (ixg_pman_fusion_get(&g_nv.fusion) != 0 || !g_nv.fusion.enabled || g_nv.fusion.node_count == 0u) return;
  if (g_nv.fusion.node_count > IXGP_FUSION_MAX_NODES) g_nv.fusion.node_count = IXGP_FUSION_MAX_NODES;
  { ix_u8 i; for (i = 0u; i < g_nv.fusion.node_count; i++) (void)ixg_pman_fusion_get_node(i, &g_nv.fusion_nodes[i]); }
  g_nv.fusion_active = 1u;
}

static ix_u64 nv_select_fusion_mask(ix_u8 compute, ix_u32 pixel_count) {
  if (!g_nv.fusion_active || g_nv.fusion.node_count == 0u)
    return g_nv.fusion.fusion_mask ? g_nv.fusion.fusion_mask : IXGP_MASK_CPU;
  if (g_nv.fusion.mode == IX_GP_FUSION_MODE_COMPUTE && !compute) return nv_slot_mask(g_nv.fusion_nodes[0].slot);
  if (g_nv.fusion.mode == IX_GP_FUSION_MODE_SPLIT_FRAME) {
    if (compute) return g_nv.fusion.fusion_mask;
    if (g_nv.dev_info.pcie_gen < 3u || g_nv.dev_info.pcie_lanes < 8u) return nv_slot_mask(g_nv.fusion_nodes[0].slot);
    if (pixel_count > 0u && pixel_count < 2073600u) return nv_slot_mask(g_nv.fusion_nodes[0].slot);
    return g_nv.fusion.fusion_mask;
  }
  if (g_nv.fusion.mode == IX_GP_FUSION_MODE_COMPUTE || g_nv.fusion.mode == IX_GP_FUSION_MODE_MIXED)
    return g_nv.fusion.fusion_mask;
  if (g_nv.fusion.mode == IX_GP_FUSION_MODE_ALT_FRAME) {
    ix_u8 idx = g_nv.fusion_rr++;
    if (idx >= g_nv.fusion.node_count) { idx = 0u; g_nv.fusion_rr = 1u; }
    return nv_slot_mask(g_nv.fusion_nodes[idx].slot);
  }
  return nv_slot_mask(g_nv.fusion_nodes[0].slot);
}

static void nv_dma_push_method_n(ix_u32 subch, ix_u32 method, const ix_u32* data, ix_u32 count);
static void nv_push_method(ix_u32 subch, ix_u32 method, const ix_u32* data, ix_u32 count);

static void nv_emit_fusion_select(ix_u64 mask, ix_u8 compute) {
  ix_u32 data[3];
  if (!g_nv.fusion_active) return;
  data[0] = (ix_u32)(mask & 0xFFFFFFFFu);
  data[1] = (ix_u32)(mask >> 32u);
  data[2] = (compute ? 1u : 0u) | ((ix_u32)g_nv.fusion.mode << 8u) | ((ix_u32)g_nv.fusion.frame_pacing << 16u);
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_push_method_n(NV_SUBCH_3D, NV_MTHD_FUSION_MASK_LO, data, 3u);
  } else {
    nv_push_method(NV_SUBCH_3D, NV_MTHD_FUSION_MASK_LO, data, 3u);
  }
}

static ix_u8 nv_arch_from_device(ix_u16 dev) {
  if (dev >= 0x2900u) return NV_ARCH_BLACKWELL;
  if (dev >= 0x2600u) return NV_ARCH_ADA;
  if (dev >= 0x2200u) return NV_ARCH_AMPERE;
  if (dev >= 0x1E00u) return NV_ARCH_TURING;
  if (dev >= 0x1B00u) return NV_ARCH_VOLTA;
  if (dev >= 0x1500u) return NV_ARCH_PASCAL;
  if (dev >= 0x1200u) return NV_ARCH_MAXWELL;
  if (dev >= 0x0FC0u) return NV_ARCH_KEPLER;
  if (dev >= 0x0D00u) return NV_ARCH_FERMI;
  if (dev >= 0x0600u) return NV_ARCH_TESLA;
  if (dev >= 0x0300u) return NV_ARCH_RANKINE;
  if (dev >= 0x0200u) return NV_ARCH_KELVIN;
  if (dev >= 0x0100u) return NV_ARCH_CELSIUS;
  if (dev >= 0x0020u) return NV_ARCH_TNT;
  if (dev >= 0x0010u) return NV_ARCH_NV3;
  return NV_ARCH_NV1;
}

typedef enum {
  NV_FW_CHIP_NULL = 0, NV_FW_CHIP_TU10X = 1, NV_FW_CHIP_TU11X = 2,
  NV_FW_CHIP_GA100 = 3, NV_FW_CHIP_GA10X = 4, NV_FW_CHIP_AD10X = 5,
  NV_FW_CHIP_GH100 = 6, NV_FW_CHIP_GB10X = 8, NV_FW_CHIP_GB20X = 9,
  NV_FW_CHIP_GR10X = 10, NV_FW_CHIP_GB10Y = 11, NV_FW_CHIP_GB20Y = 12
} nv_fw_chip_family_t;

static ix_u8 nv_fw_chip_family_from_arch(ix_u8 arch, ix_u16 dev) {
  switch (arch) {
    case NV_ARCH_TURING: return (dev >= 0x2180u) ? (ix_u8)NV_FW_CHIP_TU11X : (ix_u8)NV_FW_CHIP_TU10X;
    case NV_ARCH_AMPERE: return (dev >= 0x2300u) ? (ix_u8)NV_FW_CHIP_GA10X : (ix_u8)NV_FW_CHIP_GA100;
    case NV_ARCH_ADA: return (ix_u8)NV_FW_CHIP_AD10X;
    case NV_ARCH_BLACKWELL: return (dev >= 0x2C00u) ? (ix_u8)NV_FW_CHIP_GB20X : (ix_u8)NV_FW_CHIP_GB10X;
    default: return (ix_u8)NV_FW_CHIP_NULL;
  }
}

static void nv_detect_hw_caps(void) {
  nv_device_info_t* info = &g_nv.dev_info;
  memset(info, 0, sizeof(*info));
  info->arch = g_nv.arch;
  info->chipset = g_ixg_driver_hw.device_id;
  info->warp_size = 32u;
  switch (g_nv.arch) {
    case NV_ARCH_KEPLER:
      info->gpc_count = 3; info->tpc_per_gpc = 2; info->sm_per_tpc = 1;
      info->sm_version = 0x0350u; info->max_threads_per_sm = 2048;
      info->max_warps_per_sm = 64; info->shared_mem_per_sm = 49152;
      info->register_file_per_sm = 65536; break;
    case NV_ARCH_MAXWELL:
      info->gpc_count = 4; info->tpc_per_gpc = 4; info->sm_per_tpc = 1;
      info->sm_version = 0x0500u; info->max_threads_per_sm = 2048;
      info->max_warps_per_sm = 64; info->shared_mem_per_sm = 65536;
      info->register_file_per_sm = 65536; break;
    case NV_ARCH_PASCAL:
      info->gpc_count = 6; info->tpc_per_gpc = 5; info->sm_per_tpc = 1;
      info->sm_version = 0x0600u; info->max_threads_per_sm = 2048;
      info->max_warps_per_sm = 64; info->shared_mem_per_sm = 65536;
      info->register_file_per_sm = 65536; break;
    case NV_ARCH_VOLTA:
      info->gpc_count = 6; info->tpc_per_gpc = 7; info->sm_per_tpc = 2;
      info->sm_version = 0x0700u; info->has_tensor_cores = 1; info->tensor_core_count = 640;
      info->max_threads_per_sm = 2048; info->max_warps_per_sm = 64;
      info->shared_mem_per_sm = 98304; info->register_file_per_sm = 65536; break;
    case NV_ARCH_TURING:
      info->gpc_count = 6; info->tpc_per_gpc = 8; info->sm_per_tpc = 2;
      info->sm_version = 0x0750u; info->has_tensor_cores = 1; info->tensor_core_count = 544;
      info->has_rt_cores = 1; info->rt_core_count = 68; info->has_gsp = 1;
      info->max_threads_per_sm = 1024; info->max_warps_per_sm = 32;
      info->shared_mem_per_sm = 65536; info->register_file_per_sm = 65536; break;
    case NV_ARCH_AMPERE:
      info->gpc_count = 7; info->tpc_per_gpc = 12; info->sm_per_tpc = 2;
      info->sm_version = 0x0800u; info->has_tensor_cores = 1; info->tensor_core_count = 336;
      info->has_rt_cores = 1; info->rt_core_count = 84; info->has_gsp = 1;
      info->max_threads_per_sm = 1536; info->max_warps_per_sm = 48;
      info->shared_mem_per_sm = 102400; info->register_file_per_sm = 65536; break;
    case NV_ARCH_ADA:
      info->gpc_count = 12; info->tpc_per_gpc = 8; info->sm_per_tpc = 2;
      info->sm_version = 0x0890u; info->has_tensor_cores = 1; info->tensor_core_count = 512;
      info->has_rt_cores = 1; info->rt_core_count = 128; info->has_gsp = 1;
      info->max_threads_per_sm = 1536; info->max_warps_per_sm = 48;
      info->shared_mem_per_sm = 102400; info->register_file_per_sm = 65536; break;
    case NV_ARCH_BLACKWELL:
      info->gpc_count = 12; info->tpc_per_gpc = 12; info->sm_per_tpc = 2;
      info->sm_version = 0x0A00u; info->has_tensor_cores = 1; info->tensor_core_count = 768;
      info->has_rt_cores = 1; info->rt_core_count = 192; info->has_gsp = 1;
      info->max_threads_per_sm = 2048; info->max_warps_per_sm = 64;
      info->shared_mem_per_sm = 131072; info->register_file_per_sm = 65536; break;
    default:
      info->gpc_count = 1; info->tpc_per_gpc = 1; info->sm_per_tpc = 1;
      info->sm_version = 0x0100u; break;
  }
  info->total_sm_count = (ix_u32)info->gpc_count * (ix_u32)info->tpc_per_gpc * (ix_u32)info->sm_per_tpc;
  info->fw_chip_family = nv_fw_chip_family_from_arch(g_nv.arch, (ix_u16)g_ixg_driver_hw.device_id);
  info->vram_mb = ixg_mutant_estimate_vram_mb(0x10DEu, g_ixg_driver_hw.device_id, 0);
  if (info->vram_mb == 0u) info->vram_mb = 4096u;
  ixg_driver_detect_pcie_caps(g_ixg_driver_hw.bus, g_ixg_driver_hw.slot, g_ixg_driver_hw.func, &info->pcie_gen, &info->pcie_lanes);
}

static void nv_pick_classes(void) {
  g_nv.arch = nv_arch_from_device(g_ixg_driver_hw.device_id);
  g_nv.legacy_path = (g_nv.arch < NV_ARCH_KEPLER) ? 1u : 0u;
  g_nv.class_ce = 0u;
  g_nv.class_m2mf = NV_OBJ_M2MF;
  if (g_nv.arch < NV_ARCH_CELSIUS) { g_nv.class_3d = NV_OBJ_LEGACY_DX5_TRIANGLE; g_nv.class_compute = 0u; }
  else if (g_nv.arch < NV_ARCH_KELVIN) { g_nv.class_3d = NV_OBJ_LEGACY_CELSIUS; g_nv.class_compute = 0u; }
  else if (g_nv.arch < NV_ARCH_RANKINE) { g_nv.class_3d = NV_OBJ_LEGACY_KELVIN; g_nv.class_compute = 0u; }
  else if (g_nv.arch < NV_ARCH_TESLA) { g_nv.class_3d = NV_OBJ_LEGACY_RANKINE; g_nv.class_compute = 0u; }
  else if (g_nv.arch < NV_ARCH_KEPLER) { g_nv.class_3d = NV_OBJ_LEGACY_TESLA; g_nv.class_compute = 0u; }
  else if (g_nv.arch >= NV_ARCH_BLACKWELL) { g_nv.class_3d = NV_OBJ_3D_BLACKWELL; g_nv.class_compute = NV_OBJ_COMPUTE_BLACKWELL; g_nv.class_ce = NV_OBJ_CE_BLACKWELL; }
  else if (g_nv.arch >= NV_ARCH_ADA) { g_nv.class_3d = NV_OBJ_3D_ADA; g_nv.class_compute = NV_OBJ_COMPUTE_ADA; g_nv.class_ce = NV_OBJ_CE_ADA; }
  else if (g_nv.arch >= NV_ARCH_AMPERE) { g_nv.class_3d = NV_OBJ_3D_AMPERE; g_nv.class_compute = NV_OBJ_COMPUTE_AMPERE; g_nv.class_ce = NV_OBJ_CE_AMPERE; }
  else if (g_nv.arch >= NV_ARCH_TURING) { g_nv.class_3d = NV_OBJ_3D_TURING; g_nv.class_compute = NV_OBJ_COMPUTE_TURING; g_nv.class_ce = NV_OBJ_CE_TURING; }
  else if (g_nv.arch >= NV_ARCH_VOLTA) { g_nv.class_3d = NV_OBJ_3D_VOLTA; g_nv.class_compute = NV_OBJ_COMPUTE_VOLTA; g_nv.class_ce = NV_OBJ_CE_VOLTA; }
  else if (g_nv.arch >= NV_ARCH_PASCAL) { g_nv.class_3d = NV_OBJ_3D_PASCAL; g_nv.class_compute = NV_OBJ_COMPUTE_PASCAL; g_nv.class_ce = NV_OBJ_CE_PASCAL; }
  else if (g_nv.arch >= NV_ARCH_MAXWELL) { g_nv.class_3d = NV_OBJ_3D_MAXWELL; g_nv.class_compute = NV_OBJ_COMPUTE_MAXWELL; g_nv.class_ce = NV_OBJ_CE_MAXWELL; }
  else { g_nv.class_3d = NV_OBJ_3D_KEPLER; g_nv.class_compute = NV_OBJ_COMPUTE_KEPLER; g_nv.class_ce = NV_OBJ_CE_KEPLER; }
  if (g_nv.arch >= NV_ARCH_BLACKWELL) g_nv.class_m2mf = g_nv.class_ce;
  nv_detect_hw_caps();
}

static int nv_dma_pushbuf_init(void) {
  void* cpu = malloc(NV_DMA_PUSHBUF_CAP);
  if (!cpu) return -1;
  memset(&g_nv.pushbuf, 0, sizeof(g_nv.pushbuf));
  g_nv.pushbuf.pman_handle = 0;
  g_nv.pushbuf.gpu_addr = (ix_u64)(uintptr_t)cpu;
  g_nv.pushbuf.cpu_ptr = (ix_u32*)cpu;
  g_nv.pushbuf.capacity_dwords = NV_DMA_PUSHBUF_DWORDS;
  g_nv.pushbuf.state = 0u;
  return 0;
}

static int nv_dma_ib_init(void) {
  void* cpu = malloc(NV_DMA_IB_ENTRIES * (ix_u32)sizeof(nv_dma_ib_entry_t));
  if (!cpu) return -1;
  memset(&g_nv.ib_ring, 0, sizeof(g_nv.ib_ring));
  g_nv.ib_ring.count = NV_DMA_IB_ENTRIES;
  g_nv.ib_ring.pman_handle = 0;
  g_nv.ib_ring.gpu_addr = (ix_u64)(uintptr_t)cpu;
  g_nv.ib_ring.cpu_ptr = (nv_dma_ib_entry_t*)cpu;
  return 0;
}

static int nv_dma_notifier_init(void) {
  void* cpu = malloc(NV_DMA_NOTIFIER_SLOTS * sizeof(ix_u32));
  if (!cpu) return -1;
  g_nv.notifier.pman_handle = 0;
  g_nv.notifier.gpu_addr = (ix_u64)(uintptr_t)cpu;
  g_nv.notifier.cpu_ptr = (volatile ix_u32*)cpu;
  g_nv.notifier.slot_count = NV_DMA_NOTIFIER_SLOTS;
  memset((void*)g_nv.notifier.cpu_ptr, 0, NV_DMA_NOTIFIER_SLOTS * sizeof(ix_u32));
  return 0;
}

static int nv_dma_semaphore_init(void) {
  void* cpu = malloc(NV_DMA_SEMAPHORE_SLOTS * sizeof(ix_u32));
  if (!cpu) return -1;
  g_nv.semaphore.pman_handle = 0;
  g_nv.semaphore.gpu_addr = (ix_u64)(uintptr_t)cpu;
  g_nv.semaphore.cpu_ptr = (volatile ix_u32*)cpu;
  g_nv.semaphore.slot_count = NV_DMA_SEMAPHORE_SLOTS;
  memset((void*)g_nv.semaphore.cpu_ptr, 0, NV_DMA_SEMAPHORE_SLOTS * sizeof(ix_u32));
  return 0;
}

static void nv_dma_begin(void) {
  nv_dma_pushbuf_t* pb = &g_nv.pushbuf;
  if (!pb->cpu_ptr) return;
  pb->put = 0u;
  pb->used_dwords = 0u;
  pb->state = 1u;
}

static void nv_dma_push(ix_u32 word) {
  nv_dma_pushbuf_t* pb = &g_nv.pushbuf;
  if (!pb->cpu_ptr || pb->put >= pb->capacity_dwords) { pb->overflow_count++; return; }
  pb->cpu_ptr[pb->put++] = word;
  pb->used_dwords++;
}

static void nv_dma_push_method_hdr(ix_u32 subch, ix_u32 method, ix_u32 count) {
  nv_dma_push(NV_METHOD_TYPE_INC | ((count & 0x1FFFu) << 18) | ((subch & 7u) << 13) | (method & 0x1FFCu));
}

static void nv_dma_push_method1(ix_u32 subch, ix_u32 method, ix_u32 val) {
  nv_dma_push_method_hdr(subch, method, 1u);
  nv_dma_push(val);
}

static void nv_dma_push_method_n(ix_u32 subch, ix_u32 method, const ix_u32* data, ix_u32 count) {
  ix_u32 i;
  nv_dma_push_method_hdr(subch, method, count);
  for (i = 0u; i < count; i++) nv_dma_push(data[i]);
}

static void nv_dma_submit(void) {
  nv_dma_pushbuf_t* pb = &g_nv.pushbuf;
  nv_dma_ib_ring_t* ib = &g_nv.ib_ring;
  if (!pb->cpu_ptr || pb->used_dwords == 0u) return;
  if (ib->cpu_ptr && ib->count > 0u) {
    ix_u32 idx = ib->put % ib->count;
    ib->cpu_ptr[idx].gpu_addr = pb->gpu_addr;
    ib->cpu_ptr[idx].length_dwords = pb->used_dwords;
    ib->cpu_ptr[idx].flags = 0u;
    ib->put++;
  }
  ixg_driver_kick_ring(0x10DEu);
  pb->fence_seq = ++g_nv.sequence;
  pb->state = 2u;
}

static void nv_dma_emit_semaphore(ix_u32 slot, ix_u32 value) {
  ix_u64 addr;
  ix_u32 data[4];
  if (!g_nv.semaphore.cpu_ptr || slot >= g_nv.semaphore.slot_count) return;
  addr = g_nv.semaphore.gpu_addr + (ix_u64)slot * 4u;
  data[0] = (ix_u32)(addr >> 32);
  data[1] = (ix_u32)(addr & 0xFFFFFFFFu);
  data[2] = value;
  data[3] = 0x01u;
  nv_dma_push_method_n(NV_SUBCH_3D, NV_MTHD_SEMAPHORE_ADDR_HI, data, 4u);
}

static ix_u32 nv_method(ix_u32 subch, ix_u32 method, ix_u32 count) {
  return NV_METHOD_TYPE_INC | ((count & 0x1FFFu) << 18) | ((subch & 7u) << 13) | (method & 0x1FFCu);
}

static void nv_ring_write_slot(const ix_u32 words[8]) {
  ix_u32 used;
  ix_u32 spins = NV_FIFO_FREE_SPINS;
  ix_u8 ok = 0u;
  while (spins--) {
    used = ixg_driver_ring_used();
    if (used < (IXG_DRIVER_CMD_RING_CAP - 2u)) { ok = 1u; break; }
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

static void nv_push_words(const ix_u32* words, ix_u32 count) {
  ix_u32 i = 0u;
  while (i < count) {
    ix_u32 slot[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    ix_u32 n = count - i;
    if (n > 8u) n = 8u;
    for (ix_u32 k = 0u; k < n; k++) slot[k] = words[i + k];
    nv_ring_write_slot(slot);
    if (g_ixg_driver_hw.submit_failed) return;
    i += n;
  }
  ixg_driver_kick_ring(0x10DEu);
}

static void nv_push_method(ix_u32 subch, ix_u32 method, const ix_u32* data, ix_u32 count) {
  ix_u32 buf[NV_PUSH_MAX_DWORDS];
  if (!data || count == 0u || count + 1u > NV_PUSH_MAX_DWORDS) return;
  buf[0] = nv_method(subch, method, count);
  for (ix_u32 i = 0u; i < count; i++) buf[i + 1u] = data[i];
  nv_push_words(buf, count + 1u);
}

static void nv_push_method1(ix_u32 subch, ix_u32 method, ix_u32 v0) {
  ix_u32 data = v0;
  nv_push_method(subch, method, &data, 1u);
}

static ix_u64 nv_stage(const void* src, ix_u32 bytes) {
  ix_u64 gpu = ixg_driver_dma_stage_upload(src, bytes);
  if (gpu) { ix_iommu_assert_safe(IX_IOMMU_DOMAIN_GPU, gpu, bytes, IX_IOMMU_FLAG_READ); return gpu; }
  return (ix_u64)(uintptr_t)src;
}

static void nv_bind_objects(void) {
  if (g_nv.bound_objects) return;
  nv_pick_classes();
  nv_refresh_fusion();
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    nv_dma_push_method1(NV_SUBCH_M2MF, NV_MTHD_SET_OBJECT, g_nv.class_m2mf);
    nv_dma_push_method1(NV_SUBCH_2D, NV_MTHD_SET_OBJECT, NV_OBJ_2D);
    nv_dma_push_method1(NV_SUBCH_3D, NV_MTHD_SET_OBJECT, g_nv.class_3d);
    if (g_nv.class_compute) nv_dma_push_method1(NV_SUBCH_COMPUTE, NV_MTHD_SET_OBJECT, g_nv.class_compute);
    if (g_nv.class_ce) nv_dma_push_method1(NV_SUBCH_CE, NV_MTHD_SET_OBJECT, g_nv.class_ce);
    nv_dma_submit();
  } else {
    nv_push_method1(NV_SUBCH_M2MF, NV_MTHD_SET_OBJECT, g_nv.legacy_path ? NV_OBJ_LEGACY_IMAGE_BLIT : g_nv.class_m2mf);
    nv_push_method1(NV_SUBCH_2D, NV_MTHD_SET_OBJECT, g_nv.legacy_path ? NV_OBJ_LEGACY_RECT : NV_OBJ_2D);
    nv_push_method1(NV_SUBCH_3D, NV_MTHD_SET_OBJECT, g_nv.class_3d);
    if (g_nv.class_compute) nv_push_method1(NV_SUBCH_COMPUTE, NV_MTHD_SET_OBJECT, g_nv.class_compute);
  }
  g_nv.bound_objects = 1u;
}

static void nv_flush(void) {
  nv_bind_objects();
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    nv_dma_push_method1(NV_SUBCH_3D, NV_MTHD_NO_OPERATION, ++g_nv.sequence);
    nv_dma_emit_semaphore(0u, g_nv.sequence);
    nv_dma_submit();
  } else {
    nv_push_method1(NV_SUBCH_3D, NV_MTHD_NO_OPERATION, ++g_nv.sequence);
    ixg_driver_kick_ring(0x10DEu);
  }
  (void)ixg_driver_wait_idle(IXG_DRIVER_MMIO_POLL_SPINS);
}

static void nv_draw_rect(int x, int y, int w, int h, ix_u32 color) {
  ix_u32 data[8];
  if (w <= 0 || h <= 0) return;
  nv_bind_objects();
  nv_emit_fusion_select(nv_select_fusion_mask(0u, (ix_u32)(w * h)), 0u);
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    data[0] = 0xCFu;
    data[1] = g_nv.fb_pitch ? g_nv.fb_pitch : 4096u;
    data[2] = (ix_u32)(g_nv.fb_gpu_addr & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_2D, NV_2D_SET_DST_FORMAT, data, 3u);
    nv_dma_push_method1(NV_SUBCH_2D, NV_2D_SET_COLOR_FORMAT, 0xCFu);
    nv_dma_push_method1(NV_SUBCH_2D, NV_2D_SET_COLOR, color);
    nv_dma_push_method1(NV_SUBCH_2D, NV_2D_RENDER_SOLID_PRIM_MODE, 1u);
    data[0] = ((ix_u32)(ix_u16)x << 16) | (ix_u32)(ix_u16)y;
    data[1] = ((ix_u32)(ix_u16)w << 16) | (ix_u32)(ix_u16)h;
    nv_dma_push_method_n(NV_SUBCH_2D, NV_2D_RENDER_SOLID_PRIM_POINT_SET_X, data, 2u);
    nv_dma_submit();
    return;
  }
  if (g_nv.legacy_path) {
    data[0] = color;
    data[1] = ((ix_u32)(ix_u16)x << 16) | (ix_u32)(ix_u16)y;
    data[2] = ((ix_u32)(ix_u16)w << 16) | (ix_u32)(ix_u16)h;
    nv_push_method(NV_SUBCH_2D, NV_2D_SET_COLOR, data, 3u);
    return;
  }
  data[0] = 0xCFu;
  data[1] = g_nv.fb_pitch ? g_nv.fb_pitch : 4096u;
  data[2] = (ix_u32)(g_nv.fb_gpu_addr & 0xFFFFFFFFu);
  nv_push_method(NV_SUBCH_2D, NV_2D_SET_DST_FORMAT, data, 3u);
  nv_push_method1(NV_SUBCH_2D, NV_2D_SET_COLOR_FORMAT, 0xCFu);
  nv_push_method1(NV_SUBCH_2D, NV_2D_SET_COLOR, color);
  nv_push_method1(NV_SUBCH_2D, NV_2D_RENDER_SOLID_PRIM_MODE, 1u);
  data[0] = ((ix_u32)(ix_u16)x << 16) | (ix_u32)(ix_u16)y;
  data[1] = ((ix_u32)(ix_u16)w << 16) | (ix_u32)(ix_u16)h;
  nv_push_method(NV_SUBCH_2D, NV_2D_RENDER_SOLID_PRIM_POINT_SET_X, data, 2u);
}

static void nv_bitblt(int dx, int dy, int w, int h, void* src, int sx, int sy) {
  ix_u64 gpu;
  ix_u32 data[9];
  if (!src || w <= 0 || h <= 0 || sx < 0 || sy < 0) return;
  nv_bind_objects();
  nv_emit_fusion_select(nv_select_fusion_mask(0u, (ix_u32)(w * h)), 0u);
  gpu = nv_stage(src, (ix_u32)w * (ix_u32)h * 4u);
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    data[0] = (ix_u32)(gpu >> 32);
    data[1] = (ix_u32)(g_nv.fb_gpu_addr >> 32);
    nv_dma_push_method_n(NV_SUBCH_M2MF, NV_M2MF_OFFSET_IN_HIGH, data, 2u);
    data[0] = (ix_u32)(gpu & 0xFFFFFFFFu);
    data[1] = (ix_u32)(g_nv.fb_gpu_addr + ((ix_u64)(ix_u32)dy * (g_nv.fb_pitch ? g_nv.fb_pitch : (ix_u32)w * 4u)) + (ix_u64)(ix_u32)dx * 4u);
    data[2] = (ix_u32)w * 4u;
    data[3] = g_nv.fb_pitch ? g_nv.fb_pitch : (ix_u32)w * 4u;
    data[4] = (ix_u32)w * 4u;
    data[5] = (ix_u32)h;
    data[6] = 0u;
    nv_dma_push_method_n(NV_SUBCH_M2MF, NV_M2MF_OFFSET_IN_LOW, data, 7u);
    nv_dma_push_method1(NV_SUBCH_M2MF, NV_M2MF_EXEC, 0x111u);
    nv_dma_submit();
    return;
  }
  if (g_nv.legacy_path) {
    data[0] = (ix_u32)(gpu & 0xFFFFFFFFu);
    data[1] = (ix_u32)w * 4u;
    data[2] = ((ix_u32)(ix_u16)dx << 16) | (ix_u32)(ix_u16)dy;
    data[3] = ((ix_u32)(ix_u16)w << 16) | (ix_u32)(ix_u16)h;
    nv_push_method(NV_SUBCH_M2MF, NV_M2MF_OFFSET_IN_LOW, data, 4u);
    return;
  }
  data[0] = (ix_u32)(gpu >> 32);
  data[1] = (ix_u32)(g_nv.fb_gpu_addr >> 32);
  nv_push_method(NV_SUBCH_M2MF, NV_M2MF_OFFSET_IN_HIGH, data, 2u);
  data[0] = (ix_u32)(gpu & 0xFFFFFFFFu);
  data[1] = (ix_u32)(g_nv.fb_gpu_addr + ((ix_u64)(ix_u32)dy * (g_nv.fb_pitch ? g_nv.fb_pitch : (ix_u32)w * 4u)) + (ix_u64)(ix_u32)dx * 4u);
  data[2] = (ix_u32)w * 4u;
  data[3] = g_nv.fb_pitch ? g_nv.fb_pitch : (ix_u32)w * 4u;
  data[4] = (ix_u32)w * 4u;
  data[5] = (ix_u32)h;
  data[6] = 0u;
  nv_push_method(NV_SUBCH_M2MF, NV_M2MF_OFFSET_IN_LOW, data, 7u);
  nv_push_method1(NV_SUBCH_M2MF, NV_M2MF_EXEC, 0x111u);
}

static int nv_rect_blend(int x, int y, int w, int h, ix_u32 color, ix_u8 premultiplied) {
  (void)premultiplied;
  if (w <= 0 || h <= 0) return -1;
  nv_draw_rect(x, y, w, h, color);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int nv_bitblt_alpha(int dx, int dy, int w, int h, const void* src, ix_u32 stride_bytes, ix_u8 premultiplied) {
  (void)stride_bytes; (void)premultiplied;
  if (!src || w <= 0 || h <= 0) return -1;
  nv_bitblt(dx, dy, w, h, (void*)src, 0, 0);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int nv_emit_vertices(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture) {
  ix_u32 total = triangle_count * 3u;
  ix_u32 i;
  if (!vertices || triangle_count == 0u) return -1;
  (void)texture;
  nv_bind_objects();
  nv_emit_fusion_select(nv_select_fusion_mask(0u, 0u), 0u);
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    ix_u32 size_bytes = total * (ix_u32)sizeof(ixg_hw_vertex_t);
    void* vbo_cpu = malloc(size_bytes);
    if (vbo_cpu) {
      memcpy(vbo_cpu, vertices, size_bytes);
      ix_u64 vbo_gpu = (ix_u64)(uintptr_t)vbo_cpu;
      ix_u32 data[4];
      nv_dma_begin();
      data[0] = (ix_u32)(vbo_gpu >> 32);
      data[1] = (ix_u32)(vbo_gpu & 0xFFFFFFFFu);
      nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_START_HIGH(0), data, 2u);
      data[0] = (ix_u32)((vbo_gpu + size_bytes - 1u) >> 32);
      data[1] = (ix_u32)((vbo_gpu + size_bytes - 1u) & 0xFFFFFFFFu);
      nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_LIMIT_HIGH(0), data, 2u);
      nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BEGIN_GL, 4u);
      nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_FIRST, 0u);
      nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_COUNT, total);
      nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_END_GL, 0u);
      nv_dma_submit();
      free(vbo_cpu);
      return g_ixg_driver_hw.submit_failed ? -1 : 0;
    }
  }
  if (g_nv.legacy_path) {
    nv_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BEGIN_GL, 4u);
    for (i = 0u; i < total; i++) {
      ix_u32 v[7];
      v[0] = nv_float_bits(vertices[i].x);
      v[1] = nv_float_bits(vertices[i].y);
      v[2] = nv_float_bits(vertices[i].z);
      v[3] = vertices[i].color;
      v[4] = nv_float_bits(vertices[i].u);
      v[5] = nv_float_bits(vertices[i].v);
      v[6] = texture ? 1u : 0u;
      nv_push_method(NV_SUBCH_3D, NV_3D_VERTEX_DATA, v, 7u);
    }
    nv_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_END_GL, 0u);
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  nv_push_method1(NV_SUBCH_3D, NV_3D_SET_ZETA_ENABLE, 0u);
  {
    ix_u32 rt_data[2];
    rt_data[0] = 1u;
    rt_data[1] = 0xCFu;
    nv_push_method(NV_SUBCH_3D, NV_3D_SET_RT_ENABLE, rt_data, 2u);
  }
  nv_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BEGIN_GL, 4u);
  for (i = 0u; i < total; i++) {
    ix_u32 v[7];
    v[0] = nv_float_bits(vertices[i].x);
    v[1] = nv_float_bits(vertices[i].y);
    v[2] = nv_float_bits(vertices[i].z);
    v[3] = nv_float_bits(vertices[i].rhw);
    v[4] = nv_float_bits(vertices[i].u);
    v[5] = nv_float_bits(vertices[i].v);
    v[6] = vertices[i].color;
    nv_push_method(NV_SUBCH_3D, NV_3D_VERTEX_DATA, v, 7u);
  }
  nv_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_END_GL, 0u);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int nv_triangles(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count) {
  return nv_emit_vertices(vertices, triangle_count, 0);
}

static int nv_triangles_tex(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture) {
  if (!texture) return -1;
  return nv_emit_vertices(vertices, triangle_count, texture);
}

/*
 * CPU-visible render target (см. ixg_backend_intel.c): кадр
 * рендерится в буфер системной RAM, отображённый в aperture ixg.
 * fb_* в g_nv перенаправляются на него, так что и 3D-путь, и
 * BLT-операции пишут в CPU-видимую память; readback отдаёт кадр
 * ядру для canvas окна (видимый GPU-вывод AxGL в режиме AUTO).
 */
static int nv_set_render_target(ix_u32 w, ix_u32 h) {
  if (ixg_driver_render_target_setup(w, h) != 0) return -1;
  g_nv.fb_width    = w;
  g_nv.fb_height   = h;
  g_nv.fb_pitch    = ixg_driver_render_target_stride_px() * 4u;
  g_nv.fb_gpu_addr = ixg_driver_render_target_gpu_addr();
  return 0;
}

static int nv_readback_rect(ix_u32* dst, ix_u32 w, ix_u32 h) {
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

static int nv_compute(const void* dispatch_desc) {
  ix_u64 gpu;
  ix_u32 data[4];
  if (!dispatch_desc) return -1;
  nv_bind_objects();
  nv_emit_fusion_select(nv_select_fusion_mask(1u, 0u), 1u);
  if (!g_nv.class_compute) return -1;
  gpu = nv_stage(dispatch_desc, sizeof(ixg_compute_dispatch_desc_t));
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    data[0] = (ix_u32)(gpu >> 32);
    data[1] = (ix_u32)(gpu & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_COMPUTE, NV_COMPUTE_SET_PROGRAM_REGION, data, 2u);
    nv_dma_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_SET_LAUNCH_ID, ++g_nv.sequence);
    nv_dma_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_LAUNCH, 1u);
    nv_dma_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  data[0] = (ix_u32)(gpu >> 32);
  data[1] = (ix_u32)(gpu & 0xFFFFFFFFu);
  nv_push_method(NV_SUBCH_COMPUTE, NV_COMPUTE_SET_PROGRAM_REGION, data, 2u);
  nv_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_SET_LAUNCH_ID, ++g_nv.sequence);
  nv_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_LAUNCH, 1u);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

static int nv_trace(const void* trace_desc) { return nv_compute(trace_desc); }
static int nv_control(const void* control_desc, void* control_result) { (void)control_result; return nv_compute(control_desc); }

static ix_u16 nv_gsp_read_le16(const ix_u8* p) {
  return (ix_u16)(((ix_u16)p[0]) | ((ix_u16)p[1] << 8));
}
static ix_u64 nv_gsp_read_le64(const ix_u8* p) {
  return ((ix_u64)p[0]) | ((ix_u64)p[1] << 8) | ((ix_u64)p[2] << 16) |
         ((ix_u64)p[3] << 24) | ((ix_u64)p[4] << 32) | ((ix_u64)p[5] << 40) |
         ((ix_u64)p[6] << 48) | ((ix_u64)p[7] << 56);
}
static ix_u32 nv_gsp_read_le32(const ix_u8* p) {
  return ((ix_u32)p[0]) | ((ix_u32)p[1] << 8) | ((ix_u32)p[2] << 16) | ((ix_u32)p[3] << 24);
}
static ix_u32 nv_gsp_fnv32(const ix_u8* p, ix_u32 n) {
  ix_u32 h = 2166136261u;
  for (ix_u32 i = 0u; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}
static ix_u32 nv_gsp_detect_format(const ix_u8* p, ix_u32 fw_size) {
  if (!p || fw_size < 0x1000u) return NV_GSP_FW_FORMAT_UNKNOWN;
  if (p[0] == 0x4Eu && p[1] == 0x56u && p[2] == 0x47u && p[3] == 0x46u) return NV_GSP_FW_FORMAT_NVGF;
  if (p[0] == 0x7Fu && p[1] == 0x45u && p[2] == 0x4Cu && p[3] == 0x46u) return NV_GSP_FW_FORMAT_ELF;
  return NV_GSP_FW_FORMAT_RAW;
}
static int nv_gsp_validate_elf(const ix_u8* p, ix_u32 fw_size) {
  ix_u16 e_phnum, e_phentsize;
  ix_u64 e_phoff;
  ix_u8 ei_class;
  if (fw_size < 0x40u) return 0;
  ei_class = p[4];
  if (ei_class != 2u) return 0;
  if (p[16] != 2u && p[16] != 3u) return 0;
  e_phoff = nv_gsp_read_le64(p + 0x20u);
  e_phentsize = (ix_u16)nv_gsp_read_le16(p + 0x36u);
  e_phnum = (ix_u16)nv_gsp_read_le16(p + 0x38u);
  if (e_phnum == 0u || e_phentsize < 0x38u) return 0;
  if (e_phoff + (ix_u64)e_phnum * e_phentsize > fw_size) return 0;
  return 1;
}
static int nv_gsp_validate_firmware(const void* fw_data, ix_u32 fw_size) {
  const ix_u8* p = (const ix_u8*)fw_data;
  ix_u32 fmt;
  if (!p || fw_size < 0x1000u || fw_size > (8u * 1024u * 1024u)) return 0;
  fmt = nv_gsp_detect_format(p, fw_size);
  if (fmt == NV_GSP_FW_FORMAT_NVGF) {
    ix_u32 header_ck = nv_gsp_read_le32(p + 8u);
    ix_u32 trailer_ck = nv_gsp_read_le32(p + fw_size - 4u);
    if (header_ck) return nv_gsp_fnv32(p + 16u, fw_size - 16u) == header_ck;
    if (trailer_ck) return nv_gsp_fnv32(p, fw_size - 4u) == trailer_ck;
    return 0;
  }
  if (fmt == NV_GSP_FW_FORMAT_ELF) return nv_gsp_validate_elf(p, fw_size);
  if (fmt == NV_GSP_FW_FORMAT_RAW) return (fw_size >= 0x4000u) ? 1 : 0;
  return 0;
}

static ix_u32 nv_falcon_gsp_mmio_base(void) {
  if (g_nv.arch >= NV_ARCH_BLACKWELL) return NV_GSP_MMIO_BASE_BLACKWELL;
  if (g_nv.arch >= NV_ARCH_ADA) return NV_GSP_MMIO_BASE_ADA;
  if (g_nv.arch >= NV_ARCH_AMPERE) return NV_GSP_MMIO_BASE_AMPERE;
  return NV_GSP_MMIO_BASE_TURING;
}

static int nv_falcon_dma_boot(ix_u32 mmio_base, ix_u64 fw_gpu_addr, ix_u32* out_status) {
  ix_u32 spins = 100000u;
  if (!g_ixg_driver_hw.mmio) return -1;
  ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFBASE, (ix_u32)(fw_gpu_addr >> 8));
  ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFMOFFS, 0u);
  ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFFBOFFS, 0u);
  ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFCMD, 0x12u);
  ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_BOOTVEC, 0u);
  if (g_nv.arch >= NV_ARCH_BLACKWELL) {
    ixg_driver_mmio_write(0x0011A140u, 1u);
    ixg_driver_mmio_write(0x0011A100u, 1u);
  } else {
    ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_CPUCTL, 0x02u);
  }
  ixg_driver_mmio_flush_posted();
  while (spins--) {
    ix_u32 status = ixg_driver_mmio_read(mmio_base + NV_GSP_FALCON_STATUS);
    if ((status & 0x10u) != 0u) {
      if (out_status) *out_status = status;
      return 0;
    }
    IXG_INLINE_PAUSE();
  }
  return -1;
}

int ixg_backend_nvidia_gsp_load_firmware(const void* fw_data, ix_u32 fw_size) {
  void* cpu;
  ix_u32 mmio_base;
  nv_gsp_state_info_t* gsp = &g_nv.gsp;
  if (!fw_data || fw_size == 0u) return -1;
  if (fw_size < 0x1000u || fw_size > (8u * 1024u * 1024u)) return -1;
  if (!g_nv.dev_info.has_gsp) return -1;
  if (g_nv.arch < NV_ARCH_TURING) return -1;
  if (!nv_gsp_validate_firmware(fw_data, fw_size)) return -1;
  cpu = malloc(fw_size);
  if (!cpu) { gsp->state = (ix_u8)NV_GSP_STATE_ERROR; gsp->error_code = 0xDEAD0001u; return -1; }
  memcpy(cpu, fw_data, fw_size);
  gsp->pman_handle = 0;
  gsp->gpu_addr = (ix_u64)(uintptr_t)cpu;
  gsp->cpu_ptr = cpu;
  gsp->total_size = fw_size;
  gsp->state = (ix_u8)NV_GSP_STATE_LOADING;
  gsp->fw_format = nv_gsp_detect_format((const ix_u8*)fw_data, fw_size);
  gsp->image_load_addr = gsp->gpu_addr;
  gsp->image_size = fw_size;
  gsp->segment_count = 1u;
  gsp->segments[0].load_addr = gsp->gpu_addr;
  gsp->segments[0].size_bytes = fw_size;
  gsp->segments[0].flags = 0u;
  mmio_base = nv_falcon_gsp_mmio_base();
  if (g_ixg_driver_hw.mmio) {
    ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFBASE, (ix_u32)(gsp->gpu_addr >> 8));
    ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFMOFFS, 0u);
    ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFFBOFFS, 0u);
    ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_DMATRFCMD, 0x12u);
    ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_BOOTVEC, 0u);
    ixg_driver_mmio_write(mmio_base + NV_GSP_FALCON_CPUCTL, 0x02u);
    ixg_driver_mmio_flush_posted();
  }
  {
    ix_u32 spins = 100000u;
    while (spins--) {
      if (g_ixg_driver_hw.mmio) {
        ix_u32 status = ixg_driver_mmio_read(mmio_base + NV_GSP_FALCON_STATUS);
        if ((status & 0x10u) != 0u) {
          gsp->state = (ix_u8)NV_GSP_STATE_RUNNING;
          gsp->boot_status = status;
          return 0;
        }
      }
      IXG_INLINE_PAUSE();
    }
  }
  gsp->state = (ix_u8)NV_GSP_STATE_ERROR;
  gsp->error_code = 0xDEAD0002u;
  return -1;
}

int ixg_backend_nvidia_gsp_status(nv_gsp_state_info_t* out_state) {
  if (!out_state) return -1;
  *out_state = g_nv.gsp;
  return 0;
}

int ixg_backend_nvidia_gsp_set_booter(const void* booter_data, ix_u32 booter_size) {
  void* cpu;
  nv_gsp_state_info_t* gsp = &g_nv.gsp;
  if (!booter_data || booter_size == 0u) return -1;
  if (booter_size < 0x400u || booter_size > (2u * 1024u * 1024u)) return -1;
  if (!g_nv.dev_info.has_gsp || g_nv.arch < NV_ARCH_TURING) return -1;
  if (gsp->booter_pman_handle) { free((void*)gsp->booter_cpu_ptr); gsp->booter_pman_handle = 0; gsp->booter_loaded = 0; }
  cpu = malloc(booter_size);
  if (!cpu) return -1;
  memcpy(cpu, booter_data, booter_size);
  ix_iommu_register_dma(IX_IOMMU_DOMAIN_GPU, (ix_u64)(uintptr_t)cpu, booter_size, IX_IOMMU_FLAG_READ | IX_IOMMU_FLAG_DEVICE);
  gsp->booter_pman_handle = 0;
  gsp->booter_gpu_addr = (ix_u64)(uintptr_t)cpu;
  gsp->booter_cpu_ptr = cpu;
  gsp->booter_size = booter_size;
  gsp->booter_loaded = 1u;
  return 0;
}

int ixg_backend_nvidia_falcon_boot(ix_u8 engine, ix_u32 mmio_base, const void* fw_data, ix_u32 fw_size) {
  void* cpu;
  ix_u32 status = 0u;
  nv_falcon_state_t* fal;
  if (!fw_data || fw_size == 0u || engine >= NV_FALCON_MAX_ENGINES) return -1;
  if (g_nv.arch < NV_ARCH_TURING) return -1;
  if (mmio_base == 0u) mmio_base = nv_falcon_gsp_mmio_base();
  cpu = malloc(fw_size);
  if (!cpu) return -1;
  memcpy(cpu, fw_data, fw_size);
  fal = &g_nv.falcons[engine];
  fal->engine = engine;
  fal->mmio_base = mmio_base;
  fal->fw_pman_handle = 0;
  fal->fw_gpu_addr = (ix_u64)(uintptr_t)cpu;
  fal->fw_cpu_ptr = cpu;
  fal->fw_size = fw_size;
  fal->ready = 0u;
  if (nv_falcon_dma_boot(mmio_base, fal->fw_gpu_addr, &status) != 0) return -1;
  fal->version = (ix_u16)(status & 0xFFFFu);
  fal->ready = 1u;
  return 0;
}

int ixg_backend_nvidia_gsp_rm_init(void) { return 0; }
int ixg_backend_nvidia_gsp_rm_call(ix_u32 function, const void* in_data, ix_u32 in_len, void* out_data, ix_u32 out_cap, ix_u32* out_len) {
  (void)function; (void)in_data; (void)in_len; (void)out_data; (void)out_cap;
  if (out_len) *out_len = 0u;
  return -1;
}
ix_u32 ixg_backend_nvidia_gsp_rm_heartbeat(void) {
  if (!g_ixg_driver_hw.mmio) return 0u;
  return ixg_driver_mmio_read(0x00110810u);
}
ix_u32 ixg_backend_nvidia_gsp_class(ix_u8 kind) {
  switch (g_nv.arch) {
    case NV_ARCH_TURING:
      return (kind == 0) ? 0x0000C46Fu : (kind == 1) ? 0x0000C497u : (kind == 2) ? 0x0000C4C0u : 0u;
    case NV_ARCH_AMPERE:
      return (kind == 0) ? 0x0000C56Fu : (kind == 1) ? 0x0000C597u : (kind == 2) ? 0x0000C5C0u : 0u;
    case NV_ARCH_ADA:
      return (kind == 0) ? 0x0000C86Fu : (kind == 1) ? 0x0000C897u : (kind == 2) ? 0x0000C8C0u : 0u;
    case NV_ARCH_BLACKWELL:
      return (kind == 0) ? 0x0000C96Fu : (kind == 1) ? 0x0000CD97u : (kind == 2) ? 0x0000CDC0u : (kind == 3) ? 0x0000C9B5u : 0u;
    default: return 0u;
  }
}

int ixg_backend_nvidia_tensor_dispatch(const nv_tensor_desc_t* desc) {
  ix_u64 desc_gpu;
  ix_u32 data[4];
  if (!desc) return -1;
  if (!g_nv.dev_info.has_tensor_cores) return -1;
  if (!g_nv.class_compute) return -1;
  nv_bind_objects();
  desc_gpu = nv_stage(desc, sizeof(nv_tensor_desc_t));
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    data[0] = (ix_u32)(desc_gpu >> 32);
    data[1] = (ix_u32)(desc_gpu & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_COMPUTE, NV_COMPUTE_TENSOR_DESC_ADDR_HI, data, 2u);
    nv_dma_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_TENSOR_LAUNCH, ++g_nv.sequence);
    nv_dma_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  data[0] = (ix_u32)(desc_gpu >> 32);
  data[1] = (ix_u32)(desc_gpu & 0xFFFFFFFFu);
  nv_push_method(NV_SUBCH_COMPUTE, NV_COMPUTE_TENSOR_DESC_ADDR_HI, data, 2u);
  nv_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_TENSOR_LAUNCH, ++g_nv.sequence);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

int ixg_backend_nvidia_rt_build_bvh(nv_rt_bvh_t* bvh) {
  if (!bvh) return -1;
  if (!g_nv.dev_info.has_rt_cores) return -1;
  if (bvh->primitive_count == 0u) return -1;
  {
    ix_u32 node_size = 64u;
    ix_u32 total_nodes = bvh->primitive_count * 2u;
    ix_u32 size_bytes;
    if (total_nodes > NV_RT_BVH_MAX_NODES) total_nodes = NV_RT_BVH_MAX_NODES;
    size_bytes = total_nodes * node_size;
    bvh->cpu_ptr = malloc(size_bytes);
    if (!bvh->cpu_ptr) return -1;
    bvh->pman_handle = 0;
    bvh->gpu_addr = (ix_u64)(uintptr_t)bvh->cpu_ptr;
    bvh->node_count = total_nodes;
    bvh->size_bytes = size_bytes;
  }
  return 0;
}

int ixg_backend_nvidia_rt_trace(const nv_rt_bvh_t* tlas, ix_u32 ray_count, ix_u64 rays_gpu, ix_u64 hits_gpu) {
  ix_u32 data[8];
  if (!tlas || ray_count == 0u || !rays_gpu || !hits_gpu) return -1;
  if (!g_nv.dev_info.has_rt_cores) return -1;
  if (!g_nv.class_compute) return -1;
  nv_bind_objects();
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    data[0] = (ix_u32)(tlas->gpu_addr >> 32);
    data[1] = (ix_u32)(tlas->gpu_addr & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_BVH_ADDR_HI, data, 2u);
    data[0] = (ix_u32)(rays_gpu >> 32);
    data[1] = (ix_u32)(rays_gpu & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_RAY_ADDR_HI, data, 2u);
    data[0] = (ix_u32)(hits_gpu >> 32);
    data[1] = (ix_u32)(hits_gpu & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_HIT_ADDR_HI, data, 2u);
    nv_dma_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_RAY_COUNT, ray_count);
    nv_dma_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_LAUNCH, ++g_nv.sequence);
    nv_dma_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  data[0] = (ix_u32)(tlas->gpu_addr >> 32);
  data[1] = (ix_u32)(tlas->gpu_addr & 0xFFFFFFFFu);
  nv_push_method(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_BVH_ADDR_HI, data, 2u);
  data[0] = (ix_u32)(rays_gpu >> 32);
  data[1] = (ix_u32)(rays_gpu & 0xFFFFFFFFu);
  nv_push_method(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_RAY_ADDR_HI, data, 2u);
  data[0] = (ix_u32)(hits_gpu >> 32);
  data[1] = (ix_u32)(hits_gpu & 0xFFFFFFFFu);
  nv_push_method(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_HIT_ADDR_HI, data, 2u);
  nv_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_RAY_COUNT, ray_count);
  nv_push_method1(NV_SUBCH_COMPUTE, NV_COMPUTE_RT_LAUNCH, ++g_nv.sequence);
  return g_ixg_driver_hw.submit_failed ? -1 : 0;
}

int ixg_backend_nvidia_create_vbo(const void* vertex_data, ix_u32 size_bytes, ix_u32 stride, ix_u32* out_vbo_id) {
  nv_vbo_slot_t* vbo;
  if (!vertex_data || size_bytes == 0u || !out_vbo_id || stride == 0u) return -1;
  if (g_nv.vbo_count >= NV_VBO_SLOT_MAX) return -1;
  vbo = &g_nv.vbos[g_nv.vbo_count];
  memset(vbo, 0, sizeof(*vbo));
  vbo->cpu_ptr = malloc(size_bytes);
  if (!vbo->cpu_ptr) return -1;
  memcpy(vbo->cpu_ptr, vertex_data, size_bytes);
  vbo->size_bytes = size_bytes; vbo->stride_bytes = stride;
  vbo->vertex_count = size_bytes / stride; vbo->bound = 1u;
  g_nv.vbo_count++;
  *out_vbo_id = g_nv.vbo_count;
  return 0;
}

void ixg_backend_nvidia_destroy_vbo(ix_u32 vbo_id) {
  nv_vbo_slot_t* vbo;
  if (vbo_id == 0u || vbo_id > g_nv.vbo_count) return;
  vbo = &g_nv.vbos[vbo_id - 1u];
  if (!vbo->bound) return;
  if (vbo->cpu_ptr) free(vbo->cpu_ptr);
  memset(vbo, 0, sizeof(*vbo));
}

int ixg_backend_nvidia_draw_vbo(ix_u32 vbo_id, ix_u32 first, ix_u32 count, ix_u32 topology) {
  nv_vbo_slot_t* vbo;
  ix_u32 data[4];
  if (vbo_id == 0u || vbo_id > g_nv.vbo_count) return -1;
  vbo = &g_nv.vbos[vbo_id - 1u];
  if (!vbo->bound || first + count > vbo->vertex_count) return -1;
  nv_bind_objects();
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    data[0] = (ix_u32)(vbo->gpu_addr >> 32);
    data[1] = (ix_u32)(vbo->gpu_addr & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_START_HIGH(0), data, 2u);
    data[0] = (ix_u32)((vbo->gpu_addr + vbo->size_bytes - 1u) >> 32);
    data[1] = (ix_u32)((vbo->gpu_addr + vbo->size_bytes - 1u) & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_LIMIT_HIGH(0), data, 2u);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BEGIN_GL, topology);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_FIRST, first);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_BUFFER_COUNT, count);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_VERTEX_END_GL, 0u);
    nv_dma_submit();
    return g_ixg_driver_hw.submit_failed ? -1 : 0;
  }
  return -1;
}

int ixg_backend_nvidia_nv3dl_demo(void) {
  ix_u32 data[4];
  if (g_nv.arch >= NV_ARCH_TURING) return -2;
  nv_bind_objects();
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    nv_dma_begin();
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_SET_ZETA_ENABLE, 0u);
    data[0] = 1u; data[1] = 0xCFu;
    nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_SET_RT_ENABLE, data, 2u);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_SET_RT_FORMAT, 0xCFu);
    data[0] = (ix_u32)(g_nv.fb_gpu_addr >> 32);
    data[1] = (ix_u32)(g_nv.fb_gpu_addr & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_SET_RT_ADDRESS_HIGH, data, 2u);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_SET_RT_HORIZ, g_nv.fb_width);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_SET_RT_VERT, g_nv.fb_height);
    nv_dma_submit();
    (void)ixg_driver_wait_idle(IXG_DRIVER_MMIO_POLL_SPINS);
    return 0;
  }
  return -1;
}

int ixg_backend_nvidia_hotplug_rescan(void) { return 0; }
int ixg_backend_nvidia_hotplug_attach(void) { return 0; }
int ixg_backend_nvidia_hotplug_detach(void) { return 0; }
nv_hotplug_state_t ixg_backend_nvidia_hotplug_state(void) { return NV_HOTPLUG_STATE_ABSENT; }

int ixg_backend_nvidia_get_device_info(nv_device_info_t* out_info) {
  if (!out_info) return -1;
  *out_info = g_nv.dev_info;
  return 0;
}

static nv_shader_slot_t* nv_find_shader(ix_u32 id) {
  ix_u32 i;
  for (i = 0u; i < NV_SHADER_SLOT_MAX; i++) if (g_nv.shaders[i].pman_handle == id && g_nv.shaders[i].size_bytes > 0) return &g_nv.shaders[i];
  return 0;
}

static void nv_seed_shader(ix_u32 id, ix_u32 tag) {
  ix_u32 code[16]; nv_shader_slot_t* slot; ix_u32 i;
  for (i = 0u; i < 16u; i++) code[i] = 0x49584153u ^ tag ^ (i * 0x9E3779B9u);
  slot = &g_nv.shaders[id - 1u];
  if (slot) {
    slot->pman_handle = id;
    slot->size_bytes = sizeof(code);
    slot->shader_type = 0u;
    slot->gpu_addr = nv_stage(code, sizeof(code));
  }
}

void ixg_backend_nvidia_post_init(void) {
  memset(&g_nv, 0, sizeof(g_nv));
  g_nv.initialized = 1u;
  nv_pick_classes();
  g_nv.fb_width = 1024u; g_nv.fb_height = 768u; g_nv.fb_pitch = 4096u; g_nv.fb_gpu_addr = 0u;
  if (!g_nv.legacy_path) {
    (void)nv_dma_pushbuf_init();
    (void)nv_dma_ib_init();
    (void)nv_dma_notifier_init();
    (void)nv_dma_semaphore_init();
  }
  nv_seed_shader(1u, 0x56532020u);
  nv_seed_shader(2u, 0x50532020u);
  nv_seed_shader(3u, 0x43532020u);
  {
    ix_u32 data[8];
    ixg_driver_program_dma_registers();
    ixg_driver_program_ring_registers();
    if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
      nv_dma_begin();
      nv_dma_push_method1(NV_SUBCH_M2MF, NV_MTHD_SET_OBJECT, g_nv.class_m2mf);
      nv_dma_push_method1(NV_SUBCH_2D, NV_MTHD_SET_OBJECT, NV_OBJ_2D);
      nv_dma_push_method1(NV_SUBCH_3D, NV_MTHD_SET_OBJECT, g_nv.class_3d);
      if (g_nv.class_compute) nv_dma_push_method1(NV_SUBCH_COMPUTE, NV_MTHD_SET_OBJECT, g_nv.class_compute);
      if (g_nv.class_ce) nv_dma_push_method1(NV_SUBCH_CE, NV_MTHD_SET_OBJECT, g_nv.class_ce);
      nv_dma_push_method1(NV_SUBCH_3D, NV_MTHD_NO_OPERATION, ++g_nv.sequence);
      nv_dma_submit();
    } else {
      nv_push_method1(NV_SUBCH_M2MF, NV_MTHD_SET_OBJECT, g_nv.legacy_path ? NV_OBJ_LEGACY_IMAGE_BLIT : g_nv.class_m2mf);
      nv_push_method1(NV_SUBCH_2D, NV_MTHD_SET_OBJECT, g_nv.legacy_path ? NV_OBJ_LEGACY_RECT : NV_OBJ_2D);
      nv_push_method1(NV_SUBCH_3D, NV_MTHD_SET_OBJECT, g_nv.class_3d);
      if (g_nv.class_compute) nv_push_method1(NV_SUBCH_COMPUTE, NV_MTHD_SET_OBJECT, g_nv.class_compute);
      data[0] = ++g_nv.sequence;
      nv_push_method1(NV_SUBCH_3D, NV_MTHD_NO_OPERATION, data[0]);
    }
    (void)ixg_driver_wait_idle(IXG_DRIVER_MMIO_POLL_SPINS);
  }
  if (g_ixg_driver_hw.submit_failed) return;
  nv_bind_objects();
}

void ixg_backend_nvidia_shutdown(void) {
  ix_u32 i;
  for (i = 0u; i < g_nv.vbo_count; i++) {
    if (g_nv.vbos[i].bound && g_nv.vbos[i].cpu_ptr) free(g_nv.vbos[i].cpu_ptr);
  }
  if (g_nv.pushbuf.cpu_ptr) free(g_nv.pushbuf.cpu_ptr);
  if (g_nv.ib_ring.cpu_ptr) free(g_nv.ib_ring.cpu_ptr);
  if (g_nv.notifier.cpu_ptr) free((void*)g_nv.notifier.cpu_ptr);
  if (g_nv.semaphore.cpu_ptr) free((void*)g_nv.semaphore.cpu_ptr);
  if (g_nv.gsp.cpu_ptr) free(g_nv.gsp.cpu_ptr);
  if (g_nv.gsp.booter_cpu_ptr) free(g_nv.gsp.booter_cpu_ptr);
  for (i = 0u; i < NV_FALCON_MAX_ENGINES; i++) {
    if (g_nv.falcons[i].fw_cpu_ptr) free(g_nv.falcons[i].fw_cpu_ptr);
  }
  memset(&g_nv, 0, sizeof(g_nv));
}

int ixg_backend_nvidia_upload_shader(const ix_u8* binary, ix_u32 binary_size, ix_u32* out_shader_id) {
  nv_shader_slot_t* slot;
  if (!binary || binary_size == 0u || !out_shader_id) return -1;
  if (g_nv.shader_count >= NV_SHADER_SLOT_MAX) return -1;
  slot = &g_nv.shaders[g_nv.shader_count];
  memset(slot, 0, sizeof(*slot));
  slot->pman_handle = g_nv.shader_count + 1;
  slot->size_bytes = binary_size;
  slot->shader_type = 0u;
  slot->gpu_addr = nv_stage(binary, binary_size);
  g_nv.shader_count++;
  *out_shader_id = g_nv.shader_count;
  return 0;
}

void ixg_backend_nvidia_delete_shader(ix_u32 shader_id) {
  nv_shader_slot_t* slot = nv_find_shader(shader_id);
  if (!slot) return;
  memset(slot, 0, sizeof(*slot));
}

static int nv_bind_shaders(const ixg_hw_shader_bind_t* bind) {
  static ixg_shader_program_t vs_prog, ps_prog;
  static ixg_sass_binary_t vs_bin, ps_bin;
  const ixg_shader_inst_t* src;
  ix_u32 inst_count, i;
  if (!bind || !bind->vs_bytecode || bind->vs_size == 0u || !bind->ps_bytecode || bind->ps_size == 0u)
    return IXG_BIND_ERROR;
  if ((bind->vs_size % (ix_u32)sizeof(ixg_shader_inst_t)) != 0u) return IXG_BIND_ERROR;
  if ((bind->ps_size % (ix_u32)sizeof(ixg_shader_inst_t)) != 0u) return IXG_BIND_ERROR;
  inst_count = bind->vs_size / (ix_u32)sizeof(ixg_shader_inst_t);
  if (inst_count == 0u || inst_count > IXG_SHADER_MAX_INSTRUCTIONS) return IXG_BIND_ERROR;
  memset(&vs_prog, 0, sizeof(vs_prog));
  vs_prog.stage_mask = (ix_u32)IXG_SHADER_STAGE_3D_VERTEX;
  vs_prog.instruction_count = inst_count;
  src = (const ixg_shader_inst_t*)bind->vs_bytecode;
  for (i = 0u; i < inst_count; i++) {
    vs_prog.instructions[i].opcode = src[i].op;
    vs_prog.instructions[i].dst = src[i].dst;
    vs_prog.instructions[i].src[0] = src[i].src0;
    vs_prog.instructions[i].src[1] = src[i].src1;
    vs_prog.instructions[i].src[2] = src[i].src2;
    vs_prog.instructions[i].write_mask = src[i].write_mask;
    vs_prog.instructions[i].flags = src[i].flags;
    vs_prog.instructions[i].imm = src[i].imm;
  }
  inst_count = bind->ps_size / (ix_u32)sizeof(ixg_shader_inst_t);
  if (inst_count == 0u || inst_count > IXG_SHADER_MAX_INSTRUCTIONS) return IXG_BIND_ERROR;
  memset(&ps_prog, 0, sizeof(ps_prog));
  ps_prog.stage_mask = (ix_u32)IXG_SHADER_STAGE_3D_PIXEL;
  ps_prog.instruction_count = inst_count;
  src = (const ixg_shader_inst_t*)bind->ps_bytecode;
  for (i = 0u; i < inst_count; i++) {
    ps_prog.instructions[i].opcode = src[i].op;
    ps_prog.instructions[i].dst = src[i].dst;
    ps_prog.instructions[i].src[0] = src[i].src0;
    ps_prog.instructions[i].src[1] = src[i].src1;
    ps_prog.instructions[i].src[2] = src[i].src2;
    ps_prog.instructions[i].write_mask = src[i].write_mask;
    ps_prog.instructions[i].flags = src[i].flags;
    ps_prog.instructions[i].imm = src[i].imm;
  }
  {
    ix_u32 sm = g_nv.dev_info.sm_version ? g_nv.dev_info.sm_version : 0x0075u;
    if (ixg_sass_encode(&vs_prog, 1u, sm, &vs_bin) != 0) return IXG_BIND_SOFTWARE_FALLBACK;
    if (ixg_sass_encode(&ps_prog, 2u, sm, &ps_bin) != 0) return IXG_BIND_SOFTWARE_FALLBACK;
  }
  nv_bind_objects();
  if (g_nv.pushbuf.cpu_ptr && !g_nv.legacy_path) {
    ix_u64 vs_gpu = nv_stage(vs_bin.bytes, vs_bin.byte_len);
    ix_u64 ps_gpu = nv_stage(ps_bin.bytes, ps_bin.byte_len);
    ix_u32 data[4];
    nv_dma_begin();
    data[0] = (ix_u32)(vs_gpu >> 32);
    data[1] = (ix_u32)(vs_gpu & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_SHADER_PROGRAM_ADDR_HI, data, 2u);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_BIND_SHADER_PROGRAM_TYPE, 0u);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_BIND_SHADER_PROGRAM, 1u);
    data[0] = (ix_u32)(ps_gpu >> 32);
    data[1] = (ix_u32)(ps_gpu & 0xFFFFFFFFu);
    nv_dma_push_method_n(NV_SUBCH_3D, NV_3D_SHADER_PROGRAM_ADDR_HI, data, 2u);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_BIND_SHADER_PROGRAM_TYPE, 1u);
    nv_dma_push_method1(NV_SUBCH_3D, NV_3D_BIND_SHADER_PROGRAM, 1u);
    nv_dma_submit();
  }
  ixg_driver_kick_ring(0x10DEu);
  return g_ixg_driver_hw.submit_failed ? IXG_BIND_ERROR : IXG_BIND_OK;
}

void ixg_backend_nvidia_init_ops(ixg_hw_ops_t* out_ops) {
  if (!out_ops) return;
  memset(out_ops, 0, sizeof(*out_ops));
  out_ops->hw_draw_rect = nv_draw_rect;
  out_ops->hw_bitblt = nv_bitblt;
  out_ops->hw_draw_rect_blend = nv_rect_blend;
  out_ops->hw_bitblt_alpha = nv_bitblt_alpha;
  out_ops->hw_flush = nv_flush;
  out_ops->hw_draw_triangles = nv_triangles;
  out_ops->hw_draw_triangles_textured = nv_triangles_tex;
  out_ops->hw_set_render_target = nv_set_render_target;
  out_ops->hw_readback_rect = nv_readback_rect;
  out_ops->hw_compute_dispatch = nv_compute;
  out_ops->hw_trace_rays = nv_trace;
  out_ops->hw_control_dispatch = nv_control;
  out_ops->hw_bind_shaders = nv_bind_shaders;
}