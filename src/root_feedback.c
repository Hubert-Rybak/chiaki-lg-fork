#define _GNU_SOURCE

#include "root_feedback.h"

#include "app_id.h"
#include "app_log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

    pid_t child = fork();
    if (child < 0) {
        app_log("[ROOT] Could not start Homebrew root bootstrap: %s\n",
                strerror(errno));
        return;
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
        execl(LUNA_SEND_PUB, LUNA_SEND_PUB, "-n", "1", "-f",
              ROOT_EXEC_URI, payload, (char *)NULL);
        _exit(127);
    }

    int elapsed_ms = 0;
    int status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            break;
        if (result < 0 && errno != EINTR) {
            app_log("[ROOT] Could not wait for Homebrew root bootstrap: %s\n",
                    strerror(errno));
            return;
        }
        if (elapsed_ms >= ROOT_BOOTSTRAP_TIMEOUT_MS) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            app_log_always("[ROOT] Homebrew root bootstrap timed out; "
                           "continuing without controller rebind\n");
            return;
        }
        usleep(ROOT_BOOTSTRAP_POLL_MS * 1000);
        elapsed_ms += ROOT_BOOTSTRAP_POLL_MS;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        app_log("[ROOT] DualSense compatibility bootstrap completed\n");
    else
        app_log("[ROOT] Homebrew root bootstrap unavailable (status=%d)\n", status);
}
