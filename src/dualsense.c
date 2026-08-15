/*
 * DualSense Bluetooth feedback for webOS.
 *
 * Adapted from punktfunk-webos/src/platform/webos/{dualsense,luna}.rs at
 * commit 7bd261a9f51c89b68994bf619a19b48ef827948a.
 * Copyright (c) 2026 dyptan-io. Used under the MIT license; see
 * THIRD-PARTY-NOTICES.md. This C adaptation has been modified for Chiaki's
 * feedback event model.
 */

#define _GNU_SOURCE

#include "dualsense.h"
#include "app_log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LUNA_SEND_PUB "/usr/bin/luna-send-pub"
#define SEND_DATA_URI \
    "luna://com.webos.service.bluetooth2/hid/internal/sendData"
#define ROOT_PATCH_MARKER "/tmp/chiaki-bluetooth-output-patched"

#define COMMON_OFFSET             3
#define FLAG0_RIGHT_TRIGGER       0x04
#define FLAG0_LEFT_TRIGGER        0x08
#define FLAG1_LIGHTBAR            0x04
#define FLAG1_PLAYER_LEDS         0x10
#define FLAG1_EFFECT_INTENSITY    0x40
#define OFF_RIGHT_TRIGGER         10
#define OFF_LEFT_TRIGGER          21
#define OFF_EFFECT_INTENSITY      36
#define OFF_PLAYER_LEDS           43
#define OFF_LIGHTBAR_RED          44

#define MIN_SEND_INTERVAL_MS      250
#define LUNA_CALL_TIMEOUT_MS      800
#define RELEASE_WAIT_MS           2500

struct DualSenseFeedback {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool running;
    bool thread_started;
    bool pending;
    uint64_t queued_generation;
    uint64_t sent_generation;
    char address[32];
    DualSenseOutputState state;
};

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool state_equal(const DualSenseOutputState *a,
                        const DualSenseOutputState *b)
{
    return a->lightbar_set == b->lightbar_set &&
           memcmp(a->lightbar, b->lightbar, sizeof(a->lightbar)) == 0 &&
           a->player_leds_set == b->player_leds_set &&
           a->player_leds == b->player_leds &&
           memcmp(a->right_trigger, b->right_trigger,
                  sizeof(a->right_trigger)) == 0 &&
           memcmp(a->left_trigger, b->left_trigger,
                  sizeof(a->left_trigger)) == 0 &&
           a->triggers_owned == b->triggers_owned &&
           a->intensity_set == b->intensity_set &&
           a->intensity == b->intensity;
}

uint32_t dualsense_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1u) ? (crc >> 1) ^ UINT32_C(0xedb88320)
                             : crc >> 1;
    }
    return ~crc;
}

void dualsense_build_report(uint8_t sequence,
                            const DualSenseOutputState *state,
                            uint8_t report[DUALSENSE_REPORT_LEN])
{
    memset(report, 0, DUALSENSE_REPORT_LEN);
    report[0] = 0x31;
    report[1] = (uint8_t)((sequence & 0x0f) << 4);
    report[2] = 0x10;

    if (state->triggers_owned) {
        report[COMMON_OFFSET] |= FLAG0_RIGHT_TRIGGER | FLAG0_LEFT_TRIGGER;
        memcpy(report + COMMON_OFFSET + OFF_RIGHT_TRIGGER,
               state->right_trigger, DUALSENSE_EFFECT_LEN);
        memcpy(report + COMMON_OFFSET + OFF_LEFT_TRIGGER,
               state->left_trigger, DUALSENSE_EFFECT_LEN);
    }
    if (state->lightbar_set) {
        report[COMMON_OFFSET + 1] |= FLAG1_LIGHTBAR;
        memcpy(report + COMMON_OFFSET + OFF_LIGHTBAR_RED,
               state->lightbar, sizeof(state->lightbar));
    }
    if (state->player_leds_set) {
        report[COMMON_OFFSET + 1] |= FLAG1_PLAYER_LEDS;
        report[COMMON_OFFSET + OFF_PLAYER_LEDS] = state->player_leds & 0x1f;
    }
    if (state->intensity_set) {
        report[COMMON_OFFSET + 1] |= FLAG1_EFFECT_INTENSITY;
        report[COMMON_OFFSET + OFF_EFFECT_INTENSITY] = state->intensity;
    }

    /* hid-playstation signs Bluetooth output with the HIDP output seed 0xa2. */
    uint8_t signed_data[DUALSENSE_REPORT_LEN - 3];
    signed_data[0] = 0xa2;
    memcpy(signed_data + 1, report, DUALSENSE_REPORT_LEN - 4);
    uint32_t crc = dualsense_crc32(signed_data, sizeof(signed_data));
    report[DUALSENSE_REPORT_LEN - 4] = (uint8_t)crc;
    report[DUALSENSE_REPORT_LEN - 3] = (uint8_t)(crc >> 8);
    report[DUALSENSE_REPORT_LEN - 2] = (uint8_t)(crc >> 16);
    report[DUALSENSE_REPORT_LEN - 1] = (uint8_t)(crc >> 24);
}

