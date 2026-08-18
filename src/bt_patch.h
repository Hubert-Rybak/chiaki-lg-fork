#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum ChiakiBtPatchState {
    CHIAKI_BT_PATCH_NOT_FOUND = 0,
    CHIAKI_BT_PATCH_UNPATCHED,
    CHIAKI_BT_PATCH_ACTIVE,
    CHIAKI_BT_PATCH_AMBIGUOUS,
} ChiakiBtPatchState;

typedef struct ChiakiBtPatchMatch {
    ChiakiBtPatchState state;
    size_t offset;
    size_t count;
} ChiakiBtPatchMatch;

/*
 * Locate LG's old BlueDroid send_data report-type instruction. The signature
 * deliberately covers the surrounding data-flow instructions and accepts only
 * the original immediate (feature = 3) or our corrected immediate (output = 2).
 */
ChiakiBtPatchMatch chiaki_bt_patch_find(const uint8_t *data, size_t size);
