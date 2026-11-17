#ifndef IXG_BACKEND_NVIDIA_H
#define IXG_BACKEND_NVIDIA_H

#include "ixg_driver_internal.h"

#define NV_MIN_DEVICE 0x0008u
#define NV_MAX_DEVICE 0x33FFu

#define NV_ARCH_NV1       0
#define NV_ARCH_NV3       1
#define NV_ARCH_TNT       2
#define NV_ARCH_CELSIUS   3
#define NV_ARCH_KELVIN    4
#define NV_ARCH_RANKINE   5
#define NV_ARCH_TESLA     6
#define NV_ARCH_FERMI     7
#define NV_ARCH_KEPLER    8
#define NV_ARCH_MAXWELL   9
#define NV_ARCH_PASCAL    10
#define NV_ARCH_VOLTA     11
#define NV_ARCH_TURING    12
#define NV_ARCH_AMPERE    13
#define NV_ARCH_ADA       14
#define NV_ARCH_BLACKWELL 15
#define NV_ARCH_COUNT     16

#define NV_DMA_PUSHBUF_CAP       (64u * 1024u)
#define NV_DMA_PUSHBUF_DWORDS    (NV_DMA_PUSHBUF_CAP / 4u)
#define NV_DMA_IB_ENTRIES        256u
#define NV_DMA_NOTIFIER_SLOTS    16u
#define NV_DMA_SEMAPHORE_SLOTS   32u
#define NV_SHADER_SLOT_MAX       64u
#define NV_VBO_SLOT_MAX          32u
#define NV_GSP_FW_MAX_SEGMENTS   8u
#define NV_GSP_FW_MAGIC          0x47535046u
#define NV_FALCON_MAX_ENGINES    4u
#define NV_TENSOR_MAX_DESCRIPTORS 16u
#define NV_RT_BVH_MAX_NODES      4096u
#define NV_RT_TLAS_MAX           4u
#define NV_RT_BLAS_MAX           32u

typedef enum {
  NV_GSP_STATE_OFF = 0, NV_GSP_STATE_LOADING = 1,
  NV_GSP_STATE_RUNNING = 2, NV_GSP_STATE_ERROR = 3
} nv_gsp_state_t;

typedef enum {
  NV_FALCON_SEC2 = 0, NV_FALCON_GSP = 1,
  NV_FALCON_NVDEC = 2, NV_FALCON_NVENC = 3
} nv_falcon_engine_t;

typedef enum {
  NV_TENSOR_FP16 = 0, NV_TENSOR_BF16 = 1, NV_TENSOR_TF32 = 2,
  NV_TENSOR_INT8 = 3, NV_TENSOR_FP8 = 4, NV_TENSOR_INT4 = 5
} nv_tensor_dtype_t;

typedef struct {
  ix_u32 put;
  ix_u32 get;
  ix_u32 fence_seq;
  ix_u32 capacity_dwords;
  ix_u32 used_dwords;
  ix_u8  state;
  ix_u8  overflow_count;
  ix_u16 reserved0;
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  ix_u32* cpu_ptr;
} nv_dma_pushbuf_t;

typedef struct {
  ix_u64 gpu_addr;
  ix_u32 length_dwords;
  ix_u32 flags;
} nv_dma_ib_entry_t;

typedef struct {
  ix_u32 count;
  ix_u32 put;
  ix_u32 get;
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  nv_dma_ib_entry_t* cpu_ptr;
} nv_dma_ib_ring_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  volatile ix_u32* cpu_ptr;
  ix_u32 slot_count;
} nv_dma_notifier_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  volatile ix_u32* cpu_ptr;
  ix_u32 slot_count;
} nv_dma_semaphore_t;

typedef struct {
  ix_u64 load_addr;
  ix_u32 size_bytes;
  ix_u32 flags;
} nv_gsp_fw_segment_t;

typedef struct {
  ix_u8  state;
  ix_u8  version_major;
  ix_u8  version_minor;
  ix_u8  segment_count;
  ix_u32 total_size;
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  void*  cpu_ptr;
  nv_gsp_fw_segment_t segments[NV_GSP_FW_MAX_SEGMENTS];
  ix_u32 boot_status;
  ix_u32 error_code;
  ix_u32 fw_format;
  ix_u32 booter_pman_handle;
  ix_u64 booter_gpu_addr;
  void*  booter_cpu_ptr;
  ix_u32 booter_size;
  ix_u32 booter_loaded;
  ix_u64 image_load_addr;
  ix_u32 image_size;
} nv_gsp_state_info_t;

typedef struct {
  ix_u8  engine;
  ix_u8  ready;
  ix_u16 version;
  ix_u32 mmio_base;
  ix_u32 fw_pman_handle;
  ix_u64 fw_gpu_addr;
  void*  fw_cpu_ptr;
  ix_u32 fw_size;
} nv_falcon_state_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  void*  cpu_ptr;
  ix_u32 size_bytes;
  ix_u32 shader_type;
  ix_u32 local_mem_size;
  ix_u32 shared_mem_size;
  ix_u32 register_count;
  ix_u32 barrier_count;
  ix_u16 sm_version;
  ix_u16 reserved0;
} nv_shader_slot_t;

