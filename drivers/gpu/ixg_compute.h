#ifndef IXG_COMPUTE_H
#define IXG_COMPUTE_H

#include "ixg_types.h"
#include "ixg_driver.h"

#define IXG_COMPUTE_MAX_DEVICES 16u
#define IXG_COMPUTE_MAX_KERNEL_ARGS 8u

#define IXG_COMPUTE_FEAT_SHADER      (1u << 0)
#define IXG_COMPUTE_FEAT_FP16         (1u << 1)
#define IXG_COMPUTE_FEAT_INT8         (1u << 2)
#define IXG_COMPUTE_FEAT_SOFT_FALLBACK (1u << 3)
#define IXG_COMPUTE_FEAT_OPENCL       (1u << 4)
#define IXG_COMPUTE_FEAT_IXSL         (1u << 5)
#define IXG_COMPUTE_FEAT_CUDA         (1u << 6)
#define IXG_COMPUTE_FEAT_BF16         (1u << 7)
#define IXG_COMPUTE_FEAT_RAW_GPU_ASM  (1u << 8)
#define IXG_COMPUTE_FEAT_TENSOR       (1u << 9)
#define IXG_COMPUTE_FEAT_RAY_TRACING  (1u << 10)
#define IXG_COMPUTE_FEAT_HEADLESS     (1u << 11)
#define IXG_COMPUTE_FEAT_AMD_MATRIX   (1u << 12)
#define IXG_COMPUTE_FEAT_INTEL_NPU    (1u << 13)

#define IXG_COMPUTE_KERNEL_COPY_U32   1u
#define IXG_COMPUTE_KERNEL_ADD_U32     2u
#define IXG_COMPUTE_KERNEL_SAXPY_F32  3u
#define IXG_COMPUTE_KERNEL_MATMUL4X4_F32 4u
#define IXG_COMPUTE_KERNEL_IXSL_HANDLE 5u
#define IXG_COMPUTE_KERNEL_TENSOR_GEMM 6u

#define IXG_COMPUTE_SOURCE_IXSL      0u
#define IXG_COMPUTE_SOURCE_OPENCL_C  1u
#define IXG_COMPUTE_SOURCE_HLSL      2u
#define IXG_COMPUTE_SOURCE_GLSL      3u

typedef struct {
  ix_u64 device_mask;
  ix_u32 kernel;
  ix_u32 x, y, z;
  void*  dst;
  void*  src0;
  void*  src1;
  void*  src2;
  float  scalar0;
  float  scalar1;
  ix_u32 dst_stride;
} ixg_compute_dispatch_desc_t;

typedef struct {
  ix_u8  slot;
  ix_u8  type;
  ix_u8  backend;
  ix_u16 vendor_id;
  ix_u16 device_id;
  ix_u32 feature_flags;
  ix_u32 score;
  ix_u32 vram_mb;
  ix_u32 tensor_tops;
  ix_u32 npu_tops;
} ixg_compute_device_caps_t;

typedef struct {
  ix_u8  ready;
  ix_u8  device_count;
  ix_u16 reserved0;
  ix_u32 fusion_enabled;
  ix_u32 fusion_mode;
  ix_u64 fusion_mask;
  ix_u64 active_mask;
  ix_u32 aggregate_score;
  ix_u32 unified_vram_mb;
} ixg_compute_state_t;

typedef struct {
  ix_u32 thread_count[3];
  ix_u32 group_count[3];
  ix_u32 shared_mem;
  ix_u32 flags;
  void*  args;
  ix_u32 arg_count;
} ixg_compute_kernel_t;

typedef struct {
  ix_u32 size;
  const void* data;
} ixg_compute_kernel_arg_t;

typedef struct {
  ix_u32 source_language;
  ix_u32 source_len;
  const char* source;
  const char* entry;
  ix_u32 flags;
} ixg_compute_program_desc_t;

typedef struct {
  ix_u32 m;
  ix_u32 n;
  ix_u32 k;
  const void* a;
  const void* b;
  const void* c;
  void* d;
  float alpha;
  float beta;
  ix_u64 device_mask;
} ixg_compute_tensor_gemm_desc_t;

