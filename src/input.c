#include "input.h"
#include "app_log.h"
#include "controller_features.h"
#include "dualsense.h"
#include "rumble_policy.h"
#include "webos_keys.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHORD_WINDOW_MS 100u
#define HAPTIC_HOLD_MS   50u
#define HAPTIC_RUMBLE_MIN_STRENGTH 100u
/* Bound continuous 1 kHz DualSense motion/touch updates to Chiaki's cadence. */
#define CONTINUOUS_INPUT_SEND_INTERVAL_MS 8u
#define CONTROLLER_POWER_POLL_INTERVAL_MS 10000u

typedef enum {
    CHORD_NONE = 0,
    CHORD_BACK,
    CHORD_START,
} PendingChordButton;

typedef enum {
    CONTROLLER_TRANSPORT_UNKNOWN = 0,
    CONTROLLER_TRANSPORT_USB,
    CONTROLLER_TRANSPORT_BLUETOOTH,
} ControllerTransport;

struct InputContext {
    pthread_mutex_t mutex;
    ChiakiControllerState state;
    ControllerFeatures features;
    ChiakiSession *session;

    SDL_GameController *controller;
    SDL_JoystickID instance_id;
    bool is_dualsense;
    DualSenseFeedback *dualsense_feedback;
    bool accel_sensor_enabled;
    bool gyro_sensor_enabled;
    bool rumble_available;
    bool dualsense_bluetooth_enhanced;
    bool touch_event_logged;
    bool accel_event_logged;
    bool gyro_event_logged;
    bool motion_reset_pending;
    bool motion_dirty;
    uint64_t last_motion_send_ms;
    bool touch_motion_dirty;
    uint64_t last_touch_motion_send_ms;

    bool back_held;
    bool start_held;
    bool chord_active;
    PendingChordButton chord_pending;
    uint32_t chord_started_ms;

    uint16_t base_rumble_left;
    uint16_t base_rumble_right;
    uint16_t haptic_rumble_left;
    uint16_t haptic_rumble_right;
    uint64_t haptic_until_ms;
    RumblePolicy rumble_policy;
    bool rumble_error_logged;
    bool rumble_write_logged;
    bool session_rumble_nonzero_logged;
    bool haptic_frame_logged;
    bool haptic_nonzero_logged;
    uint64_t session_rumble_events;
    uint64_t haptic_frames;
    uint64_t last_power_poll_ms;

    float rumble_multiplier;
    bool haptics_enabled;
    bool triggers_enabled;
    int haptic_intensity;
    int trigger_intensity;

    bool led_pending;
    uint8_t led[3];
    bool player_index_pending;
    int player_index;
};

