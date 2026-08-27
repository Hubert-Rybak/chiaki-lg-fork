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
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LUNA_SEND_PUB "/usr/bin/luna-send-pub"
#define SEND_DATA_URI \
    "luna://com.webos.service.bluetooth2/hid/internal/sendData"
#define COMMON_OFFSET             3
#define FLAG0_HAPTICS_SELECT      0x02
#define FLAG0_RIGHT_TRIGGER       0x04
#define FLAG0_LEFT_TRIGGER        0x08
#define FLAG1_LIGHTBAR            0x04
#define FLAG1_PLAYER_LEDS         0x10
#define FLAG1_EFFECT_INTENSITY    0x40
#define FLAG2_COMPATIBLE_VIBRATION2 0x04
#define OFF_MOTOR_RIGHT           2
#define OFF_MOTOR_LEFT            3
#define OFF_RIGHT_TRIGGER         10
#define OFF_LEFT_TRIGGER          21
#define OFF_EFFECT_INTENSITY      36
#define OFF_VALID_FLAG2           38
#define OFF_PLAYER_LEDS           43
#define OFF_LIGHTBAR_RED          44

#define MIN_SEND_INTERVAL_MS      100
#define LUNA_CALL_TIMEOUT_MS      800
#define RELEASE_WAIT_MS           2500
#define RUMBLE_HEARTBEAT_MS       500
#define WRITER_IDLE_STOP_MS       1250
#define WRITER_ACK_TIMEOUT_MS     1200
#define WRITER_STOP_WAIT_MS       2500
#define CHILD_TERM_WAIT_MS        250
#define CHILD_KILL_WAIT_MS        250
#define WRITER_MAGIC              UINT32_C(0x44533557)
#define WRITER_PROTOCOL_VERSION   1u

typedef enum WriterMessageType {
    WRITER_MESSAGE_REPORT = 1,
    WRITER_MESSAGE_SHUTDOWN = 2,
} WriterMessageType;

typedef struct WriterMessage {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t request_id;
    char address[32];
    uint8_t report[DUALSENSE_REPORT_LEN];
} WriterMessage;

typedef struct WriterReply {
    uint32_t magic;
    uint16_t version;
    int16_t status;
    uint32_t request_id;
} WriterReply;

typedef enum WriterReplyResult {
    WRITER_REPLY_TRANSPORT_FAILED = -1,
    WRITER_REPLY_DELIVERY_FAILED = 0,
    WRITER_REPLY_DELIVERED = 1,
} WriterReplyResult;

static pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
static int writer_fd = -1;
static pid_t writer_pid = -1;
static uint32_t writer_request_id;
static volatile sig_atomic_t writer_child_stop;

struct DualSenseFeedback {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool running;
    bool thread_started;
    bool pending;
    uint64_t queued_generation;
    uint64_t sent_generation;
    bool sent_generation_success;
    bool delivery_attempted;
    bool delivery_healthy;
    char address[32];
    DualSenseOutputState state;
};

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool wait_child_bounded(pid_t child, uint64_t timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD))
            return true;
        if (result < 0 && errno != EINTR)
            return true; /* Do not risk signalling a PID we cannot verify. */
        if (monotonic_ms() >= deadline)
            return false;
        usleep(10000);
    }
}

static void terminate_and_reap_child(pid_t child)
{
    if (child <= 0 || wait_child_bounded(child, 0))
        return;

    (void)kill(child, SIGTERM);
    if (wait_child_bounded(child, CHILD_TERM_WAIT_MS))
        return;

    (void)kill(child, SIGKILL);
    if (!wait_child_bounded(child, CHILD_KILL_WAIT_MS))
        app_log_always("[DUALSENSE] Timed out reaping output child %ld\n",
                       (long)child);
}

