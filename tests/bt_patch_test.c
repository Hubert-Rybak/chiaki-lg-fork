#include "bt_patch.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t unpatched[] = {
    0xbb, 0x68, 0x03, 0x22, 0xda, 0x80, 0xbb, 0x69,
    0x1b, 0x79, 0xba, 0x68, 0xf9, 0x69, 0x18, 0x46,
};

int main(void)
{
    uint8_t image[96];
    memset(image, 0xa5, sizeof(image));

    ChiakiBtPatchMatch match = chiaki_bt_patch_find(image, sizeof(image));
    assert(match.state == CHIAKI_BT_PATCH_NOT_FOUND);
    assert(match.count == 0);

    memcpy(image + 17, unpatched, sizeof(unpatched));
    match = chiaki_bt_patch_find(image, sizeof(image));
    assert(match.state == CHIAKI_BT_PATCH_UNPATCHED);
    assert(match.offset == 17);
    assert(match.count == 1);

    image[17 + 2] = 2;
    match = chiaki_bt_patch_find(image, sizeof(image));
    assert(match.state == CHIAKI_BT_PATCH_ACTIVE);
    assert(match.offset == 17);
    assert(match.count == 1);

    memcpy(image + 57, unpatched, sizeof(unpatched));
    match = chiaki_bt_patch_find(image, sizeof(image));
    assert(match.state == CHIAKI_BT_PATCH_AMBIGUOUS);
    assert(match.count == 2);

    image[57 + 3] ^= 1;
    match = chiaki_bt_patch_find(image, sizeof(image));
    assert(match.state == CHIAKI_BT_PATCH_ACTIVE);
    assert(match.count == 1);

    puts("Bluetooth runtime patch signature tests passed");
    return 0;
}
