#include "rumble_policy.h"

#include <stddef.h>

void rumble_policy_reset(RumblePolicy *policy)
{
    if (!policy)
        return;

    policy->last_left = 0;
    policy->last_right = 0;
    policy->last_send_ms = 0;
    policy->valid = false;
}

bool rumble_policy_prepare(RumblePolicy *policy, bool is_dualsense,
                           uint64_t now_ms, uint16_t desired_left,
                           uint16_t desired_right, uint16_t *send_left,
                           uint16_t *send_right)
{
    if (!policy || !send_left || !send_right)
        return false;

    uint16_t left = desired_left;
    uint16_t right = desired_right;
    if (is_dualsense) {
        left &= UINT16_C(0xff00);
        right &= UINT16_C(0xff00);
    }

    *send_left = left;
    *send_right = right;

    if (policy->valid && left == policy->last_left &&
        right == policy->last_right)
        return false;

    bool stopping = left == 0 && right == 0;
    if (is_dualsense && !stopping && policy->valid &&
        now_ms - policy->last_send_ms < DUALSENSE_RUMBLE_SEND_INTERVAL_MS)
        return false;

    policy->last_left = left;
    policy->last_right = right;
    policy->last_send_ms = now_ms;
    policy->valid = true;
    return true;
}