typedef struct {
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  void*  cpu_ptr;
  ix_u32 size_bytes;
  ix_u32 stride_bytes;
  ix_u32 vertex_count;
  ix_u8  bound;
  ix_u8  reserved0[3];
} nv_vbo_slot_t;

typedef struct {
  ix_u8  dtype;
  ix_u8  reserved0[3];
  ix_u32 m, n, k;
  ix_u32 a_pman_handle; ix_u64 a_gpu_addr;
  ix_u32 b_pman_handle; ix_u64 b_gpu_addr;
  ix_u32 c_pman_handle; ix_u64 c_gpu_addr;
  ix_u32 d_pman_handle; ix_u64 d_gpu_addr;
  float alpha, beta;
} nv_tensor_desc_t;

typedef struct {
  ix_u32 node_count;
  ix_u32 pman_handle;
  ix_u64 gpu_addr;
  void*  cpu_ptr;
  ix_u32 size_bytes;
  ix_u32 build_flags;
  ix_u32 primitive_count;
  ix_u8  is_tlas;
  ix_u8  reserved0[3];
} nv_rt_bvh_t;

typedef struct {
  ix_u32 arch;
  ix_u32 chipset;
  ix_u32 vram_mb;
  ix_u8  gpc_count;
  ix_u8  tpc_per_gpc;
  ix_u8  sm_per_tpc;
  ix_u16 sm_version;
  ix_u8  has_tensor_cores;
  ix_u8  has_rt_cores;
  ix_u8  has_gsp;
  ix_u32 tensor_core_count;
  ix_u32 rt_core_count;
  ix_u32 total_sm_count;
  ix_u32 max_threads_per_sm;
  ix_u32 max_warps_per_sm;
  ix_u32 warp_size;
  ix_u32 shared_mem_per_sm;
  ix_u32 register_file_per_sm;
  ix_u32 l2_cache_bytes;
  ix_u8  nvlink_count;
  ix_u8  pcie_gen;
  ix_u8  pcie_lanes;
  ix_u8  fw_chip_family;
} nv_device_info_t;

void ixg_backend_nvidia_init_ops(ixg_hw_ops_t* out_ops);
void ixg_backend_nvidia_post_init(void);
void ixg_backend_nvidia_shutdown(void);
int  ixg_backend_nvidia_upload_shader(const ix_u8* binary, ix_u32 binary_size, ix_u32* out_shader_id);
void ixg_backend_nvidia_delete_shader(ix_u32 shader_id);
int  ixg_backend_nvidia_get_device_info(nv_device_info_t* out_info);
int  ixg_backend_nvidia_tensor_dispatch(const nv_tensor_desc_t* desc);
int  ixg_backend_nvidia_rt_build_bvh(nv_rt_bvh_t* bvh);
int  ixg_backend_nvidia_rt_trace(const nv_rt_bvh_t* tlas, ix_u32 ray_count, ix_u64 rays_gpu, ix_u64 hits_gpu);
int  ixg_backend_nvidia_gsp_load_firmware(const void* fw_data, ix_u32 fw_size);
int  ixg_backend_nvidia_gsp_set_booter(const void* booter_data, ix_u32 booter_size);
int  ixg_backend_nvidia_gsp_status(nv_gsp_state_info_t* out_state);
int  ixg_backend_nvidia_falcon_boot(ix_u8 engine, ix_u32 mmio_base, const void* fw_data, ix_u32 fw_size);
int  ixg_backend_nvidia_gsp_rm_init(void);
int  ixg_backend_nvidia_gsp_rm_call(ix_u32 function, const void* in_data, ix_u32 in_len, void* out_data, ix_u32 out_cap, ix_u32* out_len);
ix_u32 ixg_backend_nvidia_gsp_rm_heartbeat(void);
ix_u32 ixg_backend_nvidia_gsp_class(ix_u8 kind);
int  ixg_backend_nvidia_create_vbo(const void* vertex_data, ix_u32 size_bytes, ix_u32 stride, ix_u32* out_vbo_id);
void ixg_backend_nvidia_destroy_vbo(ix_u32 vbo_id);
int  ixg_backend_nvidia_draw_vbo(ix_u32 vbo_id, ix_u32 first, ix_u32 count, ix_u32 topology);
int  ixg_backend_nvidia_nv3dl_demo(void);

typedef enum {
  NV_HOTPLUG_STATE_ABSENT = 0, NV_HOTPLUG_STATE_DETECTED = 1,
  NV_HOTPLUG_STATE_LOADING = 2, NV_HOTPLUG_STATE_READY = 3,
  NV_HOTPLUG_STATE_FAILED = 4, NV_HOTPLUG_STATE_REMOVED = 5
} nv_hotplug_state_t;

int ixg_backend_nvidia_hotplug_rescan(void);
int ixg_backend_nvidia_hotplug_attach(void);
int ixg_backend_nvidia_hotplug_detach(void);
nv_hotplug_state_t ixg_backend_nvidia_hotplug_state(void);

#define NV_GSP_FW_FORMAT_UNKNOWN 0u
#define NV_GSP_FW_FORMAT_NVGF    1u
#define NV_GSP_FW_FORMAT_ELF     2u
#define NV_GSP_FW_FORMAT_RAW     3u

#endif