static void reset_controller_state_locked(InputContext *ctx)
{
    controller_features_reset(&ctx->features, &ctx->state);
    ctx->back_held = false;
    ctx->start_held = false;
    ctx->chord_active = false;
    ctx->chord_pending = CHORD_NONE;
    ctx->motion_reset_pending = false;
    ctx->motion_dirty = false;
    ctx->last_motion_send_ms = 0;
    ctx->touch_motion_dirty = false;
    ctx->last_touch_motion_send_ms = 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool name_contains(const char *name, const char *needle)
{
    if (!name || !needle) return false;
    size_t n = strlen(needle);
    for (const char *p = name; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

static bool controller_is_dualsense(SDL_GameController *controller)
{
    if (!controller) return false;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_PS5)
        return true;
#endif
    const char *name = SDL_GameControllerName(controller);
    if (name_contains(name, "dualsense") || name_contains(name, "ps5 controller"))
        return true;
#if SDL_VERSION_ATLEAST(2, 0, 6)
    Uint16 vendor = SDL_GameControllerGetVendor(controller);
    Uint16 product = SDL_GameControllerGetProduct(controller);
    if (vendor == 0x054c && (product == 0x0ce6 || product == 0x0df2))
        return true;
#endif
    return false;
}

static const char *power_level_name(SDL_JoystickPowerLevel power)
{
    switch (power) {
    case SDL_JOYSTICK_POWER_EMPTY:   return "empty";
    case SDL_JOYSTICK_POWER_LOW:     return "low";
    case SDL_JOYSTICK_POWER_MEDIUM:  return "medium";
    case SDL_JOYSTICK_POWER_FULL:    return "full";
    case SDL_JOYSTICK_POWER_WIRED:   return "wired";
    case SDL_JOYSTICK_POWER_MAX:     return "max";
    case SDL_JOYSTICK_POWER_UNKNOWN:
    default:                         return "unknown";
    }
}

static const char *controller_transport_name(ControllerTransport transport)
{
    switch (transport) {
    case CONTROLLER_TRANSPORT_USB:       return "usb";
    case CONTROLLER_TRANSPORT_BLUETOOTH: return "bluetooth";
    case CONTROLLER_TRANSPORT_UNKNOWN:
    default:                             return "unknown";
    }
}

/* SDL-webOS does not expose HIDAPI's Bluetooth flag through its public API.
 * Resolve it from Linux hidraw/input sysfs so the stability policy can still
 * allow full USB feedback without writing to a Bluetooth DualSense. */
static ControllerTransport controller_transport_from_path(const char *path)
{
    if (!path)
        return CONTROLLER_TRANSPORT_UNKNOWN;

    const char *node = strrchr(path, '/');
    node = node ? node + 1 : path;
    const char *digits = NULL;
    bool hidraw = false;
    if (strncmp(node, "hidraw", 6) == 0) {
        digits = node + 6;
        hidraw = true;
    } else if (strncmp(node, "event", 5) == 0) {
        digits = node + 5;
    } else if (strncmp(node, "js", 2) == 0) {
        digits = node + 2;
    }
    if (!digits || *digits == '\0')
        return CONTROLLER_TRANSPORT_UNKNOWN;
    for (const char *p = digits; *p; ++p) {
        if (*p < '0' || *p > '9')
            return CONTROLLER_TRANSPORT_UNKNOWN;
    }

    char uevent_path[PATH_MAX];
    int written = hidraw
        ? snprintf(uevent_path, sizeof(uevent_path),
                   "/sys/class/hidraw/%s/device/uevent", node)
        : snprintf(uevent_path, sizeof(uevent_path),
                   "/sys/class/input/%s/device/id/bustype", node);
    if (written <= 0 || (size_t)written >= sizeof(uevent_path))
        return CONTROLLER_TRANSPORT_UNKNOWN;

    FILE *f = fopen(uevent_path, "r");
    if (!f)
        return CONTROLLER_TRANSPORT_UNKNOWN;

    ControllerTransport transport = CONTROLLER_TRANSPORT_UNKNOWN;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        unsigned bus = 0;
        int parsed = hidraw
            ? sscanf(line, "HID_ID=%x:", &bus)
            : sscanf(line, "%x", &bus);
        if (parsed != 1)
            continue;
        if (bus == 0x0003)
            transport = CONTROLLER_TRANSPORT_USB;
        else if (bus == 0x0005)
            transport = CONTROLLER_TRANSPORT_BLUETOOTH;
        break;
    }
    fclose(f);
    return transport;
}

static ControllerTransport controller_transport_with_power_fallback(
    const char *path, SDL_JoystickPowerLevel power)
{
    ControllerTransport transport = controller_transport_from_path(path);
    if (transport != CONTROLLER_TRANSPORT_UNKNOWN)
        return transport;
    if (power == SDL_JOYSTICK_POWER_WIRED)
        return CONTROLLER_TRANSPORT_USB;
    if (power >= SDL_JOYSTICK_POWER_EMPTY &&
        power <= SDL_JOYSTICK_POWER_FULL) {
        return CONTROLLER_TRANSPORT_BLUETOOTH;
    }
    return CONTROLLER_TRANSPORT_UNKNOWN;
}

static bool log_controller_candidate(int device_index)
{
    const char *name = SDL_JoystickNameForIndex(device_index);
    const char *path = SDL_JoystickPathForIndex(device_index);
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(device_index);
    char guid_string[33] = {0};
    SDL_JoystickGetGUIDString(guid, guid_string, sizeof(guid_string));
    Uint16 vendor = SDL_JoystickGetDeviceVendor(device_index);
    Uint16 product = SDL_JoystickGetDeviceProduct(device_index);
    bool mapped = SDL_IsGameController(device_index) == SDL_TRUE;

    app_log_always(
        "[INPUT] SDL joystick index=%d name=\"%s\" path=%s guid=%s "
        "vid=%04x pid=%04x mapped=%s\n",
        device_index,
        name ? name : "unknown",
        path ? path : "unknown",
        guid_string[0] ? guid_string : "unknown",
        (unsigned)vendor,
        (unsigned)product,
        mapped ? "yes" : "no");

    if (mapped) {
        char *mapping = SDL_GameControllerMappingForDeviceIndex(device_index);
        if (mapping) {
            app_log("[INPUT] SDL mapping index=%d: %s\n", device_index, mapping);
            SDL_free(mapping);
        }
    }
    return mapped;
}

static void send_state(InputContext *ctx)
{
    ChiakiControllerState state;
    ChiakiSession *session;
    pthread_mutex_lock(&ctx->mutex);
    state = ctx->state;
    session = ctx->session;
    pthread_mutex_unlock(&ctx->mutex);
    if (session)
        chiaki_session_set_controller_state(session, &state);
}

static uint8_t normalize_trigger(Sint16 value)
{
    if (value <= 0) return 0;
    return (uint8_t)(((uint32_t)(uint16_t)value * 255u) / 32767u);
}

static uint32_t controller_button_bit(Uint8 button)
{
    switch ((SDL_GameControllerButton)button) {
    case SDL_CONTROLLER_BUTTON_A:             return CHIAKI_CONTROLLER_BUTTON_CROSS;
    case SDL_CONTROLLER_BUTTON_B:             return CHIAKI_CONTROLLER_BUTTON_MOON;
    case SDL_CONTROLLER_BUTTON_X:             return CHIAKI_CONTROLLER_BUTTON_BOX;
    case SDL_CONTROLLER_BUTTON_Y:             return CHIAKI_CONTROLLER_BUTTON_PYRAMID;
    case SDL_CONTROLLER_BUTTON_GUIDE:         return CHIAKI_CONTROLLER_BUTTON_PS;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return CHIAKI_CONTROLLER_BUTTON_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return CHIAKI_CONTROLLER_BUTTON_R3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return CHIAKI_CONTROLLER_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return CHIAKI_CONTROLLER_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_BUTTON_MISC1:
    case SDL_CONTROLLER_BUTTON_TOUCHPAD:      return CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
#endif
    default:                                  return 0;
    }
}

static void close_controller(InputContext *ctx)
{
    if (!ctx->controller) return;

    const char *name = SDL_GameControllerName(ctx->controller);
    SDL_JoystickPowerLevel power = SDL_JoystickCurrentPowerLevel(
        SDL_GameControllerGetJoystick(ctx->controller));
    app_log_always("[INPUT] Closing controller: %s "
                   "(instance=%d, power=%s, uptime_ms=%llu)\n",
                   name ? name : "unknown", (int)ctx->instance_id,
                   power_level_name(power),
                   (unsigned long long)monotonic_ms());
    if (ctx->rumble_available)
        (void)SDL_GameControllerRumble(ctx->controller, 0, 0, 0);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (ctx->accel_sensor_enabled)
        (void)SDL_GameControllerSetSensorEnabled(
            ctx->controller, SDL_SENSOR_ACCEL, SDL_FALSE);
    if (ctx->gyro_sensor_enabled)
        (void)SDL_GameControllerSetSensorEnabled(
            ctx->controller, SDL_SENSOR_GYRO, SDL_FALSE);
#endif

    pthread_mutex_lock(&ctx->mutex);
    reset_controller_state_locked(ctx);
    ctx->is_dualsense = false;
    ctx->accel_sensor_enabled = false;
    ctx->gyro_sensor_enabled = false;
    ctx->rumble_available = false;
    ctx->touch_event_logged = false;
    ctx->accel_event_logged = false;
    ctx->gyro_event_logged = false;
    rumble_policy_reset(&ctx->rumble_policy);
    ctx->rumble_error_logged = false;
    ctx->rumble_write_logged = false;
    ctx->last_power_poll_ms = 0;
    DualSenseFeedback *feedback = ctx->dualsense_feedback;
    ctx->dualsense_feedback = NULL;
    pthread_mutex_unlock(&ctx->mutex);
    send_state(ctx);

    SDL_GameControllerClose(ctx->controller);
    ctx->controller = NULL;
    ctx->instance_id = -1;
    dualsense_feedback_free(feedback);
}

static bool open_controller(InputContext *ctx, int device_index)
{
    if (ctx->controller || device_index < 0)
        return false;
    if (!log_controller_candidate(device_index)) {
        app_log_always(
            "[INPUT] Joystick index=%d has no SDL GameController mapping; skipped\n",
            device_index);
        return false;
    }

    char device_path[PATH_MAX] = {0};
    const char *candidate_path = SDL_JoystickPathForIndex(device_index);
    if (candidate_path)
        snprintf(device_path, sizeof(device_path), "%s", candidate_path);

    SDL_GameController *controller = SDL_GameControllerOpen(device_index);
    if (!controller) {
        app_log("[INPUT] SDL_GameControllerOpen(%d) failed: %s\n",
                device_index, SDL_GetError());
        return false;
    }

    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickID instance = SDL_JoystickInstanceID(joystick);
    if (instance < 0) {
        app_log("[INPUT] Could not get controller instance: %s\n", SDL_GetError());
        SDL_GameControllerClose(controller);
        return false;
    }

    ctx->controller = controller;
    ctx->instance_id = instance;
    bool is_dualsense = controller_is_dualsense(controller);
    SDL_JoystickPowerLevel power = SDL_JoystickCurrentPowerLevel(joystick);
    ControllerTransport transport = controller_transport_with_power_fallback(
        device_path[0] ? device_path : NULL, power);
    bool dualsense_advanced_allowed = !is_dualsense ||
        ctx->dualsense_bluetooth_enhanced ||
        transport == CONTROLLER_TRANSPORT_USB;
    /*
     * Keep SDL HIDAPI as the sole live DualSense output writer.  The former
     * Luna path forked a helper for every advanced-feedback report after the
     * GPU/video stack was active and interleaved a second Bluetooth framing
     * format with SDL rumble.  It remains quarantined until an in-process or
     * pre-spawned transport is available.
     */
    DualSenseFeedback *feedback = NULL;

    int touchpad_count = 0;
    int touch_finger_count = 0;
    bool has_accel = false;
    bool has_gyro = false;
    bool rumble_available = false;
    bool accel_enabled = false;
    bool gyro_enabled = false;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    touchpad_count = SDL_GameControllerGetNumTouchpads(controller);
    if (touchpad_count > 0)
        touch_finger_count = SDL_GameControllerGetNumTouchpadFingers(
            controller, 0);
    has_accel = SDL_GameControllerHasSensor(
        controller, SDL_SENSOR_ACCEL) == SDL_TRUE;
    has_gyro = SDL_GameControllerHasSensor(
        controller, SDL_SENSOR_GYRO) == SDL_TRUE;
    if (has_accel && dualsense_advanced_allowed) {
        if (SDL_GameControllerSetSensorEnabled(
                controller, SDL_SENSOR_ACCEL, SDL_TRUE) == 0) {
            accel_enabled = true;
        } else {
            app_log_always("[INPUT] Could not enable accelerometer: %s\n",
                           SDL_GetError());
        }
    }
    if (has_gyro && dualsense_advanced_allowed) {
        if (SDL_GameControllerSetSensorEnabled(
                controller, SDL_SENSOR_GYRO, SDL_TRUE) == 0) {
            gyro_enabled = true;
        } else {
            app_log_always("[INPUT] Could not enable gyroscope: %s\n",
                           SDL_GetError());
        }
    }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
    bool sdl_rumble_available =
        SDL_GameControllerHasRumble(controller) == SDL_TRUE;
#else
    /* SDL_GameControllerRumble predates the capability query. Preserve the
     * previous behavior for non-pinned host builds while the Bluetooth policy
     * still blocks unsafe DualSense output. */
    bool sdl_rumble_available = true;
#endif
    rumble_available = sdl_rumble_available && dualsense_advanced_allowed;
    bool advanced_capabilities_observed = touchpad_count > 0 || has_accel ||
                                          has_gyro || sdl_rumble_available;

    pthread_mutex_lock(&ctx->mutex);
    ctx->is_dualsense = is_dualsense;
    ctx->dualsense_feedback = feedback;
    ctx->accel_sensor_enabled = accel_enabled;
    ctx->gyro_sensor_enabled = gyro_enabled;
    ctx->rumble_available = rumble_available;
    ctx->touch_event_logged = false;
    ctx->accel_event_logged = false;
    ctx->gyro_event_logged = false;
    rumble_policy_reset(&ctx->rumble_policy);
    ctx->rumble_error_logged = false;
    ctx->rumble_write_logged = false;
    ctx->last_power_poll_ms = 0;
    int combined_intensity =
        (ctx->trigger_intensity < 0 ? 0xf0 : ctx->trigger_intensity) |
        (ctx->haptic_intensity < 0 ? 0x0f : ctx->haptic_intensity);
    pthread_mutex_unlock(&ctx->mutex);

    if (feedback)
        dualsense_feedback_set_intensity(feedback, (uint8_t)combined_intensity);

    if (is_dualsense)
        app_log_always("[DUALSENSE] Advanced Bluetooth feedback disabled; "
                       "SDL is the sole controller-output path\n");

    if (is_dualsense) {
        app_log_always(
            "[INPUT] DualSense transport=%s advanced_capabilities=%s "
            "bluetooth_policy=%s app_output=%s\n",
            controller_transport_name(transport),
            advanced_capabilities_observed ? "yes" : "no",
            ctx->dualsense_bluetooth_enhanced ? "enabled" : "basic",
            dualsense_advanced_allowed ? "allowed" : "blocked");
        if (!ctx->dualsense_bluetooth_enhanced &&
            transport == CONTROLLER_TRANSPORT_BLUETOOTH &&
            advanced_capabilities_observed) {
            app_log_always(
                "[INPUT] DualSense arrived in one-way enhanced Bluetooth mode; "
                "fully power it off, then reconnect it for stable basic mode\n");
        } else if (!ctx->dualsense_bluetooth_enhanced &&
                   transport == CONTROLLER_TRANSPORT_UNKNOWN &&
                   advanced_capabilities_observed) {
            app_log_always(
                "[INPUT] DualSense transport is unknown; app output is blocked "
                "by the basic-mode safety policy\n");
        }
    }

    app_log_always("[INPUT] Controller opened: %s (instance=%d, DualSense=%s)\n",
                   SDL_GameControllerName(controller) ? SDL_GameControllerName(controller) : "unknown",
                   (int)instance,
                   is_dualsense ? "yes" : "no");
    app_log_always(
        "[INPUT] Controller features: touchpads=%d fingers=%d "
        "accel=%s/%s gyro=%s/%s rumble=%s firmware=0x%04x power=%s\n",
        touchpad_count, touch_finger_count,
        has_accel ? "available" : "unavailable",
        accel_enabled ? "enabled" : "disabled",
        has_gyro ? "available" : "unavailable",
        gyro_enabled ? "enabled" : "disabled",
        sdl_rumble_available
            ? (rumble_available ? "available" : "blocked-by-policy")
            : "unavailable",
#if SDL_VERSION_ATLEAST(2, 24, 0)
        (unsigned)SDL_GameControllerGetFirmwareVersion(controller),
#else
        0u,
#endif
        power_level_name(power));
    return true;
}

static void open_first_controller(InputContext *ctx)
{
    if (ctx->controller) return;
    int count = SDL_NumJoysticks();
    if (count < 0) {
        app_log_always("[INPUT] SDL joystick enumeration failed: %s\n",
                       SDL_GetError());
        return;
    }
    app_log_always("[INPUT] SDL enumerated %d joystick device(s)\n", count);
    for (int i = 0; i < count; ++i) {
        if (open_controller(ctx, i))
            return;
    }
    app_log_always(
        "[INPUT] No SDL GameController found; Magic Remote remains available\n");
}

static void handle_chord_button(InputContext *ctx, Uint8 button, bool pressed)
{
    bool is_back = button == SDL_CONTROLLER_BUTTON_BACK;
    uint32_t individual = is_back ? CHIAKI_CONTROLLER_BUTTON_SHARE
                                  : CHIAKI_CONTROLLER_BUTTON_OPTIONS;
    uint32_t other_individual = is_back ? CHIAKI_CONTROLLER_BUTTON_OPTIONS
                                        : CHIAKI_CONTROLLER_BUTTON_SHARE;
    PendingChordButton own_pending = is_back ? CHORD_BACK : CHORD_START;
    PendingChordButton other_pending = is_back ? CHORD_START : CHORD_BACK;
    bool send_twice = false;
    bool changed = false;

    pthread_mutex_lock(&ctx->mutex);
    if (pressed) {
        if (is_back) ctx->back_held = true;
        else         ctx->start_held = true;
        bool other_held = is_back ? ctx->start_held : ctx->back_held;
        if (ctx->chord_pending == other_pending || other_held) {
            ctx->state.buttons &= ~(individual | other_individual);
            ctx->state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
            ctx->chord_active = true;
            ctx->chord_pending = CHORD_NONE;
            changed = true;
        } else {
            ctx->chord_pending = own_pending;
            ctx->chord_started_ms = SDL_GetTicks();
        }
    } else {
        if (is_back) ctx->back_held = false;
        else         ctx->start_held = false;
        if (ctx->chord_active) {
            if (!ctx->back_held && !ctx->start_held) {
                ctx->state.buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
                ctx->chord_active = false;
                changed = true;
            }
        } else if (ctx->chord_pending == own_pending) {
            /* Preserve a quick tap by emitting a press followed by a release. */
            ctx->state.buttons |= individual;
            ctx->chord_pending = CHORD_NONE;
            changed = true;
            send_twice = true;
        } else {
            ctx->state.buttons &= ~individual;
            changed = true;
        }
    }
    pthread_mutex_unlock(&ctx->mutex);

    if (changed) send_state(ctx);
    if (send_twice) {
        pthread_mutex_lock(&ctx->mutex);
        ctx->state.buttons &= ~individual;
        pthread_mutex_unlock(&ctx->mutex);
        send_state(ctx);
    }
}

static void handle_controller_button(InputContext *ctx,
                                     const SDL_ControllerButtonEvent *event)
{
    if (!ctx->controller || event->which != ctx->instance_id)
        return;
    bool pressed = event->state == SDL_PRESSED;

    if (event->button == SDL_CONTROLLER_BUTTON_BACK ||
        event->button == SDL_CONTROLLER_BUTTON_START) {
        handle_chord_button(ctx, event->button, pressed);
        return;
    }

    uint32_t bit = controller_button_bit(event->button);
    if (!bit) return;
    pthread_mutex_lock(&ctx->mutex);
    if (pressed) ctx->state.buttons |= bit;
    else         ctx->state.buttons &= ~bit;
    pthread_mutex_unlock(&ctx->mutex);
    send_state(ctx);
}

static void handle_controller_axis(InputContext *ctx,
                                   const SDL_ControllerAxisEvent *event)
{
    if (!ctx->controller || event->which != ctx->instance_id)
        return;

    pthread_mutex_lock(&ctx->mutex);
    switch ((SDL_GameControllerAxis)event->axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:        ctx->state.left_x = event->value; break;
    case SDL_CONTROLLER_AXIS_LEFTY:        ctx->state.left_y = event->value; break;
    case SDL_CONTROLLER_AXIS_RIGHTX:       ctx->state.right_x = event->value; break;
    case SDL_CONTROLLER_AXIS_RIGHTY:       ctx->state.right_y = event->value; break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  ctx->state.l2_state = normalize_trigger(event->value); break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: ctx->state.r2_state = normalize_trigger(event->value); break;
    default:
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }
    pthread_mutex_unlock(&ctx->mutex);
    send_state(ctx);
}

#if SDL_VERSION_ATLEAST(2, 0, 14)
static void handle_controller_touchpad(
    InputContext *ctx, const SDL_ControllerTouchpadEvent *event)
{
    if (!ctx->controller || event->which != ctx->instance_id)
        return;

    ControllerTouchPhase phase;
    switch (event->type) {
    case SDL_CONTROLLERTOUCHPADDOWN:
        phase = CONTROLLER_TOUCH_DOWN;
        break;
    case SDL_CONTROLLERTOUCHPADMOTION:
        phase = CONTROLLER_TOUCH_MOTION;
        break;
    case SDL_CONTROLLERTOUCHPADUP:
        phase = CONTROLLER_TOUCH_UP;
        break;
    default:
        return;
    }

    pthread_mutex_lock(&ctx->mutex);
    bool changed = controller_features_handle_touch(
        &ctx->features, &ctx->state, phase,
        event->touchpad, event->finger, event->x, event->y);
    bool send_immediately = changed && phase != CONTROLLER_TOUCH_MOTION;
    if (changed && phase == CONTROLLER_TOUCH_MOTION)
        ctx->touch_motion_dirty = true;
    else if (changed && phase == CONTROLLER_TOUCH_UP)
        ctx->touch_motion_dirty = false;
    bool first_event = changed && !ctx->touch_event_logged;
    if (first_event)
        ctx->touch_event_logged = true;
    pthread_mutex_unlock(&ctx->mutex);

    if (!changed)
        return;
    if (first_event) {
        app_log_always(
            "[INPUT] First touch-surface event received (touchpad=%d finger=%d)\n",
            (int)event->touchpad, (int)event->finger);
    }
    if (send_immediately)
        send_state(ctx);
}

static void handle_controller_sensor(
    InputContext *ctx, const SDL_ControllerSensorEvent *event)
{
    if (!ctx->controller || event->which != ctx->instance_id)
        return;

    ControllerSensorType sensor;
    if (event->sensor == SDL_SENSOR_ACCEL)
        sensor = CONTROLLER_SENSOR_ACCEL;
    else if (event->sensor == SDL_SENSOR_GYRO)
        sensor = CONTROLLER_SENSOR_GYRO;
    else
        return;

    /* Keep timestamp handling identical to the pinned chiaki-ng frontend. */
    uint32_t timestamp_us = event->timestamp * 1000u;
    pthread_mutex_lock(&ctx->mutex);
    bool changed = controller_features_handle_sensor(
        &ctx->features, &ctx->state, sensor,
        event->data[0], event->data[1], event->data[2], timestamp_us);
    if (changed)
        ctx->motion_dirty = true;
    bool first_event = false;
    if (changed && sensor == CONTROLLER_SENSOR_ACCEL &&
        !ctx->accel_event_logged) {
        ctx->accel_event_logged = true;
        first_event = true;
    } else if (changed && sensor == CONTROLLER_SENSOR_GYRO &&
               !ctx->gyro_event_logged) {
        ctx->gyro_event_logged = true;
        first_event = true;
    }
    pthread_mutex_unlock(&ctx->mutex);

    if (!changed)
        return;
    if (first_event) {
        app_log_always("[INPUT] First %s event received\n",
                       sensor == CONTROLLER_SENSOR_ACCEL
                           ? "accelerometer" : "gyroscope");
    }
}
#endif

static void handle_remote_key(InputContext *ctx, const SDL_KeyboardEvent *event)
{
    SDL_Keycode key = event->keysym.sym;
    bool pressed = event->state == SDL_PRESSED;
    uint32_t button = 0;
    if      (key == SDLK_UP)     button = CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    else if (key == SDLK_DOWN)   button = CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    else if (key == SDLK_LEFT)   button = CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    else if (key == SDLK_RIGHT)  button = CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
    else if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
                                 button = CHIAKI_CONTROLLER_BUTTON_CROSS;
    else if (key == WEBOS_KEY_GREEN_IR)
                                 button = CHIAKI_CONTROLLER_BUTTON_CROSS;
    else if (key == WEBOS_KEY_BLUE_IR)
                                 button = CHIAKI_CONTROLLER_BUTTON_OPTIONS;
    else if (key == WEBOS_KEY_YELLOW_IR)
                                 button = CHIAKI_CONTROLLER_BUTTON_PYRAMID;
    else return;

    pthread_mutex_lock(&ctx->mutex);
    if (!ctx->session) {
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }
    if (pressed) ctx->state.buttons |= button;
    else         ctx->state.buttons &= ~button;
    pthread_mutex_unlock(&ctx->mutex);
    send_state(ctx);
}

InputContext *input_init(bool dualsense_bluetooth_enhanced)
{
    InputContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    pthread_mutex_init(&ctx->mutex, NULL);
    reset_controller_state_locked(ctx);
    ctx->instance_id = -1;
    ctx->rumble_multiplier = 1.0f;
    ctx->haptics_enabled = true;
    ctx->triggers_enabled = true;
    ctx->haptic_intensity = 0x00;
    ctx->trigger_intensity = 0x00;
    ctx->dualsense_bluetooth_enhanced = dualsense_bluetooth_enhanced;
    SDL_version linked;
    SDL_GetVersion(&linked);
    app_log_always("[INPUT] SDL runtime %u.%u.%u; standardized GameController input enabled\n",
                   linked.major, linked.minor, linked.patch);
    SDL_GameControllerEventState(SDL_ENABLE);
    open_first_controller(ctx);
    return ctx;
}

void input_set_session(InputContext *ctx, ChiakiSession *session)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->mutex);
    bool detached_session = ctx->session != NULL && session == NULL;
    uint64_t rumble_events = ctx->session_rumble_events;
    uint64_t haptic_frames = ctx->haptic_frames;
    ctx->session = session;
    /* UI controller events must never leak held buttons into a new stream. */
    reset_controller_state_locked(ctx);
    if (session) {
        ctx->session_rumble_nonzero_logged = false;
        ctx->haptic_frame_logged = false;
        ctx->haptic_nonzero_logged = false;
        ctx->rumble_write_logged = false;
        ctx->session_rumble_events = 0;
        ctx->haptic_frames = 0;
    }
    if (!session) {
        ctx->base_rumble_left = 0;
        ctx->base_rumble_right = 0;
        ctx->haptic_rumble_left = 0;
        ctx->haptic_rumble_right = 0;
        ctx->haptic_until_ms = 0;
        rumble_policy_reset(&ctx->rumble_policy);
    }
    DualSenseFeedback *feedback = ctx->dualsense_feedback;
    bool rumble_available = ctx->rumble_available;
    pthread_mutex_unlock(&ctx->mutex);

    if (!session) {
        if (detached_session)
            app_log_always("[INPUT] Feedback summary: rumble_events=%llu "
                           "haptic_frames=%llu\n",
                           (unsigned long long)rumble_events,
                           (unsigned long long)haptic_frames);
        if (ctx->controller && rumble_available)
            (void)SDL_GameControllerRumble(ctx->controller, 0, 0, 0);
        dualsense_feedback_release(feedback);
    } else {
        send_state(ctx);
    }
}

