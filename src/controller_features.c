#include "controller_features.h"

#include <stddef.h>
#include <string.h>

static uint16_t scale_touch_coordinate(float value, uint16_t maximum)
{
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return maximum;
    return (uint16_t)(value * (float)maximum);
}

static int find_touch(const ControllerFeatures *features,
                      int32_t touchpad, int32_t finger)
{
    for (size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i) {
        const ControllerTouchBinding *binding = &features->touches[i];
        if (binding->active && binding->touchpad == touchpad &&
            binding->finger == finger)
            return (int)i;
    }
    return -1;
}

static int find_free_touch(const ControllerFeatures *features)
{
    for (size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i) {
        if (!features->touches[i].active)
            return (int)i;
    }
    return -1;
}

void controller_features_reset(ControllerFeatures *features,
                               ChiakiControllerState *state)
{
    chiaki_controller_state_set_idle(state);
    chiaki_orientation_tracker_init(&features->orientation_tracker);
    chiaki_accel_new_zero_set_inactive(&features->accel_zero, false);
    chiaki_accel_new_zero_set_inactive(&features->real_accel, true);
    features->last_motion_timestamp_us = 0;
    memset(features->touches, 0, sizeof(features->touches));
    for (size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i)
        features->touches[i].chiaki_id = -1;
}

bool controller_features_handle_touch(ControllerFeatures *features,
                                      ChiakiControllerState *state,
                                      ControllerTouchPhase phase,
                                      int32_t touchpad, int32_t finger,
                                      float x, float y)
{
    int slot = find_touch(features, touchpad, finger);
    uint16_t scaled_x = scale_touch_coordinate(x, CONTROLLER_TOUCHPAD_MAX_X);
    uint16_t scaled_y = scale_touch_coordinate(y, CONTROLLER_TOUCHPAD_MAX_Y);

    switch (phase) {
    case CONTROLLER_TOUCH_DOWN:
        if (slot >= 0) {
            chiaki_controller_state_set_touch_pos(
                state, (uint8_t)features->touches[slot].chiaki_id,
                scaled_x, scaled_y);
            return true;
        }
        slot = find_free_touch(features);
        if (slot < 0)
            return false;
        {
            int8_t id = chiaki_controller_state_start_touch(
                state, scaled_x, scaled_y);
            if (id < 0)
                return false;
            features->touches[slot].active = true;
            features->touches[slot].touchpad = touchpad;
            features->touches[slot].finger = finger;
            features->touches[slot].chiaki_id = id;
        }
        return true;

    case CONTROLLER_TOUCH_MOTION:
        if (slot < 0)
            return false;
        chiaki_controller_state_set_touch_pos(
            state, (uint8_t)features->touches[slot].chiaki_id,
            scaled_x, scaled_y);
        return true;

    case CONTROLLER_TOUCH_UP:
        if (slot < 0)
            return false;
        chiaki_controller_state_stop_touch(
            state, (uint8_t)features->touches[slot].chiaki_id);
        features->touches[slot].active = false;
        features->touches[slot].chiaki_id = -1;
        return true;
    }

    return false;
}

bool controller_features_handle_sensor(ControllerFeatures *features,
                                       ChiakiControllerState *state,
                                       ControllerSensorType sensor,
                                       float x, float y, float z,
                                       uint32_t timestamp_us)
{
    switch (sensor) {
    case CONTROLLER_SENSOR_ACCEL:
        x /= CONTROLLER_STANDARD_GRAVITY;
        y /= CONTROLLER_STANDARD_GRAVITY;
        z /= CONTROLLER_STANDARD_GRAVITY;
        chiaki_accel_new_zero_set_active(
            &features->real_accel, x, y, z, true);
        chiaki_orientation_tracker_update(
            &features->orientation_tracker,
            state->gyro_x, state->gyro_y, state->gyro_z,
            x, y, z, &features->accel_zero, false, timestamp_us);
        break;

    case CONTROLLER_SENSOR_GYRO:
        chiaki_orientation_tracker_update(
            &features->orientation_tracker,
            x, y, z, state->accel_x, state->accel_y, state->accel_z,
            &features->accel_zero, true, timestamp_us);
        break;

    default:
        return false;
    }

    features->last_motion_timestamp_us = timestamp_us;
    chiaki_orientation_tracker_apply_to_controller_state(
        &features->orientation_tracker, state);
    return true;
}

void controller_features_reset_motion(ControllerFeatures *features,
                                      ChiakiControllerState *state)
{
    chiaki_accel_new_zero_set_active(
        &features->accel_zero,
        features->real_accel.accel_x,
        features->real_accel.accel_y,
        features->real_accel.accel_z,
        false);
    chiaki_orientation_tracker_init(&features->orientation_tracker);
    chiaki_orientation_tracker_update(
        &features->orientation_tracker,
        state->gyro_x, state->gyro_y, state->gyro_z,
        features->real_accel.accel_x,
        features->real_accel.accel_y,
        features->real_accel.accel_z,
        &features->accel_zero, false,
        features->last_motion_timestamp_us);
    chiaki_orientation_tracker_apply_to_controller_state(
        &features->orientation_tracker, state);
}