bool dualsense_build_payload(const char *address,
                             const uint8_t report[DUALSENSE_REPORT_LEN],
                             char *payload, size_t payload_size)
{
    if (!address || !payload || payload_size == 0)
        return false;

    int n = snprintf(payload, payload_size,
                     "{\"address\":\"%s\",\"reportData\":[", address);
    if (n < 0 || (size_t)n >= payload_size)
        return false;
    size_t used = (size_t)n;

    for (size_t i = 0; i < DUALSENSE_REPORT_LEN; ++i) {
        n = snprintf(payload + used, payload_size - used,
                     i == 0 ? "%u" : ",%u", (unsigned)report[i]);
        if (n < 0 || (size_t)n >= payload_size - used)
            return false;
        used += (size_t)n;
    }
    n = snprintf(payload + used, payload_size - used, "]}");
    return n >= 0 && (size_t)n < payload_size - used;
}

static char *read_text_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;

    size_t capacity = 4096;
    size_t used = 0;
    char *text = malloc(capacity);
    if (!text) {
        fclose(file);
        return NULL;
    }

    for (;;) {
        if (used + 2048 + 1 > capacity) {
            capacity *= 2;
            char *grown = realloc(text, capacity);
            if (!grown) {
                free(text);
                fclose(file);
                return NULL;
            }
            text = grown;
        }
        size_t got = fread(text + used, 1, 2048, file);
        used += got;
        if (got < 2048) {
            if (ferror(file)) {
                free(text);
                fclose(file);
                return NULL;
            }
            break;
        }
    }
    fclose(file);
    text[used] = '\0';
    return text;
}

static bool contains_case_insensitive(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0)
        return true;
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) ==
               tolower((unsigned char)needle[i]))
            ++i;
        if (i == needle_len)
            return true;
    }
    return false;
}

static bool block_value(const char *block, const char *prefix,
                        char *out, size_t out_size)
{
    size_t prefix_len = strlen(prefix);
    const char *line = block;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);
        if (line_len >= prefix_len && memcmp(line, prefix, prefix_len) == 0) {
            const char *value = line + prefix_len;
            size_t value_len = line_len - prefix_len;
            while (value_len > 0 && isspace((unsigned char)value[value_len - 1]))
                --value_len;
            if (value_len + 1 > out_size)
                return false;
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return value_len > 0;
        }
        if (!end)
            break;
        line = end + 1;
    }
    return false;
}

static bool block_is_dualsense(const char *block)
{
    char name[256];
    return block_value(block, "N: Name=", name, sizeof(name)) &&
           contains_case_insensitive(name, "dualsense");
}

typedef bool (*BlockVisitor)(const char *block, void *user);

static bool visit_input_blocks(BlockVisitor visitor, void *user)
{
    char *text = read_text_file("/proc/bus/input/devices");
    if (!text)
        return false;

    bool matched = false;
    char *block = text;
    while (*block) {
        char *end = strstr(block, "\n\n");
        if (end)
            *end = '\0';
        if (visitor(block, user)) {
            matched = true;
            break;
        }
        if (!end)
            break;
        block = end + 2;
        while (*block == '\n')
            ++block;
    }
    free(text);
    return matched;
}

