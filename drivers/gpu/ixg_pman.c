#include "ixg_pman.h"

int ixg_pman_fusion_get(ix_gpman_fusion_state_t* out_state) {
    if (out_state) {
        out_state->enabled = 0u;
        out_state->mode = IX_GP_FUSION_MODE_DISABLED;
        out_state->node_count = 0u;
        out_state->frame_pacing = 0u;
        out_state->compose_mask = 0ull;
        out_state->fusion_mask = 0ull;
        out_state->aggregate_score = 0u;
        out_state->unified_vram_mb = 0u;
    }
    return -1;
}

int ixg_pman_fusion_get_node(ix_u8 index, ix_gpman_fusion_node_t* out_node) {
    (void)index;
    if (out_node) {
        out_node->slot = 0u;
        out_node->type = IX_GP_DEV_CPU;
        out_node->vendor_id = 0u;
        out_node->device_id = 0u;
        out_node->headless = 1u;
        out_node->weight = 0u;
        out_node->vram_mb = 0u;
        out_node->score = 0u;
    }
    return -1;
}

int ixg_pman_get_state(ix_gpman_state_t* out_state) {
    if (out_state) {
        out_state->has_igpu = 0u;
        out_state->dgpu_count = 0u;
        out_state->mutant_count = 0u;
        out_state->reserved = 0u;
        out_state->compose_mask = 0ull;
    }
    return -1;
}

int ixg_pman_get_device(ix_u8 slot, ix_gpman_device_t* out_dev) {
    (void)slot;
    if (out_dev) {
        out_dev->type = IX_GP_DEV_CPU;
        out_dev->index = 0u;
        out_dev->vendor_id = 0u;
        out_dev->device_id = 0u;
        out_dev->headless = 1u;
        out_dev->reserved[0] = 0u;
        out_dev->reserved[1] = 0u;
        out_dev->reserved[2] = 0u;
    }
    return -1;
}
