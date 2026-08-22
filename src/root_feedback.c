#define _GNU_SOURCE

#include "root_feedback.h"

#include "app_id.h"
#include "app_log.h"

#include <json-c/json.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LUNA_SEND_PUB "/usr/bin/luna-send-pub"
#define ROOT_EXEC_URI "luna://org.webosbrew.hbchannel.service/exec"
#define ROOT_BOOTSTRAP_TIMEOUT_MS 10000
#define ROOT_BOOTSTRAP_POLL_MS 50
#define ROOT_RESPONSE_MAX 4096

static void copy_root_error(char *out, size_t out_size, const char *message)
{
    if (!out || out_size == 0)
        return;
    if (!message)
        message = "unknown Homebrew service response";

    size_t i = 0;
    for (; message[i] && i + 1 < out_size; ++i) {
        char c = message[i];
        out[i] = (c == '\r' || c == '\n') ? ' ' : c;
    }
    out[i] = '\0';
}

static bool root_exec_response_success(
    const char *response, char *error, size_t error_size)
{
    json_object *root = response ? json_tokener_parse(response) : NULL;
    if (!root) {
        copy_root_error(error, error_size,
                        "invalid JSON from Homebrew root service");
        return false;
    }

    json_object *return_value = NULL;
    bool success = json_object_object_get_ex(
                       root, "returnValue", &return_value) &&
                   json_object_get_boolean(return_value);
    if (!success) {
        json_object *error_text = NULL;
        const char *message = NULL;
        if (json_object_object_get_ex(root, "errorText", &error_text))
            message = json_object_get_string(error_text);
        copy_root_error(error, error_size,
                        message ? message : "Homebrew command returned false");
    }
    json_object_put(root);
    return success;
}

static void drain_root_response(
    int fd, char *response, size_t response_size, size_t *used)
{
    while (*used + 1 < response_size) {
        ssize_t count = read(fd, response + *used, response_size - *used - 1);
        if (count > 0) {
            *used += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    response[*used] = '\0';
}

void root_feedback_bootstrap(void)
{
    if (access(LUNA_SEND_PUB, X_OK) != 0)
        return;

    char installer[512];
    int n = snprintf(installer, sizeof(installer), "%s/root/install.sh",
                     CHIAKI_APP_DIR);
    if (n < 0 || (size_t)n >= sizeof(installer) || access(installer, R_OK) != 0) {
        app_log("[ROOT] Bundled DualSense installer unavailable: %s\n",
                n > 0 ? installer : "invalid path");
        return;
    }

    /* APP_ID is compile-time validated to [a-z0-9.-], so no shell quoting is needed. */
    char payload[1200];
    n = snprintf(payload, sizeof(payload),
                 "{\"command\":\"/bin/sh %s %s --rebind-connected\"}",
                 installer, CHIAKI_APP_DIR);
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        app_log("[ROOT] DualSense installer command is too long\n");
        return;
    }

    int response_pipe[2];
    if (pipe(response_pipe) != 0) {
        app_log_always("[ROOT] Could not capture Homebrew root response: %s\n",
                       strerror(errno));
        return;
    }
    int pipe_flags = fcntl(response_pipe[0], F_GETFL, 0);
    if (pipe_flags >= 0)
        (void)fcntl(response_pipe[0], F_SETFL, pipe_flags | O_NONBLOCK);

    pid_t child = fork();
    if (child < 0) {
        close(response_pipe[0]);
        close(response_pipe[1]);
        app_log_always("[ROOT] Could not start Homebrew root bootstrap: %s\n",
                       strerror(errno));
        return;
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
        execl(LUNA_SEND_PUB, LUNA_SEND_PUB, "-n", "1", "-f",
              ROOT_EXEC_URI, payload, (char *)NULL);
        _exit(127);
    }
    close(response_pipe[1]);

    int elapsed_ms = 0;
    int status = 0;
    char response[ROOT_RESPONSE_MAX] = {0};
    size_t response_used = 0;
    for (;;) {
        drain_root_response(response_pipe[0], response,
                            sizeof(response), &response_used);
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            break;
        if (result < 0 && errno != EINTR) {
            close(response_pipe[0]);
            app_log_always("[ROOT] Could not wait for Homebrew root bootstrap: %s\n",
                           strerror(errno));
            return;
        }
        if (elapsed_ms >= ROOT_BOOTSTRAP_TIMEOUT_MS) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            drain_root_response(response_pipe[0], response,
                                sizeof(response), &response_used);
            close(response_pipe[0]);
            app_log_always("[ROOT] Homebrew root bootstrap timed out; "
                           "continuing without controller rebind\n");
            return;
        }
        usleep(ROOT_BOOTSTRAP_POLL_MS * 1000);
        elapsed_ms += ROOT_BOOTSTRAP_POLL_MS;
    }
    drain_root_response(response_pipe[0], response,
                        sizeof(response), &response_used);
    close(response_pipe[0]);

    char response_error[256] = {0};
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        root_exec_response_success(response, response_error,
                                   sizeof(response_error))) {
        app_log_always("[ROOT] DualSense compatibility bootstrap completed\n");
    } else {
        if (!response_error[0])
            copy_root_error(response_error, sizeof(response_error),
                            "luna-send-pub did not exit successfully");
        app_log_always("[ROOT] Compatibility bootstrap unavailable: %s "
                       "(wait_status=%d)\n", response_error, status);
    }
}