typedef struct {
    char *address;
    size_t address_size;
} AddressSearch;

bool dualsense_extract_bluetooth_address(const char *block,
                                         char *address, size_t address_size)
{
    if (!block || !address || address_size == 0 || !block_is_dualsense(block))
        return false;
    char candidate[64];

    /*
     * LG's old UHID bridge exposes the DualSense address byte-reversed in
     * U: Uniq, but preserves the canonical address in P: Phys. Prefer Phys
     * whenever it is a MAC address and retain Uniq for newer implementations.
     */
    const char *fields[] = { "P: Phys=", "U: Uniq=" };
    for (size_t field = 0; field < sizeof(fields) / sizeof(fields[0]); ++field) {
        if (!block_value(block, fields[field], candidate, sizeof(candidate)) ||
            strlen(candidate) != 17)
            continue;

        bool valid = true;
        for (size_t i = 0; i < 17; ++i) {
            if ((i + 1) % 3 == 0) {
                if (candidate[i] != ':')
                    valid = false;
            } else if (!isxdigit((unsigned char)candidate[i])) {
                valid = false;
            }
        }
        if (!valid || address_size < 18)
            continue;

        for (char *p = candidate; *p; ++p)
            *p = (char)tolower((unsigned char)*p);
        strcpy(address, candidate);
        return true;
    }
    return false;
}

static bool address_visitor(const char *block, void *user)
{
    AddressSearch *search = user;
    return dualsense_extract_bluetooth_address(block, search->address,
                                               search->address_size);
}

bool dualsense_find_bluetooth_address(char *address, size_t address_size)
{
    if (!address || address_size == 0)
        return false;
    address[0] = '\0';
    AddressSearch search = { address, address_size };
    return visit_input_blocks(address_visitor, &search);
}

static bool driver_visitor(const char *block, void *user)
{
    (void)user;
    if (!block_is_dualsense(block))
        return false;

    char sysfs[PATH_MAX];
    if (!block_value(block, "S: Sysfs=", sysfs, sizeof(sysfs)))
        return false;
    char *input = strstr(sysfs, "/input/input");
    if (!input)
        return false;
    *input = '\0';

    char driver_path[PATH_MAX];
    int n = snprintf(driver_path, sizeof(driver_path), "/sys%s/driver", sysfs);
    if (n < 0 || (size_t)n >= sizeof(driver_path))
        return false;

    char target[PATH_MAX];
    ssize_t len = readlink(driver_path, target, sizeof(target) - 1);
    if (len < 0)
        return false;
    target[len] = '\0';
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return strcmp(base, "playstation") == 0;
}

bool dualsense_hid_playstation_bound(void)
{
    return visit_input_blocks(driver_visitor, NULL);
}

static bool dualsense_root_transport_patched(void)
{
    FILE *marker = fopen(ROOT_PATCH_MARKER, "r");
    if (!marker)
        return false;

    long pid = 0;
    bool valid = fscanf(marker, "%ld", &pid) == 1 && pid > 0;
    fclose(marker);
    if (!valid)
        return false;

    char maps[64];
    int n = snprintf(maps, sizeof(maps), "/proc/%ld/maps", pid);
    return n > 0 && (size_t)n < sizeof(maps) && access(maps, R_OK) == 0;
}

static bool luna_send_report(const char *address,
                             const uint8_t report[DUALSENSE_REPORT_LEN])
{
    char payload[DUALSENSE_REPORT_LEN * 4 + 96];
    if (!dualsense_build_payload(address, report, payload, sizeof(payload))) {
        app_log("[DUALSENSE] Could not build Luna payload\n");
        return false;
    }

    pid_t child = fork();
    if (child < 0) {
        app_log("[DUALSENSE] fork failed: %s\n", strerror(errno));
        return false;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execl(LUNA_SEND_PUB, "luna-send-pub", "-n", "1",
              SEND_DATA_URI, payload, (char *)NULL);
        _exit(127);
    }

    uint64_t deadline = monotonic_ms() + LUNA_CALL_TIMEOUT_MS;
    int status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR)
            return false;
        if (monotonic_ms() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }
        usleep(10000);
    }
}

