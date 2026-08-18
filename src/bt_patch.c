#include "bt_patch.h"

#include <string.h>

#define REPORT_TYPE_OFFSET 2

static const uint8_t send_data_signature[] = {
    0xbb, 0x68, 0x00, 0x22, 0xda, 0x80, 0xbb, 0x69,
    0x1b, 0x79, 0xba, 0x68, 0xf9, 0x69, 0x18, 0x46,
};

static int signature_matches(const uint8_t *candidate)
{
    return candidate[REPORT_TYPE_OFFSET] >= 2 &&
           candidate[REPORT_TYPE_OFFSET] <= 3 &&
           memcmp(candidate, send_data_signature, REPORT_TYPE_OFFSET) == 0 &&
           memcmp(candidate + REPORT_TYPE_OFFSET + 1,
                  send_data_signature + REPORT_TYPE_OFFSET + 1,
                  sizeof(send_data_signature) - REPORT_TYPE_OFFSET - 1) == 0;
}
ChiakiBtPatchMatch chiaki_bt_patch_find(const uint8_t *data, size_t size)
{
    ChiakiBtPatchMatch match = { CHIAKI_BT_PATCH_NOT_FOUND, 0, 0 };
    if (!data || size < sizeof(send_data_signature))
        return match;

    for (size_t i = 0; i <= size - sizeof(send_data_signature); ++i) {
        if (!signature_matches(data + i))
            continue;
        match.offset = i;
        ++match.count;
        match.state = data[i + REPORT_TYPE_OFFSET] == 2
                          ? CHIAKI_BT_PATCH_ACTIVE
                          : CHIAKI_BT_PATCH_UNPATCHED;
    }

    if (match.count > 1)
        match.state = CHIAKI_BT_PATCH_AMBIGUOUS;
    return match;
}
