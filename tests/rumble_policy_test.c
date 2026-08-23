#include "rumble_policy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    RumblePolicy policy;
    uint16_t left = 0;
    uint16_t right = 0;

    rumble_policy_reset(&policy);
    assert(rumble_policy_prepare(&policy, true, 0, 0x1234, 0xabcd,
                                 &left, &right));
    assert(left == 0x1200 && right == 0xab00);

    /* Low-byte-only changes cannot alter SDL's DualSense output report. */
    assert(!rumble_policy_prepare(&policy, true, 20, 0x12ff, 0xab01,
                                  &left, &right));

    /* Keep the newest pending value and release it at the 50 ms boundary. */
    assert(!rumble_policy_prepare(&policy, true, 49, 0x3400, 0xcd00,
                                  &left, &right));
    assert(left == 0x3400 && right == 0xcd00);
    assert(rumble_policy_prepare(&policy, true, 50, 0x3400, 0xcd00,
                                 &left, &right));

    /* A stop must never wait behind the limiter. */
    assert(rumble_policy_prepare(&policy, true, 51, 0, 0, &left, &right));
    assert(left == 0 && right == 0);
    assert(!rumble_policy_prepare(&policy, true, 52, 0x5600, 0x7800,
                                  &left, &right));
    assert(rumble_policy_prepare(&policy, true, 101, 0x5600, 0x7800,
                                 &left, &right));

    /* Other SDL controller backends remain immediate and full precision. */
    rumble_policy_reset(&policy);
    assert(rumble_policy_prepare(&policy, false, 200, 0x1234, 0xabcd,
                                 &left, &right));
    assert(left == 0x1234 && right == 0xabcd);
    assert(rumble_policy_prepare(&policy, false, 201, 0x1235, 0xabce,
                                 &left, &right));

    puts("controller rumble policy tests passed");
    return 0;
}