static void feedback_queue_locked(DualSenseFeedback *feedback)
{
    feedback->pending = true;
    ++feedback->queued_generation;
    pthread_cond_signal(&feedback->cond);
}

static void *sender_thread(void *user)
{
    DualSenseFeedback *feedback = user;
    DualSenseOutputState last_state;
    memset(&last_state, 0, sizeof(last_state));
    bool have_last = false;
    bool failing = false;
    uint8_t sequence = 0;
    uint64_t last_attempt_ms = 0;

    for (;;) {
        pthread_mutex_lock(&feedback->mutex);
        while (feedback->running && !feedback->pending)
            pthread_cond_wait(&feedback->cond, &feedback->mutex);
        if (!feedback->running && !feedback->pending) {
            pthread_mutex_unlock(&feedback->mutex);
            break;
        }
        DualSenseOutputState state = feedback->state;
        uint64_t generation = feedback->queued_generation;
        feedback->pending = false;
        pthread_mutex_unlock(&feedback->mutex);

        if (have_last && state_equal(&state, &last_state)) {
            pthread_mutex_lock(&feedback->mutex);
            if (generation > feedback->sent_generation)
                feedback->sent_generation = generation;
            pthread_cond_broadcast(&feedback->cond);
            pthread_mutex_unlock(&feedback->mutex);
            continue;
        }

        uint64_t now = monotonic_ms();
        while (last_attempt_ms && now - last_attempt_ms < MIN_SEND_INTERVAL_MS) {
            usleep(10000);
            now = monotonic_ms();
        }

        /* Pick up the newest absolute state accumulated while throttling. */
        pthread_mutex_lock(&feedback->mutex);
        if (feedback->pending) {
            state = feedback->state;
            generation = feedback->queued_generation;
            feedback->pending = false;
        }
        pthread_mutex_unlock(&feedback->mutex);

        bool sent_ok = true;
        if (!have_last || !state_equal(&state, &last_state)) {
            uint8_t report[DUALSENSE_REPORT_LEN];
            dualsense_build_report(sequence++, &state, report);
            sent_ok = luna_send_report(feedback->address, report);
            last_attempt_ms = monotonic_ms();
            if (sent_ok) {
                last_state = state;
                have_last = true;
                if (failing)
                    app_log("[DUALSENSE] Bluetooth feedback recovered\n");
                failing = false;
            } else if (!failing) {
                app_log("[DUALSENSE] Bluetooth feedback send failed; suppressing repeats\n");
                failing = true;
            }
        }

        pthread_mutex_lock(&feedback->mutex);
        if (generation > feedback->sent_generation)
            feedback->sent_generation = generation;
        pthread_cond_broadcast(&feedback->cond);
        pthread_mutex_unlock(&feedback->mutex);
    }
    return NULL;
}

DualSenseFeedback *dualsense_feedback_new(void)
{
    if (access(LUNA_SEND_PUB, X_OK) != 0) {
        app_log("[DUALSENSE] %s unavailable; advanced feedback disabled\n",
                LUNA_SEND_PUB);
        return NULL;
    }

    char address[32];
    if (!dualsense_find_bluetooth_address(address, sizeof(address))) {
        app_log("[DUALSENSE] No Bluetooth DualSense address found\n");
        return NULL;
    }
    if (!dualsense_hid_playstation_bound() &&
        !dualsense_root_transport_patched()) {
        app_log_always("[DUALSENSE] No native driver or rooted Bluetooth "
                       "transport correction; advanced feedback disabled\n");
        return NULL;
    }

    DualSenseFeedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback)
        return NULL;
    snprintf(feedback->address, sizeof(feedback->address), "%s", address);
    feedback->state.intensity = 0xff;
    feedback->running = true;
    pthread_mutex_init(&feedback->mutex, NULL);
    pthread_cond_init(&feedback->cond, NULL);

    if (pthread_create(&feedback->thread, NULL, sender_thread, feedback) != 0) {
        pthread_cond_destroy(&feedback->cond);
        pthread_mutex_destroy(&feedback->mutex);
        free(feedback);
        return NULL;
    }
    feedback->thread_started = true;
    app_log_always("[DUALSENSE] Bluetooth feedback active for %s "
                   "(triggers, lightbar, player LEDs)\n", address);
    return feedback;
}

