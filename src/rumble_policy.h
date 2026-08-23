#ifndef CHIAKI_WEBOS_RUMBLE_POLICY_H
#define CHIAKI_WEBOS_RUMBLE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define DUALSENSE_RUMBLE_SEND_INTERVAL_MS 50u

typedef struct rumble_policy_t {
    uint16_t last_left;
    uint16_t last_right;
    uint64_t last_send_ms;
    bool valid;
} RumblePolicy;

void rumble_policy_reset(RumblePolicy *policy);

/*
 * Coalesce DualSense HIDAPI output to its eight-bit motor resolution.  The
 * first value and a stop are sent immediately; non-zero changes are bounded to
 * one write per interval.  Other controller backends retain full precision and
 * are not rate-limited.
 */
bool rumble_policy_prepare(RumblePolicy *policy, bool is_dualsense,
                           uint64_t now_ms, uint16_t desired_left,
                           uint16_t desired_right, uint16_t *send_left,
                           uint16_t *send_right);

#endif