void input_handle_event(InputContext *ctx, const SDL_Event *event)
{
    if (!ctx || !event) return;
    switch (event->type) {
    case SDL_CONTROLLERDEVICEADDED:
        if (!ctx->controller)
            open_controller(ctx, event->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (ctx->controller && event->cdevice.which == ctx->instance_id) {
            close_controller(ctx);
            open_first_controller(ctx);
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        handle_controller_button(ctx, &event->cbutton);
        break;
    case SDL_CONTROLLERAXISMOTION:
        handle_controller_axis(ctx, &event->caxis);
        break;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLERTOUCHPADDOWN:
    case SDL_CONTROLLERTOUCHPADMOTION:
    case SDL_CONTROLLERTOUCHPADUP:
        handle_controller_touchpad(ctx, &event->ctouchpad);
        break;
    case SDL_CONTROLLERSENSORUPDATE:
        handle_controller_sensor(ctx, &event->csensor);
        break;
#endif
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        handle_remote_key(ctx, &event->key);
        break;
    default:
        break;
    }
}

static uint16_t scale_rumble(uint8_t value, float multiplier)
{
    float scaled = (float)value * 257.0f * multiplier;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 65535.0f) return UINT16_MAX;
    return (uint16_t)scaled;
}

static uint8_t player_led_pattern(uint8_t player_index)
{
    static const uint8_t patterns[] = { 0x04, 0x0a, 0x15, 0x1b, 0x1f };
    return player_index < sizeof(patterns) ? patterns[player_index] : 0x1f;
}

void input_handle_session_event(InputContext *ctx, const ChiakiEvent *event)
{
    if (!ctx || !event) return;

    bool log_rumble = false;
    unsigned rumble_left = 0;
    unsigned rumble_right = 0;
    pthread_mutex_lock(&ctx->mutex);
    DualSenseFeedback *feedback = ctx->dualsense_feedback;
    switch (event->type) {
    case CHIAKI_EVENT_RUMBLE: {
        ctx->base_rumble_left = event->rumble.left;
        ctx->base_rumble_right = event->rumble.right;
        ++ctx->session_rumble_events;
        if ((event->rumble.left || event->rumble.right) &&
            !ctx->session_rumble_nonzero_logged) {
            ctx->session_rumble_nonzero_logged = true;
            log_rumble = true;
            rumble_left = event->rumble.left;
            rumble_right = event->rumble.right;
        }
        break;
    }
    case CHIAKI_EVENT_LED_COLOR:
        memcpy(ctx->led, event->led_state, sizeof(ctx->led));
        ctx->led_pending = true;
        if (feedback)
            dualsense_feedback_set_lightbar(feedback,
                event->led_state[0], event->led_state[1], event->led_state[2]);
        break;
    case CHIAKI_EVENT_PLAYER_INDEX:
        ctx->player_index = event->player_index;
        ctx->player_index_pending = true;
        if (feedback)
            dualsense_feedback_set_player_leds(
                feedback, player_led_pattern(event->player_index));
        break;
    case CHIAKI_EVENT_HAPTIC_INTENSITY:
        switch (event->intensity) {
        case Off:
            ctx->haptic_intensity = -1;
            ctx->rumble_multiplier = 0.0f;
            ctx->haptics_enabled = false;
            ctx->base_rumble_left = ctx->base_rumble_right = 0;
            ctx->haptic_rumble_left = ctx->haptic_rumble_right = 0;
            break;
        case Weak:
            ctx->haptic_intensity = 0x03;
            ctx->rumble_multiplier = 0.33f;
            ctx->haptics_enabled = true;
            break;
        case Medium:
            ctx->haptic_intensity = 0x02;
            ctx->rumble_multiplier = 0.5f;
            ctx->haptics_enabled = true;
            break;
        case Strong:
        default:
            ctx->haptic_intensity = 0x00;
            ctx->rumble_multiplier = 1.0f;
            ctx->haptics_enabled = true;
            break;
        }
        if (feedback) {
            uint8_t intensity = (uint8_t)(
                (ctx->trigger_intensity < 0 ? 0xf0 : ctx->trigger_intensity) |
                (ctx->haptic_intensity < 0 ? 0x0f : ctx->haptic_intensity));
            dualsense_feedback_set_intensity(feedback, intensity);
        }
        break;
    case CHIAKI_EVENT_TRIGGER_INTENSITY:
        switch (event->intensity) {
        case Off:    ctx->trigger_intensity = -1;   ctx->triggers_enabled = false; break;
        case Weak:   ctx->trigger_intensity = 0x90; ctx->triggers_enabled = true;  break;
        case Medium: ctx->trigger_intensity = 0x60; ctx->triggers_enabled = true;  break;
        case Strong:
        default:     ctx->trigger_intensity = 0x00; ctx->triggers_enabled = true;  break;
        }
        if (feedback) {
            uint8_t intensity = (uint8_t)(
                (ctx->trigger_intensity < 0 ? 0xf0 : ctx->trigger_intensity) |
                (ctx->haptic_intensity < 0 ? 0x0f : ctx->haptic_intensity));
            dualsense_feedback_set_intensity(feedback, intensity);
            if (!ctx->triggers_enabled) {
                const uint8_t clear[10] = {0};
                dualsense_feedback_set_trigger_effects(feedback, 0, clear, 0, clear);
            }
        }
        break;
    case CHIAKI_EVENT_TRIGGER_EFFECTS:
        if (feedback && ctx->triggers_enabled) {
            dualsense_feedback_set_trigger_effects(
                feedback,
                event->trigger_effects.type_left, event->trigger_effects.left,
                event->trigger_effects.type_right, event->trigger_effects.right);
        }
        break;
    case CHIAKI_EVENT_MOTION_RESET:
        /* Apply on the SDL thread; session events arrive on Chiaki's thread. */
        ctx->motion_reset_pending = true;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&ctx->mutex);
    if (log_rumble)
        app_log_always("[INPUT] First non-zero Remote Play rumble event: "
                       "left=%u right=%u\n",
                       rumble_left, rumble_right);
}

static void haptics_header_cb(ChiakiAudioHeader *header, void *user)
{
    (void)header;
    (void)user;
}

static void haptics_frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
    InputContext *ctx = user;
    if (!ctx || !buf || buf_size < 4) return;

    size_t frames = buf_size / 4;
    uint64_t sum_left = 0, sum_right = 0;
    for (size_t i = 0; i < frames; ++i) {
        int16_t left, right;
        memcpy(&left, buf + i * 4, sizeof(left));
        memcpy(&right, buf + i * 4 + 2, sizeof(right));
        sum_left += left < 0 ? (uint32_t)(-(int32_t)left) : (uint32_t)left;
        sum_right += right < 0 ? (uint32_t)(-(int32_t)right) : (uint32_t)right;
    }

    uint32_t left = (uint32_t)((sum_left * 2u) / frames);
    uint32_t right = (uint32_t)((sum_right * 2u) / frames);
    if (left <= HAPTIC_RUMBLE_MIN_STRENGTH) left = 0;
    if (right <= HAPTIC_RUMBLE_MIN_STRENGTH) right = 0;
    if (left > UINT16_MAX) left = UINT16_MAX;
    if (right > UINT16_MAX) right = UINT16_MAX;

    pthread_mutex_lock(&ctx->mutex);
    ++ctx->haptic_frames;
    bool log_haptic_frame = !ctx->haptic_frame_logged;
    ctx->haptic_frame_logged = true;
    if (!ctx->haptics_enabled) {
        left = right = 0;
    } else {
        left = (uint32_t)((float)left * ctx->rumble_multiplier);
        right = (uint32_t)((float)right * ctx->rumble_multiplier);
        if (left > UINT16_MAX) left = UINT16_MAX;
        if (right > UINT16_MAX) right = UINT16_MAX;
    }
    ctx->haptic_rumble_left = (uint16_t)left;
    ctx->haptic_rumble_right = (uint16_t)right;
    ctx->haptic_until_ms = (left || right) ? monotonic_ms() + HAPTIC_HOLD_MS : 0;
    bool log_nonzero_haptic = (left || right) && !ctx->haptic_nonzero_logged;
    if (log_nonzero_haptic)
        ctx->haptic_nonzero_logged = true;
    pthread_mutex_unlock(&ctx->mutex);
    if (log_haptic_frame)
        app_log_always("[INPUT] First PS5 haptics frame: bytes=%llu frames=%llu "
                       "motor_left=%u motor_right=%u\n",
                       (unsigned long long)buf_size,
                       (unsigned long long)frames,
                       (unsigned)left, (unsigned)right);
    if (log_nonzero_haptic && !log_haptic_frame)
        app_log_always("[INPUT] First non-zero PS5 haptics frame: "
                       "motor_left=%u motor_right=%u\n",
                       (unsigned)left, (unsigned)right);
}

