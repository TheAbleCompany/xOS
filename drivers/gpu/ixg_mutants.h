#ifndef IXG_MUTANTS_H
#define IXG_MUTANTS_H

#include "ixg_types.h"

#define IXG_MUTANT_FLAG_MINING (1u << 0)
#define IXG_MUTANT_FLAG_DATACENTER (1u << 1)
#define IXG_MUTANT_FLAG_HEADLESS (1u << 2)
#define IXG_MUTANT_FLAG_DISPLAYLESS_BOARD (1u << 3)
#define IXG_MUTANT_FLAG_TENSOR (1u << 4)
#define IXG_MUTANT_FLAG_NVLINK (1u << 5)
#define IXG_MUTANT_FLAG_MIG (1u << 6)
#define IXG_MUTANT_FLAG_GSP (1u << 7)
#define IXG_MUTANT_FLAG_NVENC_DISABLED (1u << 8)

#define IXG_MUTANT_CLASS_UNKNOWN 0u
#define IXG_MUTANT_CLASS_MINING 1u
#define IXG_MUTANT_CLASS_DATACENTER 2u
#define IXG_MUTANT_CLASS_HEADLESS_COMPUTE 3u

typedef struct {
    ix_u16 vendor_id;
    ix_u16 device_id;
    ix_u16 family;
    ix_u16 flags;
    ix_u8  class_id;
    ix_u8  headless;
    ix_u32 vram_mb;
    const char* name;
} ixg_mutant_profile_t;

typedef struct {
    ix_u32 total_seen;
    ix_u32 nvidia_seen;
    ix_u32 amd_seen;
    ix_u32 mining_seen;
    ix_u32 datacenter_seen;
    ix_u32 headless_seen;
    ix_u32 overflow_seen;
} ixg_mutant_registry_t;

int ixg_mutant_query(ix_u32 vendor_id, ix_u32 device_id, ixg_mutant_profile_t* out_profile);
ix_u32 ixg_mutant_estimate_vram_mb(ix_u32 vendor_id, ix_u32 device_id, ix_u8 headless);
void ixg_mutant_registry_reset(void);
void ixg_mutant_registry_get(ixg_mutant_registry_t* out_registry);
void ixg_mutant_init(ix_u32 vendor_id, ix_u32 device_id);

#endif