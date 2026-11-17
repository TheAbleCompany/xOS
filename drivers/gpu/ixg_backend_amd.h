#ifndef IXG_BACKEND_AMD_H
#define IXG_BACKEND_AMD_H

#include "ixg_driver_internal.h"

#define AMD_FAMILY_LEGACY    0
#define AMD_FAMILY_R600      1
#define AMD_FAMILY_EVERGREEN 2
#define AMD_FAMILY_GCN       3
#define AMD_FAMILY_POLARIS   4
#define AMD_FAMILY_VEGA      5
#define AMD_FAMILY_RDNA1     6
#define AMD_FAMILY_RDNA2     7
#define AMD_FAMILY_RDNA3     8
#define AMD_FAMILY_RDNA4     9
#define AMD_FAMILY_CDNA      10
#define AMD_FAMILY_CDNA2     11
#define AMD_FAMILY_CDNA3     12
#define AMD_FAMILY_COUNT     13

#define AMD_SHADER_SLOTS     128u
#define AMD_VBO_SLOT_MAX     32u
#define AMD_DMA_IB_ENTRIES   128u
#define AMD_DMA_IB_SIZE      (64u * 1024u)

typedef struct {
  ix_u32 family;
  ix_u32 chipset;
  ix_u32 vram_mb;
  ix_u8  se_count;
  ix_u8  sh_per_se;
  ix_u8  cu_per_sh;
  ix_u8  simd_per_cu;
  ix_u32 total_cu;
  ix_u32 total_wgp;
  ix_u32 wave_size;
  ix_u32 max_waves_per_cu;
  ix_u32 lds_per_cu;
  ix_u32 sgpr_per_cu;
  ix_u32 vgpr_per_cu;
  ix_u32 l2_cache_bytes;
  ix_u32 infinity_cache_mb;
  ix_u8  has_matrix_cores;
  ix_u8  has_rt_accelerator;
  ix_u8  has_dp_support;
  ix_u8  pcie_gen;
  ix_u8  pcie_lanes;
  ix_u8  is_cdna;
  ix_u8  is_apu;
  ix_u8  reserved0;
} amd_device_info_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  void*  cpu_ptr;
  ix_u32 size_bytes;
  ix_u32 stride_bytes;
  ix_u32 vertex_count;
  ix_u8  bound;
  ix_u8  reserved0[3];
} amd_vbo_slot_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  ix_u32* cpu_ptr;
  ix_u32 capacity_dwords;
  ix_u32 used_dwords;
  ix_u8  state;
  ix_u8  reserved0[3];
} amd_dma_ib_t;

void ixg_backend_amd_init_ops(ixg_hw_ops_t* out_ops);
void ixg_backend_amd_post_init(void);
void ixg_backend_amd_shutdown(void);
int  ixg_backend_amd_upload_shader(const ix_u8* binary, ix_u32 binary_size, ix_u32* out_shader_id);
void ixg_backend_amd_delete_shader(ix_u32 shader_id);
int  ixg_backend_amd_get_device_info(amd_device_info_t* out_info);
int  ixg_backend_amd_create_vbo(const void* vertex_data, ix_u32 size_bytes, ix_u32 stride, ix_u32* out_vbo_id);
void ixg_backend_amd_destroy_vbo(ix_u32 vbo_id);
int  ixg_backend_amd_draw_vbo(ix_u32 vbo_id, ix_u32 first, ix_u32 count, ix_u32 topology);
int  ixg_backend_amd_load_pm_firmware(const void* fw_data, ix_u32 fw_size);

#endif