ChiakiAudioSink input_make_haptics_sink(InputContext *ctx)
{
    ChiakiAudioSink sink;
    memset(&sink, 0, sizeof(sink));
    sink.user = ctx;
    sink.header_cb = haptics_header_cb;
    sink.frame_cb = haptics_frame_cb;
    return sink;
}

void input_pump(InputContext *ctx)
{
    if (!ctx) return;

    bool chord_changed = false;
    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->chord_pending != CHORD_NONE &&
        (uint32_t)(SDL_GetTicks() - ctx->chord_started_ms) >= CHORD_WINDOW_MS) {
        if (ctx->chord_pending == CHORD_BACK)
            ctx->state.buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
        else
            ctx->state.buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
        ctx->chord_pending = CHORD_NONE;
        chord_changed = true;
    }

    bool motion_reset = ctx->motion_reset_pending;
    if (motion_reset) {
        ctx->motion_reset_pending = false;
        controller_features_reset_motion(&ctx->features, &ctx->state);
        ctx->motion_dirty = false;
        ctx->last_motion_send_ms = now;
    }

    bool motion_changed = !motion_reset && ctx->motion_dirty &&
        (ctx->last_motion_send_ms == 0 ||
         now - ctx->last_motion_send_ms >= CONTINUOUS_INPUT_SEND_INTERVAL_MS);
    if (motion_changed) {
        ctx->motion_dirty = false;
        ctx->last_motion_send_ms = now;
    }
    bool touch_motion_changed = ctx->touch_motion_dirty &&
        (ctx->last_touch_motion_send_ms == 0 ||
         now - ctx->last_touch_motion_send_ms >=
             CONTINUOUS_INPUT_SEND_INTERVAL_MS);
    if (touch_motion_changed) {
        ctx->touch_motion_dirty = false;
        ctx->last_touch_motion_send_ms = now;
    }

    float base_multiplier = ctx->is_dualsense ? 1.0f : ctx->rumble_multiplier;
    uint16_t left = scale_rumble((uint8_t)ctx->base_rumble_left, base_multiplier);
    uint16_t right = scale_rumble((uint8_t)ctx->base_rumble_right, base_multiplier);
    if (ctx->haptic_until_ms > now) {
        if (ctx->haptic_rumble_left > left) left = ctx->haptic_rumble_left;
        if (ctx->haptic_rumble_right > right) right = ctx->haptic_rumble_right;
    }
    bool is_dualsense = ctx->is_dualsense;
    bool rumble_available = ctx->rumble_available;
    bool rumble_due = rumble_available && rumble_policy_prepare(
        &ctx->rumble_policy, is_dualsense, now, left, right, &left, &right);
    bool log_rumble_write = rumble_due && (left || right) &&
                            !ctx->rumble_write_logged;
    if (log_rumble_write)
        ctx->rumble_write_logged = true;
    bool led_pending = ctx->led_pending;
    uint8_t led[3]; memcpy(led, ctx->led, sizeof(led));
    ctx->led_pending = false;
    bool player_pending = ctx->player_index_pending;
    int player_index = ctx->player_index;
    ctx->player_index_pending = false;
    pthread_mutex_unlock(&ctx->mutex);

    if (motion_reset) {
        app_log_always("[INPUT] Motion controls recalibrated\n");
        send_state(ctx);
    } else if (chord_changed || motion_changed || touch_motion_changed) {
        send_state(ctx);
    }
    if (!ctx->controller) return;

    if (ctx->last_power_poll_ms == 0 ||
        now - ctx->last_power_poll_ms >= CONTROLLER_POWER_POLL_INTERVAL_MS) {
        SDL_JoystickPowerLevel power = SDL_JoystickCurrentPowerLevel(
            SDL_GameControllerGetJoystick(ctx->controller));
        ctx->last_power_poll_ms = now;
        app_log_always("[INPUT] Controller power: %s (uptime_ms=%llu)\n",
                       power_level_name(power), (unsigned long long)now);
    }

    if (rumble_due) {
        Uint32 duration = (left || right) ? 5000u : 0u;
        int result = SDL_GameControllerRumble(
            ctx->controller, left, right, duration);
        pthread_mutex_lock(&ctx->mutex);
        rumble_policy_report_result(&ctx->rumble_policy, result == 0);
        pthread_mutex_unlock(&ctx->mutex);
        if (log_rumble_write)
            app_log_always("[INPUT] First SDL rumble write: left=%u right=%u "
                           "duration_ms=%u result=%d%s%s\n",
                           (unsigned)left, (unsigned)right,
                           (unsigned)duration, result,
                           result == 0 ? "" : " error=",
                           result == 0 ? "" : SDL_GetError());
        if (result != 0) {
            if (!ctx->rumble_error_logged) {
                app_log_always("[INPUT] Controller rumble unavailable: %s\n",
                               SDL_GetError());
                ctx->rumble_error_logged = true;
            }
        } else {
            ctx->rumble_error_logged = false;
        }
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    /* Advanced DualSense LED writes remain quarantined with the Luna path. */
    if (led_pending && !is_dualsense)
        (void)SDL_GameControllerSetLED(ctx->controller, led[0], led[1], led[2]);
#else
    (void)led_pending; (void)led; (void)is_dualsense;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (player_pending && !is_dualsense)
        SDL_GameControllerSetPlayerIndex(ctx->controller, player_index);
#else
    (void)player_pending; (void)player_index;
#endif
}

void input_fini(InputContext *ctx)
{
    if (!ctx) return;
    input_set_session(ctx, NULL);
    close_controller(ctx);
    pthread_mutex_destroy(&ctx->mutex);
    free(ctx);
}