void dualsense_feedback_set_lightbar(DualSenseFeedback *feedback,
                                     uint8_t red, uint8_t green, uint8_t blue)
{
    if (!feedback) return;
    pthread_mutex_lock(&feedback->mutex);
    feedback->state.lightbar_set = 1;
    feedback->state.lightbar[0] = red;
    feedback->state.lightbar[1] = green;
    feedback->state.lightbar[2] = blue;
    feedback_queue_locked(feedback);
    pthread_mutex_unlock(&feedback->mutex);
}

void dualsense_feedback_set_player_leds(DualSenseFeedback *feedback, uint8_t bits)
{
    if (!feedback) return;
    pthread_mutex_lock(&feedback->mutex);
    feedback->state.player_leds_set = 1;
    feedback->state.player_leds = bits & 0x1f;
    feedback_queue_locked(feedback);
    pthread_mutex_unlock(&feedback->mutex);
}

void dualsense_feedback_set_trigger_effects(DualSenseFeedback *feedback,
                                            uint8_t type_left, const uint8_t left[10],
                                            uint8_t type_right, const uint8_t right[10])
{
    if (!feedback) return;
    pthread_mutex_lock(&feedback->mutex);
    feedback->state.left_trigger[0] = type_left;
    memcpy(feedback->state.left_trigger + 1, left, 10);
    feedback->state.right_trigger[0] = type_right;
    memcpy(feedback->state.right_trigger + 1, right, 10);
    feedback->state.triggers_owned = 1;
    feedback->state.intensity_set = 1;
    feedback_queue_locked(feedback);
    pthread_mutex_unlock(&feedback->mutex);
}

void dualsense_feedback_set_intensity(DualSenseFeedback *feedback, uint8_t intensity)
{
    if (!feedback) return;
    pthread_mutex_lock(&feedback->mutex);
    feedback->state.intensity = intensity;
    feedback->state.intensity_set = 1;
    feedback_queue_locked(feedback);
    pthread_mutex_unlock(&feedback->mutex);
}

void dualsense_feedback_release(DualSenseFeedback *feedback)
{
    if (!feedback || !feedback->thread_started)
        return;

    pthread_mutex_lock(&feedback->mutex);
    DualSenseOutputState released;
    memset(&released, 0, sizeof(released));
    /* Mode zero only releases resistance when both trigger-valid bits are set. */
    released.triggers_owned = 1;
    released.intensity = 0xff;
    if (!state_equal(&feedback->state, &released)) {
        feedback->state = released;
        feedback_queue_locked(feedback);
    }
    uint64_t generation = feedback->queued_generation;
    pthread_mutex_unlock(&feedback->mutex);

    uint64_t deadline = monotonic_ms() + RELEASE_WAIT_MS;
    for (;;) {
        pthread_mutex_lock(&feedback->mutex);
        bool done = feedback->sent_generation >= generation;
        pthread_mutex_unlock(&feedback->mutex);
        if (done || monotonic_ms() >= deadline)
            break;
        usleep(10000);
    }
}

void dualsense_feedback_free(DualSenseFeedback *feedback)
{
    if (!feedback) return;
    dualsense_feedback_release(feedback);
    pthread_mutex_lock(&feedback->mutex);
    feedback->running = false;
    pthread_cond_signal(&feedback->cond);
    pthread_mutex_unlock(&feedback->mutex);
    if (feedback->thread_started)
        pthread_join(feedback->thread, NULL);
    pthread_cond_destroy(&feedback->cond);
    pthread_mutex_destroy(&feedback->mutex);
    free(feedback);
}
