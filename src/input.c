#include "input.h"
#include "app_log.h"
#include "dualsense.h"
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

typedef enum {
    CHORD_NONE = 0,
    CHORD_BACK,
    CHORD_START,
} PendingChordButton;

struct InputContext {
    pthread_mutex_t mutex;
    ChiakiControllerState state;
    ChiakiSession *session;

    SDL_GameController *controller;
    SDL_JoystickID instance_id;
    bool is_dualsense;
    DualSenseFeedback *dualsense_feedback;

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
    uint16_t last_rumble_left;
    uint16_t last_rumble_right;
    bool last_rumble_valid;
    bool rumble_error_logged;

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
    app_log("[INPUT] Closing controller: %s\n", name ? name : "unknown");
    SDL_GameControllerRumble(ctx->controller, 0, 0, 0);

    pthread_mutex_lock(&ctx->mutex);
    chiaki_controller_state_set_idle(&ctx->state);
    ctx->back_held = false;
    ctx->start_held = false;
    ctx->chord_active = false;
    ctx->chord_pending = CHORD_NONE;
    ctx->is_dualsense = false;
    ctx->last_rumble_valid = false;
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
    if (ctx->controller || device_index < 0 ||
        !SDL_IsGameController(device_index))
        return false;

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
    DualSenseFeedback *feedback = is_dualsense ? dualsense_feedback_new() : NULL;

    pthread_mutex_lock(&ctx->mutex);
    ctx->is_dualsense = is_dualsense;
    ctx->dualsense_feedback = feedback;
    ctx->last_rumble_valid = false;
    int combined_intensity =
        (ctx->trigger_intensity < 0 ? 0xf0 : ctx->trigger_intensity) |
        (ctx->haptic_intensity < 0 ? 0x0f : ctx->haptic_intensity);
    pthread_mutex_unlock(&ctx->mutex);

    if (feedback)
        dualsense_feedback_set_intensity(feedback, (uint8_t)combined_intensity);

    app_log_always("[INPUT] Controller opened: %s (instance=%d, DualSense=%s)\n",
                   SDL_GameControllerName(controller) ? SDL_GameControllerName(controller) : "unknown",
                   (int)instance,
                   is_dualsense ? "yes" : "no");
    return true;
}

static void open_first_controller(InputContext *ctx)
{
    if (ctx->controller) return;
    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        if (open_controller(ctx, i))
            return;
    }
    app_log("[INPUT] No SDL GameController found; Magic Remote remains available\n");
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

InputContext *input_init(void)
{
    InputContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    pthread_mutex_init(&ctx->mutex, NULL);
    chiaki_controller_state_set_idle(&ctx->state);
    ctx->instance_id = -1;
    ctx->rumble_multiplier = 1.0f;
    ctx->haptics_enabled = true;
    ctx->triggers_enabled = true;
    ctx->haptic_intensity = 0x00;
    ctx->trigger_intensity = 0x00;
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
    ctx->session = session;
    /* UI controller events must never leak held buttons into a new stream. */
    chiaki_controller_state_set_idle(&ctx->state);
    ctx->back_held = false;
    ctx->start_held = false;
    ctx->chord_active = false;
    ctx->chord_pending = CHORD_NONE;
    if (!session) {
        ctx->base_rumble_left = 0;
        ctx->base_rumble_right = 0;
        ctx->haptic_rumble_left = 0;
        ctx->haptic_rumble_right = 0;
        ctx->haptic_until_ms = 0;
        ctx->last_rumble_valid = false;
    }
    DualSenseFeedback *feedback = ctx->dualsense_feedback;
    pthread_mutex_unlock(&ctx->mutex);

    if (!session) {
        if (ctx->controller)
            SDL_GameControllerRumble(ctx->controller, 0, 0, 0);
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

    pthread_mutex_lock(&ctx->mutex);
    DualSenseFeedback *feedback = ctx->dualsense_feedback;
    switch (event->type) {
    case CHIAKI_EVENT_RUMBLE: {
        ctx->base_rumble_left = event->rumble.left;
        ctx->base_rumble_right = event->rumble.right;
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
    default:
        break;
    }
    pthread_mutex_unlock(&ctx->mutex);
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
    pthread_mutex_unlock(&ctx->mutex);
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

    uint64_t now = monotonic_ms();
    float base_multiplier = ctx->is_dualsense ? 1.0f : ctx->rumble_multiplier;
    uint16_t left = scale_rumble((uint8_t)ctx->base_rumble_left, base_multiplier);
    uint16_t right = scale_rumble((uint8_t)ctx->base_rumble_right, base_multiplier);
    if (ctx->haptic_until_ms > now) {
        if (ctx->haptic_rumble_left > left) left = ctx->haptic_rumble_left;
        if (ctx->haptic_rumble_right > right) right = ctx->haptic_rumble_right;
    }
    bool rumble_changed = !ctx->last_rumble_valid ||
                          left != ctx->last_rumble_left ||
                          right != ctx->last_rumble_right;
    if (rumble_changed) {
        ctx->last_rumble_left = left;
        ctx->last_rumble_right = right;
        ctx->last_rumble_valid = true;
    }
    bool led_pending = ctx->led_pending;
    uint8_t led[3]; memcpy(led, ctx->led, sizeof(led));
    ctx->led_pending = false;
    bool player_pending = ctx->player_index_pending;
    int player_index = ctx->player_index;
    ctx->player_index_pending = false;
    bool is_dualsense = ctx->is_dualsense;
    pthread_mutex_unlock(&ctx->mutex);

    if (chord_changed) send_state(ctx);
    if (!ctx->controller) return;

    if (rumble_changed) {
        Uint32 duration = (left || right) ? 5000u : 0u;
        if (SDL_GameControllerRumble(ctx->controller, left, right, duration) != 0) {
            if (!ctx->rumble_error_logged) {
                app_log("[INPUT] Controller rumble unavailable: %s\n", SDL_GetError());
                ctx->rumble_error_logged = true;
            }
        } else {
            ctx->rumble_error_logged = false;
        }
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    /* DualSense LED writes use Luna: SDL's HIDAPI route is jailed on webOS. */
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
