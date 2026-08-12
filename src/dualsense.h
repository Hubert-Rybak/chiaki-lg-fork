#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DUALSENSE_REPORT_LEN 78
#define DUALSENSE_EFFECT_LEN 11

typedef struct {
    uint8_t lightbar_set;
    uint8_t lightbar[3];
    uint8_t player_leds_set;
    uint8_t player_leds;
    uint8_t right_trigger[DUALSENSE_EFFECT_LEN];
    uint8_t left_trigger[DUALSENSE_EFFECT_LEN];
    uint8_t triggers_owned;
    uint8_t intensity_set;
    uint8_t intensity;
} DualSenseOutputState;

typedef struct DualSenseFeedback DualSenseFeedback;

/* Pure protocol helpers, exposed so the exact on-device report can be tested. */
uint32_t dualsense_crc32(const uint8_t *data, size_t size);
void dualsense_build_report(uint8_t sequence,
                            const DualSenseOutputState *state,
                            uint8_t report[DUALSENSE_REPORT_LEN]);
bool dualsense_build_payload(const char *address,
                             const uint8_t report[DUALSENSE_REPORT_LEN],
                             char *payload, size_t payload_size);

bool dualsense_find_bluetooth_address(char *address, size_t address_size);
bool dualsense_hid_playstation_bound(void);

/* Bluetooth feedback. Returns NULL when this TV/controller cannot support it. */
DualSenseFeedback *dualsense_feedback_new(void);
void dualsense_feedback_set_lightbar(DualSenseFeedback *feedback,
                                     uint8_t red, uint8_t green, uint8_t blue);
void dualsense_feedback_set_player_leds(DualSenseFeedback *feedback, uint8_t bits);
void dualsense_feedback_set_trigger_effects(DualSenseFeedback *feedback,
                                            uint8_t type_left, const uint8_t left[10],
                                            uint8_t type_right, const uint8_t right[10]);
void dualsense_feedback_set_intensity(DualSenseFeedback *feedback, uint8_t intensity);
void dualsense_feedback_release(DualSenseFeedback *feedback);
void dualsense_feedback_free(DualSenseFeedback *feedback);

