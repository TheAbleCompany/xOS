#ifndef IXG_PMAN_H
#define IXG_PMAN_H

#include "ixg_types.h"

#define IXG_PMAN_MAX_DGPU 16
#define IXG_PMAN_MAX_MUTANT 16
#define IXGP_FUSION_MAX_NODES 8u

#define IXGP_MASK_CPU     1ull
#define IXGP_MASK_IGPU    2ull
#define IXGP_MASK_DGPU(i) (4ull << (i))
#define IXGP_MASK_MUTANT(i) ((ix_u64)1 << (32 + (i)))

typedef enum {
  IX_GP_DEV_CPU    = 0,
  IX_GP_DEV_IGPU   = 1,
  IX_GP_DEV_DGPU   = 2,
  IX_GP_DEV_MUTANT = 3
} ix_gp_dev_type_t;

typedef enum {
  IX_GP_FUSION_MODE_DISABLED  = 0,
  IX_GP_FUSION_MODE_COMPUTE   = 1,
  IX_GP_FUSION_MODE_SPLIT_FRAME = 2,
  IX_GP_FUSION_MODE_ALT_FRAME = 3,
  IX_GP_FUSION_MODE_MIXED     = 4
} ix_gp_fusion_mode_t;

typedef struct {
  ix_u8 type;
  ix_u8 index;
  ix_u16 vendor_id;
  ix_u16 device_id;
  ix_u8 headless;
  ix_u8 reserved[3];
} ix_gpman_device_t;

typedef struct {
  ix_u8 slot;
  ix_u8 type;
  ix_u16 vendor_id;
  ix_u16 device_id;
  ix_u8 headless;
  ix_u8 weight;
  ix_u32 vram_mb;
  ix_u32 score;
} ix_gpman_fusion_node_t;

typedef struct {
  ix_u8 enabled;
  ix_u8 mode;
  ix_u8 node_count;
  ix_u8 frame_pacing;
  ix_u64 compose_mask;
  ix_u64 fusion_mask;
  ix_u32 aggregate_score;
  ix_u32 unified_vram_mb;
} ix_gpman_fusion_state_t;

typedef struct {
  ix_u8 has_igpu;
  ix_u8 dgpu_count;
  ix_u8 mutant_count;
  ix_u8 reserved;
  ix_u64 compose_mask;
} ix_gpman_state_t;

int ixg_pman_fusion_get(ix_gpman_fusion_state_t* out_state);
int ixg_pman_fusion_get_node(ix_u8 index, ix_gpman_fusion_node_t* out_node);
int ixg_pman_get_state(ix_gpman_state_t* out_state);
int ixg_pman_get_device(ix_u8 slot, ix_gpman_device_t* out_dev);

#endif
