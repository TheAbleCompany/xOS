#ifndef IXG_BACKEND_INTEL_H
#define IXG_BACKEND_INTEL_H

#include "ixg_driver_internal.h"

#define INTEL_GEN_LEGACY   2
#define INTEL_GEN_I965     4
#define INTEL_GEN_SANDY    6
#define INTEL_GEN_HASWELL  7
#define INTEL_GEN_BROADWELL 8
#define INTEL_GEN_SKYLAKE  9
#define INTEL_GEN_XE_LP    12
#define INTEL_GEN_XE_HPG   13
#define INTEL_GEN_XE2      14
#define INTEL_GEN_XE3      15
#define INTEL_GEN_COUNT    16

#define INTEL_SHADER_SLOTS 128u
#define INTEL_VBO_SLOT_MAX 32u
#define INTEL_BB_SIZE      (64u * 1024u)

typedef struct {
  ix_u32 gen;
  ix_u32 chipset;
  ix_u32 vram_mb;
  ix_u8  slice_count;
  ix_u8  subslice_per_slice;
  ix_u8  eu_per_subslice;
  ix_u8  threads_per_eu;
  ix_u32 total_eu;
  ix_u32 total_threads;
  ix_u32 l3_cache_bytes;
  ix_u32 shared_local_mem;
  ix_u8  has_ray_tracing;
  ix_u8  has_xe_matrix;
  ix_u8  has_npu;
  ix_u8  is_dgpu;
  ix_u8  pcie_gen;
  ix_u8  pcie_lanes;
  ix_u16 npu_tops;
  ix_u32 xe_core_count;
} intel_device_info_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  void*  cpu_ptr;
  ix_u32 size_bytes;
  ix_u32 stride_bytes;
  ix_u32 vertex_count;
  ix_u8  bound;
  ix_u8  reserved0[3];
} intel_vbo_slot_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  ix_u32* cpu_ptr;
  ix_u32 capacity_dwords;
  ix_u32 used_dwords;
  ix_u8  state;
  ix_u8  reserved0[3];
} intel_batch_buffer_t;

void ixg_backend_intel_init_ops(ixg_hw_ops_t* out_ops);
void ixg_backend_intel_post_init(void);
void ixg_backend_intel_shutdown(void);
int  ixg_backend_intel_upload_shader(const ix_u8* binary, ix_u32 binary_size, ix_u32* out_shader_id);
void ixg_backend_intel_delete_shader(ix_u32 shader_id);
int  ixg_backend_intel_get_device_info(intel_device_info_t* out_info);
int  ixg_backend_intel_create_vbo(const void* vertex_data, ix_u32 size_bytes, ix_u32 stride, ix_u32* out_vbo_id);
void ixg_backend_intel_destroy_vbo(ix_u32 vbo_id);
int  ixg_backend_intel_draw_vbo(ix_u32 vbo_id, ix_u32 first, ix_u32 count, ix_u32 topology);
int  ixg_backend_intel_load_mcl_firmware(const void* fw_data, ix_u32 fw_size);

#endif