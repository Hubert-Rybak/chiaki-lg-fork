#include "controller_features.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void assert_near(float actual, float expected, float epsilon)
{
    assert(fabsf(actual - expected) <= epsilon);
}

static ChiakiControllerTouch *find_touch(ChiakiControllerState *state, int8_t id)
{
    for (size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i) {
        if (state->touches[i].id == id)
            return &state->touches[i];
    }
    return NULL;
}

static void test_touch_lifecycle(void)
{
    ControllerFeatures features;
    ChiakiControllerState state;
    controller_features_reset(&features, &state);

    assert(controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_DOWN, 0, 7, 0.25f, 0.5f));
    ChiakiControllerTouch *first = find_touch(&state, 0);
    assert(first);
    assert(first->x == 480);
    assert(first->y == 539);
    assert(state.touch_id_next == 1);

    /* A duplicate DOWN updates in place without consuming another touch ID. */
    assert(controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_DOWN, 0, 7, 0.5f, 0.25f));
    first = find_touch(&state, 0);
    assert(first && first->x == 960 && first->y == 269);
    assert(state.touch_id_next == 1);

    assert(controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_DOWN, 0, 9, 2.0f, -1.0f));
    ChiakiControllerTouch *second = find_touch(&state, 1);
    assert(second && second->x == CONTROLLER_TOUCHPAD_MAX_X);
    assert(second->y == 0);

    /* Chiaki and DualSense both allow at most two simultaneous contacts. */
    assert(!controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_DOWN, 0, 11, 0.0f, 0.0f));
    assert(!controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_MOTION, 1, 99, 0.0f, 0.0f));
    assert(!controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_UP, 1, 99, 0.0f, 0.0f));

    assert(controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_MOTION, 0, 7, 1.0f, 1.0f));
    first = find_touch(&state, 0);
    assert(first && first->x == CONTROLLER_TOUCHPAD_MAX_X);
    assert(first->y == CONTROLLER_TOUCHPAD_MAX_Y);

    assert(controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_UP, 0, 7, 1.0f, 1.0f));
    assert(!find_touch(&state, 0));
    assert(controller_features_handle_touch(
        &features, &state, CONTROLLER_TOUCH_DOWN, 1, 12, 0.1f, 0.2f));
    assert(find_touch(&state, 2));
}

static void test_motion_mapping_and_reset(void)
{
    ControllerFeatures features;
    ChiakiControllerState state;
    controller_features_reset(&features, &state);

    assert_near(state.accel_x, 0.0f, 0.0001f);
    assert_near(state.accel_y, 1.0f, 0.0001f);
    assert_near(state.orient_w, 1.0f, 0.0001f);

    assert(controller_features_handle_sensor(
        &features, &state, CONTROLLER_SENSOR_ACCEL,
        0.0f, CONTROLLER_STANDARD_GRAVITY, 0.0f, 1000));
    assert_near(state.accel_x, 0.0f, 0.0001f);
    assert_near(state.accel_y, 1.0f, 0.0001f);
    assert_near(state.accel_z, 0.0f, 0.0001f);

    for (uint32_t i = 2; i <= 20; ++i) {
        assert(controller_features_handle_sensor(
            &features, &state, CONTROLLER_SENSOR_GYRO,
            1.0f, 2.0f, 3.0f, i * 10000u));
    }
    assert_near(state.gyro_x, 1.0f, 0.0001f);
    assert_near(state.gyro_y, 2.0f, 0.0001f);
    assert_near(state.gyro_z, 3.0f, 0.0001f);
    float quaternion_norm = sqrtf(
        state.orient_x * state.orient_x + state.orient_y * state.orient_y +
        state.orient_z * state.orient_z + state.orient_w * state.orient_w);
    assert_near(quaternion_norm, 1.0f, 0.001f);
    assert(fabsf(state.orient_x) + fabsf(state.orient_y) +
           fabsf(state.orient_z) > 0.001f);

    /* Motion reset treats the current physical pose as level (+1 g on Y). */
    assert(controller_features_handle_sensor(
        &features, &state, CONTROLLER_SENSOR_ACCEL,
        CONTROLLER_STANDARD_GRAVITY, CONTROLLER_STANDARD_GRAVITY, 0.0f,
        210000));
    assert_near(state.accel_x, 1.0f, 0.0001f);
    controller_features_reset_motion(&features, &state);
    assert_near(state.accel_x, 0.0f, 0.0001f);
    assert_near(state.accel_y, 1.0f, 0.0001f);
    assert_near(state.accel_z, 0.0f, 0.0001f);

    controller_features_reset(&features, &state);
    for (size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i)
        assert(state.touches[i].id == -1);
    assert_near(state.gyro_x, 0.0f, 0.0001f);
    assert_near(state.accel_y, 1.0f, 0.0001f);
    assert_near(state.orient_w, 1.0f, 0.0001f);
}

int main(void)
{
    test_touch_lifecycle();
    test_motion_mapping_and_reset();
    puts("controller touch and motion tests passed");
    return 0;
}