typedef struct {
  float ax, ay, az;
  float bx, by, bz;
  float cx, cy, cz;
} ixg_rt_triangle_t;

typedef struct {
  float ox, oy, oz;
  float dx, dy, dz;
  float t_min, t_max;
} ixg_rt_ray_t;

typedef struct {
  ix_u8  hit;
  ix_u8  reserved0[3];
  ix_u32 triangle_index;
  float t, u, v;
} ixg_rt_hit_t;

typedef struct {
  const ixg_rt_triangle_t* triangles;
  ix_u32 triangle_count;
  const ixg_rt_ray_t* rays;
  ixg_rt_hit_t* hits;
  ix_u32 ray_count;
  ix_u64 device_mask;
} ixg_rt_trace_desc_t;

typedef struct {
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
  ix_u32 left, right;
  ix_u32 tri_offset, tri_count;
} ixg_rt_bvh_node_t;

typedef struct {
  const ixg_rt_triangle_t* triangles;
  ix_u32 tri_count;
  ixg_rt_bvh_node_t* nodes;
  ix_u32 node_count;
  ix_u32 pman_handle;
} ixg_rt_bvh_t;

typedef struct {
  ixg_rt_bvh_t* bvh;
  const ixg_rt_ray_t* rays;
  ixg_rt_hit_t* hits;
  ix_u32 ray_count;
} ixg_rt_bvh_trace_desc_t;

typedef struct {
  ix_u32 handle;
  ix_u64 gpu_addr;
  void* cpu_ptr;
  ix_u32 size_bytes;
  ix_u8  tier;
} ixg_compute_buffer_t;

#define IXG_COMPUTE_BUF_DEVICE       (1u << 0)
#define IXG_COMPUTE_BUF_HOST_VISIBLE (1u << 1)
#define IXG_COMPUTE_BUF_HOST_COHERENT (1u << 2)

#define IXG_RT_BVH_MAX_DEPTH 32u

void ixg_compute_init(void);
void ixg_compute_refresh(void);
int ixg_compute_get_state(ixg_compute_state_t* out_state);
int ixg_compute_get_device_caps(ix_u8 index, ixg_compute_device_caps_t* out_caps);
int ixg_compute_dispatch(const ixg_compute_dispatch_desc_t* desc);
int ixg_compute_trace_rays(const ixg_rt_trace_desc_t* desc);
int ixg_compute_program_create(const ixg_compute_program_desc_t* desc, ix_u32* out_program);
int ixg_compute_program_release(ix_u32 program);
int ixg_compute_program_get_ixsl(ix_u32 program, char* out_text, ix_u32 out_cap, ix_u32* out_len);
int ixg_compute_kernel_create(ix_u32 program, const char* entry, ix_u32* out_kernel);
int ixg_compute_kernel_release(ix_u32 kernel);
int ixg_compute_kernel_set_arg(ix_u32 kernel, ix_u32 index, ix_u32 size, const void* data);
int ixg_compute_kernel_dispatch(ix_u32 kernel, ix_u64 device_mask, ix_u32 x, ix_u32 y, ix_u32 z);
int ixg_compute_launch_kernel(ix_u32 device, const ixg_compute_kernel_t* kernel);
int ixg_compute_tensor_gemm(const ixg_compute_tensor_gemm_desc_t* desc);
int ixg_compute_bvh_build(const ixg_rt_triangle_t* tris, ix_u32 tri_count, ixg_rt_bvh_t* out_bvh);
void ixg_compute_bvh_destroy(ixg_rt_bvh_t* bvh);
int ixg_compute_bvh_trace(const ixg_rt_bvh_trace_desc_t* desc);
int ixg_compute_buffer_create(ix_u32 size_bytes, ix_u32 flags, ixg_compute_buffer_t* out_buf);
void ixg_compute_buffer_destroy(ixg_compute_buffer_t* buf);
int ixg_compute_dispatch_fill_u32(ix_u64 device_mask, void* dst, ix_u32 value, ix_u32 count);
int ixg_compute_dispatch_reduce_sum_f32(ix_u64 device_mask, const float* src, ix_u32 count, float* out_result);
int ixg_compute_dispatch_softmax_f32(ix_u64 device_mask, const float* src, float* dst, ix_u32 count);

#endif