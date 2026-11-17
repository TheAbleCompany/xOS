#ifndef IXG_DRIVER_INTERNAL_H
#define IXG_DRIVER_INTERNAL_H

#include "ixg_types.h"
#include "ixg_platform.h"
#include "ixg_driver.h"

#define IXG_DRIVER_CMD_RING_CAP 4096u
#define IXG_DRIVER_CMD_RING_BYTES (IXG_DRIVER_CMD_RING_CAP * 32u)
#define IXG_DRIVER_MMIO_POLL_SPINS 10000u
#define IXG_DRIVER_SPIN_THEN_SLEEP_SPINS 4096u
#define IXG_DRIVER_TDR_TIMEOUT_SPINS 2000000u
#define IXG_DRIVER_MMIO_NO_PROGRESS_SPINS 2048u
#define IXG_DRIVER_MMIO_REKICK_SPINS 256u
#define IXG_DRIVER_RESCUE_RING_USED_HIGH (IXG_DRIVER_CMD_RING_CAP - 8u)
#define IXG_DRIVER_RESCUE_MAGIC 0x52475358u
#define IXG_DRIVER_ADAPTIVE_PROBE_MIN_SCORE 5u
#define IXG_DRIVER_ADAPTIVE_PROBE_MARGIN 1u

typedef enum {
  IXG_DRIVER_DMA_MODE_AUTO  = 0,
  IXG_DRIVER_DMA_MODE_GART  = 1,
  IXG_DRIVER_DMA_MODE_IOMMU = 2
} ixg_driver_dma_mode_t;

typedef struct {
  ix_u32 ring_put;
  ix_u32 ring_get;
  ix_u32 ring_base;
  ix_u32 ring_base_hi;
  ix_u32 ring_size;
  ix_u32 ring_tail;
  ix_u32 ring_head;
  ix_u32 doorbell;
  ix_u32 status;
  ix_u32 gart_base;
  ix_u32 gart_base_hi;
  ix_u32 gart_limit;
  ix_u32 iommu_ctrl;
  ix_u32 gart_pt_base;
  ix_u32 gart_pt_base_hi;
  ix_u32 iommu_tlb_inv;
} ixg_driver_mmio_regs_t;

typedef struct {
  ix_u32 op;
  ix_s32 p0;
  ix_s32 p1;
  ix_s32 p2;
  ix_s32 p3;
  ix_u32 p4;
  ix_u32 p5;
  ix_u32 p6;
  ix_u32 p7;
} ixg_driver_gpu_cmd_t;

typedef enum {
  IXG_DRIVER_VENDOR_PATH_GENERIC = 0,
  IXG_DRIVER_VENDOR_PATH_NVIDIA  = 1,
  IXG_DRIVER_VENDOR_PATH_AMD     = 2,
  IXG_DRIVER_VENDOR_PATH_INTEL   = 3,
  IXG_DRIVER_VENDOR_PATH_EXOTIC  = 4
} ixg_driver_vendor_path_t;

typedef enum {
  IXG_DRIVER_DBELL_OP     = 0,
  IXG_DRIVER_DBELL_TAIL   = 1,
  IXG_DRIVER_DBELL_NVIDIA = 2
} ixg_driver_doorbell_mode_t;

typedef struct {
  ix_u16 vendor_id;
  ix_u16 device_id_lo;
  ix_u16 device_id_hi;
  ix_u8  vendor_path;
  ix_u8  ring_tail_is_bytes;
  ix_u8  doorbell_mode;
  ix_u8  reserved0;
  ix_u32 reg_addend;
  ixg_driver_mmio_regs_t regs;
  const char* profile_name;
} ixg_driver_profile_t;

typedef struct {
  ix_u8  ready;
  ix_u8  bus;
  ix_u8  slot;
  ix_u8  func;
  ix_u16 vendor_id;
  ix_u16 device_id;
  ix_u64 bar0_base;
  ix_u64 bar0_size;
  volatile ix_u8* mmio;
  ixg_driver_mmio_regs_t regs;
  ix_u32 cmd_tail;
  ix_u32 cmd_seq;
  ix_u32 cmd_head_cache;
  ix_u32 ring_mask;
  ix_u8  submit_failed;
  ix_u8  vendor_path;
  ix_u8  ring_tail_is_bytes;
  ix_u8  doorbell_mode;
  ix_u8  dma_mode;
  ix_u8  gart_enabled;
  ix_u8  iommu_enabled;
  ix_u8  render_hw_only;
  ix_u8  render_submit_mirror_3d;
  ix_u8  reserved_cfg0;
  ix_u16 dma_mask_bits;
  ix_u32 reg_addend;
  ix_u32 dma_aperture_mb;
  ix_u32 dma_pte_count;
  ix_u8  dma_table_ready;
  ix_u8  dma_sandbox_only;
  ix_u8  reserved_cfg1;
  ix_u8  reserved_cfg2;
  ix_u8  adaptive_profile;
  ix_u8  adaptive_probe_score;
  ix_u16 adaptive_reserved0;
  ix_u64 dma_aperture_base;
  const char* profile_name;
  ix_u8  compute_only;
  ix_u8  device_created;
  ix_u8  initialized;
  ix_u8  _pad_a;
  void*  exotic_profile;
  ix_u64 mmio_base;
  ix_u32 mmio_size;
  ix_u8  pci_valid;
  ix_u8  pci_seg;
  ix_u8  pci_bus;
  ix_u8  pci_dev;
  ix_u8  has_compute;
  ix_u8  has_tensor;
  ix_u8  has_rt;
  ix_u8  has_dma;
} ixg_driver_hw_state_t;

typedef ixg_driver_hw_state_t ixg_driver_state_t;

extern ixg_driver_hw_state_t g_ixg_driver_hw;
extern ixg_driver_gpu_cmd_t g_ixg_driver_cmd_ring[IXG_DRIVER_CMD_RING_CAP];
extern ix_u64 g_ixg_driver_dma_ptes[];
extern ix_u8* g_ixg_driver_dma_sandbox_ptr;
extern ix_u32 g_ixg_driver_dma_sandbox_bytes;

void ixg_driver_mmio_write(ix_u32 offset, ix_u32 value);
ix_u32 ixg_driver_mmio_read(ix_u32 offset);
void ixg_driver_mmio_flush_posted(void);
void ixg_driver_kick_ring(ix_u32 op);
ix_u32 ixg_driver_ring_used(void);
void ixg_driver_program_dma_registers(void);
void ixg_driver_dma_init_sandbox_pool(void);
ix_u64 ixg_driver_dma_stage_upload(const void* src, ix_u32 size_bytes);
void ixg_driver_store_rescue_blob(ix_u32 reason, ix_u32 frame_id, ix_u32 load_hint);
void ixg_driver_program_ring_registers(void);
ix_u8 ixg_driver_wait_idle(ix_u32 spins);
void ixg_driver_detect_pcie_caps(ix_u8 bus, ix_u8 slot, ix_u8 func, ix_u8* out_gen, ix_u8* out_lanes);

#endif