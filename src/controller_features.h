#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <chiaki/controller.h>
#include <chiaki/orientation.h>

#define CONTROLLER_TOUCHPAD_MAX_X 1920u
#define CONTROLLER_TOUCHPAD_MAX_Y 1079u
#define CONTROLLER_STANDARD_GRAVITY 9.80665f

typedef enum ControllerTouchPhase {
    CONTROLLER_TOUCH_DOWN,
    CONTROLLER_TOUCH_MOTION,
    CONTROLLER_TOUCH_UP,
} ControllerTouchPhase;

typedef enum ControllerSensorType {
    CONTROLLER_SENSOR_ACCEL,
    CONTROLLER_SENSOR_GYRO,
} ControllerSensorType;

typedef struct ControllerTouchBinding {
    bool active;
    int32_t touchpad;
    int32_t finger;
    int8_t chiaki_id;
} ControllerTouchBinding;

typedef struct ControllerFeatures {
    ChiakiOrientationTracker orientation_tracker;
    ChiakiAccelNewZero accel_zero;
    ChiakiAccelNewZero real_accel;
    uint32_t last_motion_timestamp_us;
    ControllerTouchBinding touches[CHIAKI_CONTROLLER_TOUCHES_MAX];
} ControllerFeatures;

/* Reset touch identity, motion calibration, orientation, and controller state. */
void controller_features_reset(ControllerFeatures *features,
                               ChiakiControllerState *state);

/* Apply normalized SDL touch coordinates to Chiaki's two-touch state. */
bool controller_features_handle_touch(ControllerFeatures *features,
                                      ChiakiControllerState *state,
                                      ControllerTouchPhase phase,
                                      int32_t touchpad, int32_t finger,
                                      float x, float y);

/* Apply SDL sensor axes. Accelerometer values are m/s^2; gyro is rad/s. */
bool controller_features_handle_sensor(ControllerFeatures *features,
                                       ChiakiControllerState *state,
                                       ControllerSensorType sensor,
                                       float x, float y, float z,
                                       uint32_t timestamp_us);

/* Recenter motion controls using the most recent physical acceleration. */
void controller_features_reset_motion(ControllerFeatures *features,
                                      ChiakiControllerState *state);