bool dualsense_output_state_equal(const DualSenseOutputState *a,
                                  const DualSenseOutputState *b)
{
    return a && b &&
           a->rumble_owned == b->rumble_owned &&
           a->motor_left == b->motor_left &&
           a->motor_right == b->motor_right &&
           a->lightbar_set == b->lightbar_set &&
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

static bool set_close_on_exec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

void dualsense_output_state_set_rumble(DualSenseOutputState *state,
                                       uint8_t left, uint8_t right)
{
    if (!state)
        return;
    state->rumble_owned = 1;
    state->motor_left = left;
    state->motor_right = right;
}

void dualsense_output_state_release(DualSenseOutputState *state)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->rumble_owned = 1;
    state->triggers_owned = 1;
    state->intensity = 0xff;
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

    if (state->rumble_owned) {
        /* DualSense firmware 2.21+ uses compatible-vibration v2. */
        report[COMMON_OFFSET] |= FLAG0_HAPTICS_SELECT;
        report[COMMON_OFFSET + OFF_MOTOR_RIGHT] = state->motor_right;
        report[COMMON_OFFSET + OFF_MOTOR_LEFT] = state->motor_left;
        report[COMMON_OFFSET + OFF_VALID_FLAG2] |=
            FLAG2_COMPATIBLE_VIBRATION2;
    }
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

static bool luna_send_report_direct(
    const char *address, const uint8_t report[DUALSENSE_REPORT_LEN])
{
    char payload[DUALSENSE_REPORT_LEN * 4 + 96];
    if (!dualsense_build_payload(address, report, payload, sizeof(payload)))
        return false;

    int response_pipe[2];
    if (pipe(response_pipe) != 0)
        return false;
    if (!set_close_on_exec(response_pipe[0]) ||
        !set_close_on_exec(response_pipe[1])) {
        close(response_pipe[0]);
        close(response_pipe[1]);
        return false;
    }

    pid_t child = fork();
    if (child < 0) {
        close(response_pipe[0]);
        close(response_pipe[1]);
        return false;
    }
    if (child == 0) {
        close(response_pipe[0]);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        dup2(response_pipe[1], STDOUT_FILENO);
        if (response_pipe[1] != STDOUT_FILENO)
            close(response_pipe[1]);
        execl(LUNA_SEND_PUB, "luna-send-pub", "-n", "1", "-w", "800",
              "-f", SEND_DATA_URI, payload, (char *)NULL);
        _exit(127);
    }
    close(response_pipe[1]);
    int pipe_flags = fcntl(response_pipe[0], F_GETFL, 0);
    if (pipe_flags >= 0)
        (void)fcntl(response_pipe[0], F_SETFL, pipe_flags | O_NONBLOCK);

    uint64_t deadline = monotonic_ms() + LUNA_CALL_TIMEOUT_MS;
    int status = 0;
    char response[4096];
    size_t response_used = 0;
    for (;;) {
        while (response_used + 1 < sizeof(response)) {
            ssize_t count = read(response_pipe[0], response + response_used,
                                 sizeof(response) - response_used - 1);
            if (count > 0) {
                response_used += (size_t)count;
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            break;
        if (result < 0 && errno != EINTR) {
            close(response_pipe[0]);
            return false;
        }
        if (monotonic_ms() >= deadline) {
            terminate_and_reap_child(child);
            close(response_pipe[0]);
            return false;
        }
        usleep(10000);
    }

    for (;;) {
        ssize_t count = read(response_pipe[0], response + response_used,
                             sizeof(response) - response_used - 1);
        if (count > 0) {
            response_used += (size_t)count;
            if (response_used + 1 == sizeof(response))
                break;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    close(response_pipe[0]);
    response[response_used] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;

    const char *key = strstr(response, "\"returnValue\"");
    if (!key)
        return false;
    const char *colon = strchr(key + sizeof("\"returnValue\"") - 1, ':');
    if (!colon)
        return false;
    do {
        ++colon;
    } while (*colon && isspace((unsigned char)*colon));
    return strncmp(colon, "true", 4) == 0 &&
           (colon[4] == '\0' || colon[4] == ',' || colon[4] == '}' ||
            isspace((unsigned char)colon[4]));
}

static bool writer_reply(int fd, uint32_t request_id, int status)
{
    WriterReply reply = {
        .magic = WRITER_MAGIC,
        .version = WRITER_PROTOCOL_VERSION,
        .status = (int16_t)status,
        .request_id = request_id,
    };
    return send(fd, &reply, sizeof(reply), MSG_NOSIGNAL) ==
           (ssize_t)sizeof(reply);
}

static bool writer_send_release_direct(
    const char *address, const uint8_t previous[DUALSENSE_REPORT_LEN])
{
    DualSenseOutputState released;
    dualsense_output_state_release(&released);
    uint8_t report[DUALSENSE_REPORT_LEN];
    uint8_t sequence = previous
        ? (uint8_t)(((previous[1] >> 4) + 1u) & 0x0fu)
        : 0;
    dualsense_build_report(sequence, &released, report);
    return luna_send_report_direct(address, report);
}

static void writer_child_signal_handler(int signal_number)
{
    (void)signal_number;
    writer_child_stop = 1;
}

static int writer_child_loop(int fd, pid_t expected_parent)
{
    writer_child_stop = 0;
    signal(SIGTERM, writer_child_signal_handler);
    signal(SIGINT, writer_child_signal_handler);
    signal(SIGHUP, writer_child_signal_handler);
#ifdef PR_SET_PDEATHSIG
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (getppid() != expected_parent)
        return 1;

    if (!writer_reply(fd, 0, 0))
        return 1;

    bool have_last = false;
    char last_address[32] = {0};
    uint8_t last_report[DUALSENSE_REPORT_LEN] = {0};
    uint64_t last_report_ms = 0;

    while (!writer_child_stop) {
        struct pollfd descriptor = { .fd = fd, .events = POLLIN };
        int poll_result = poll(&descriptor, 1, 100);
        if (poll_result < 0 && errno != EINTR)
            break;

        uint64_t now = monotonic_ms();
        if (have_last && (last_report[5] || last_report[6]) &&
            now - last_report_ms >= WRITER_IDLE_STOP_MS) {
            if (writer_send_release_direct(last_address, last_report)) {
                DualSenseOutputState released;
                dualsense_output_state_release(&released);
                dualsense_build_report(
                    (uint8_t)(((last_report[1] >> 4) + 1u) & 0x0fu),
                    &released, last_report);
            }
            last_report_ms = now;
        }

        if (poll_result <= 0)
            continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (!(descriptor.revents & POLLIN))
            continue;

        WriterMessage message;
        ssize_t received;
        do {
            received = recv(fd, &message, sizeof(message), 0);
        } while (received < 0 && errno == EINTR);
        if (received == 0)
            break;
        if (received != (ssize_t)sizeof(message) ||
            message.magic != WRITER_MAGIC ||
            message.version != WRITER_PROTOCOL_VERSION)
            break;

        if (message.type == WRITER_MESSAGE_SHUTDOWN) {
            bool stopped = !have_last ||
                writer_send_release_direct(last_address, last_report);
            (void)writer_reply(fd, message.request_id, stopped ? 0 : -1);
            have_last = false;
            break;
        }
        if (message.type != WRITER_MESSAGE_REPORT ||
            !memchr(message.address, '\0', sizeof(message.address)))
            break;

        snprintf(last_address, sizeof(last_address), "%s", message.address);
        memcpy(last_report, message.report, sizeof(last_report));
        have_last = true;
        bool sent = luna_send_report_direct(last_address, last_report);
        last_report_ms = monotonic_ms();
        if (!writer_reply(fd, message.request_id, sent ? 0 : -1))
            break;
    }

    if (have_last)
        (void)writer_send_release_direct(last_address, last_report);
    close(fd);
    return 0;
}

static WriterReplyResult writer_wait_reply_locked(int fd, uint32_t request_id,
                                                  int timeout_ms)
{
    struct pollfd descriptor = { .fd = fd, .events = POLLIN };
    int poll_result;
    do {
        poll_result = poll(&descriptor, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result <= 0 || !(descriptor.revents & POLLIN))
        return WRITER_REPLY_TRANSPORT_FAILED;

    WriterReply reply;
    ssize_t received;
    do {
        received = recv(fd, &reply, sizeof(reply), 0);
    } while (received < 0 && errno == EINTR);
    if (received != (ssize_t)sizeof(reply) ||
        reply.magic != WRITER_MAGIC ||
        reply.version != WRITER_PROTOCOL_VERSION ||
        reply.request_id != request_id)
        return WRITER_REPLY_TRANSPORT_FAILED;
    if (reply.status == 0)
        return WRITER_REPLY_DELIVERED;
    if (reply.status == -1)
        return WRITER_REPLY_DELIVERY_FAILED;
    return WRITER_REPLY_TRANSPORT_FAILED;
}

bool dualsense_writer_start(void)
{
    pthread_mutex_lock(&writer_mutex);
    if (writer_fd >= 0) {
        pthread_mutex_unlock(&writer_mutex);
        return true;
    }
    if (access(LUNA_SEND_PUB, X_OK) != 0) {
        pthread_mutex_unlock(&writer_mutex);
        app_log_always("[DUALSENSE] Output worker unavailable: %s missing\n",
                       LUNA_SEND_PUB);
        return false;
    }

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0) {
        pthread_mutex_unlock(&writer_mutex);
        app_log_always("[DUALSENSE] Could not create output worker socket: %s\n",
                       strerror(errno));
        return false;
    }
    if (!set_close_on_exec(sockets[0]) || !set_close_on_exec(sockets[1])) {
        close(sockets[0]);
        close(sockets[1]);
        pthread_mutex_unlock(&writer_mutex);
        app_log_always(
            "[DUALSENSE] Could not secure output worker descriptors\n");
        return false;
    }
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        pthread_mutex_unlock(&writer_mutex);
        app_log_always("[DUALSENSE] Could not pre-spawn output worker: %s\n",
                       strerror(errno));
        return false;
    }
    if (child == 0) {
        close(sockets[0]);
        int result = writer_child_loop(sockets[1], parent);
        _exit(result == 0 ? 0 : 1);
    }

    close(sockets[1]);
    WriterReplyResult ready = writer_wait_reply_locked(
        sockets[0], 0, WRITER_ACK_TIMEOUT_MS);
    if (ready != WRITER_REPLY_DELIVERED) {
        close(sockets[0]);
        terminate_and_reap_child(child);
        pthread_mutex_unlock(&writer_mutex);
        app_log_always("[DUALSENSE] Output worker did not become ready\n");
        return false;
    }
    writer_fd = sockets[0];
    writer_pid = child;
    writer_request_id = 0;
    pthread_mutex_unlock(&writer_mutex);
    app_log_always("[DUALSENSE] Isolated Bluetooth output worker ready\n");
    return true;
}

bool dualsense_writer_available(void)
{
    pthread_mutex_lock(&writer_mutex);
    bool available = writer_fd >= 0 && writer_pid > 0;
    pthread_mutex_unlock(&writer_mutex);
    return available;
}

static bool writer_send_report(
    const char *address, const uint8_t report[DUALSENSE_REPORT_LEN])
{
    pthread_mutex_lock(&writer_mutex);
    if (writer_fd < 0 || writer_pid <= 0) {
        pthread_mutex_unlock(&writer_mutex);
        return false;
    }
    WriterMessage message;
    memset(&message, 0, sizeof(message));
    message.magic = WRITER_MAGIC;
    message.version = WRITER_PROTOCOL_VERSION;
    message.type = WRITER_MESSAGE_REPORT;
    message.request_id = ++writer_request_id;
    snprintf(message.address, sizeof(message.address), "%s", address);
    memcpy(message.report, report, sizeof(message.report));
    bool sent = send(writer_fd, &message, sizeof(message), MSG_NOSIGNAL) ==
                (ssize_t)sizeof(message);
    WriterReplyResult reply = sent
        ? writer_wait_reply_locked(writer_fd, message.request_id,
                                   WRITER_ACK_TIMEOUT_MS)
        : WRITER_REPLY_TRANSPORT_FAILED;
    if (reply == WRITER_REPLY_TRANSPORT_FAILED) {
        int failed_fd = writer_fd;
        pid_t failed_child = writer_pid;
        writer_fd = -1;
        writer_pid = -1;
        close(failed_fd);
        terminate_and_reap_child(failed_child);
    }
    pthread_mutex_unlock(&writer_mutex);
    return reply == WRITER_REPLY_DELIVERED;
}

void dualsense_writer_stop(void)
{
    pthread_mutex_lock(&writer_mutex);
    int fd = writer_fd;
    pid_t child = writer_pid;
    if (fd >= 0 && child > 0) {
        WriterMessage message;
        memset(&message, 0, sizeof(message));
        message.magic = WRITER_MAGIC;
        message.version = WRITER_PROTOCOL_VERSION;
        message.type = WRITER_MESSAGE_SHUTDOWN;
        message.request_id = ++writer_request_id;
        if (send(fd, &message, sizeof(message), MSG_NOSIGNAL) ==
            (ssize_t)sizeof(message)) {
            (void)writer_wait_reply_locked(fd, message.request_id,
                                           WRITER_ACK_TIMEOUT_MS);
        }
        close(fd);
    }
    writer_fd = -1;
    writer_pid = -1;
    pthread_mutex_unlock(&writer_mutex);

    if (child > 0) {
        uint64_t deadline = monotonic_ms() + WRITER_STOP_WAIT_MS;
        for (;;) {
            pid_t result = waitpid(child, NULL, WNOHANG);
            if (result == child || (result < 0 && errno == ECHILD))
                break;
            if (result < 0 && errno != EINTR)
                break;
            if (monotonic_ms() >= deadline) {
                terminate_and_reap_child(child);
                break;
            }
            usleep(10000);
        }
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
        bool heartbeat_due = false;
        while (feedback->running && !feedback->pending) {
            if (failing ||
                (have_last && last_state.rumble_owned &&
                 (last_state.motor_left || last_state.motor_right))) {
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_nsec += RUMBLE_HEARTBEAT_MS * 1000000L;
                deadline.tv_sec += deadline.tv_nsec / 1000000000L;
                deadline.tv_nsec %= 1000000000L;
                int wait_result = pthread_cond_timedwait(
                    &feedback->cond, &feedback->mutex, &deadline);
                if (wait_result == ETIMEDOUT) {
                    heartbeat_due = true;
                    break;
                }
            } else {
                pthread_cond_wait(&feedback->cond, &feedback->mutex);
            }
        }
        if (!feedback->running && !feedback->pending) {
            pthread_mutex_unlock(&feedback->mutex);
            break;
        }
        DualSenseOutputState state = feedback->state;
        uint64_t generation = feedback->queued_generation;
        feedback->pending = false;
        pthread_mutex_unlock(&feedback->mutex);

        if (have_last && !heartbeat_due &&
            dualsense_output_state_equal(&state, &last_state)) {
            pthread_mutex_lock(&feedback->mutex);
            if (generation >= feedback->sent_generation) {
                feedback->sent_generation = generation;
                feedback->sent_generation_success = true;
            }
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
        if (heartbeat_due || !have_last ||
            !dualsense_output_state_equal(&state, &last_state)) {
            uint8_t report[DUALSENSE_REPORT_LEN];
            dualsense_build_report(sequence++, &state, report);
            sent_ok = writer_send_report(feedback->address, report);
            last_attempt_ms = monotonic_ms();
            if (sent_ok) {
                last_state = state;
                have_last = true;
                if (failing)
                    app_log("[DUALSENSE] Bluetooth feedback recovered\n");
                failing = false;
            } else if (!failing) {
                app_log("[DUALSENSE] Bluetooth feedback send failed; "
                        "retrying with the existing writer\n");
                failing = true;
            }
        }

        pthread_mutex_lock(&feedback->mutex);
        feedback->delivery_attempted = true;
        feedback->delivery_healthy = sent_ok;
        if (generation >= feedback->sent_generation) {
            feedback->sent_generation = generation;
            feedback->sent_generation_success = sent_ok;
        }
        pthread_cond_broadcast(&feedback->cond);
        pthread_mutex_unlock(&feedback->mutex);
    }
    return NULL;
}

DualSenseFeedback *dualsense_feedback_new(void)
{
    if (!dualsense_writer_available()) {
        app_log_always(
            "[DUALSENSE] Isolated output worker unavailable; "
            "advanced feedback disabled\n");
        return NULL;
    }

    char address[32];
    if (!dualsense_find_bluetooth_address(address, sizeof(address))) {
        app_log("[DUALSENSE] No Bluetooth DualSense address found\n");
        return NULL;
    }
    DualSenseFeedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback)
        return NULL;
    snprintf(feedback->address, sizeof(feedback->address), "%s", address);
    feedback->state.intensity = 0xff;
    feedback->running = true;
    feedback->delivery_healthy = true;
    pthread_mutex_init(&feedback->mutex, NULL);
    pthread_cond_init(&feedback->cond, NULL);

    if (pthread_create(&feedback->thread, NULL, sender_thread, feedback) != 0) {
        pthread_cond_destroy(&feedback->cond);
        pthread_mutex_destroy(&feedback->mutex);
        free(feedback);
        return NULL;
    }
    feedback->thread_started = true;
    app_log_always("[DUALSENSE] Isolated Bluetooth feedback active for %s "
                   "(rumble, triggers, lightbar, player LEDs)\n", address);
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

uint64_t dualsense_feedback_set_rumble(DualSenseFeedback *feedback,
                                       uint8_t left, uint8_t right)
{
    if (!feedback)
        return 0;
    pthread_mutex_lock(&feedback->mutex);
    if (!feedback->running) {
        pthread_mutex_unlock(&feedback->mutex);
        return 0;
    }
    dualsense_output_state_set_rumble(&feedback->state, left, right);
    feedback_queue_locked(feedback);
    uint64_t generation = feedback->queued_generation;
    pthread_mutex_unlock(&feedback->mutex);
    return generation;
}

bool dualsense_feedback_delivery_result(DualSenseFeedback *feedback,
                                        uint64_t generation, bool *success)
{
    if (!feedback || generation == 0 || !success)
        return false;
    pthread_mutex_lock(&feedback->mutex);
    bool complete = feedback->sent_generation >= generation;
    if (complete)
        *success = feedback->sent_generation_success;
    pthread_mutex_unlock(&feedback->mutex);
    return complete;
}

bool dualsense_feedback_healthy(DualSenseFeedback *feedback)
{
    if (!feedback)
        return false;
    pthread_mutex_lock(&feedback->mutex);
    bool healthy = !feedback->delivery_attempted || feedback->delivery_healthy;
    pthread_mutex_unlock(&feedback->mutex);
    return healthy;
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
    dualsense_output_state_release(&released);
    if (!dualsense_output_state_equal(&feedback->state, &released)) {
        feedback->state = released;
        feedback_queue_locked(feedback);
    }
    uint64_t generation = feedback->queued_generation;
    pthread_mutex_unlock(&feedback->mutex);

    uint64_t deadline = monotonic_ms() + RELEASE_WAIT_MS;
    for (;;) {
        pthread_mutex_lock(&feedback->mutex);
        bool complete = feedback->sent_generation >= generation;
        bool delivered = complete && feedback->sent_generation_success;
        if (complete && !delivered && feedback->running &&
            !feedback->pending && monotonic_ms() < deadline) {
            feedback_queue_locked(feedback);
            generation = feedback->queued_generation;
        }
        pthread_mutex_unlock(&feedback->mutex);
        if (delivered || monotonic_ms() >= deadline)
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
