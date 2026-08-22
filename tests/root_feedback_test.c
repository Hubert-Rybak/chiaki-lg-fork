#define _GNU_SOURCE

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Include the implementation to exercise its internal JSON response parser. */
#include "../src/root_feedback.c"

void app_log(const char *fmt, ...)
{
    (void)fmt;
}

void app_log_always(const char *fmt, ...)
{
    (void)fmt;
}

int main(void)
{
    char error[128] = {0};
    assert(root_exec_response_success(
        "{\"returnValue\":true,\"stdoutString\":\"\"}",
        error, sizeof(error)));

    assert(!root_exec_response_success(
        "{\"returnValue\":false,\"errorText\":\"Command failed\\n\"}",
        error, sizeof(error)));
    assert(strcmp(error, "Command failed ") == 0);

    assert(!root_exec_response_success("not-json", error, sizeof(error)));
    assert(strstr(error, "invalid JSON") != NULL);

    puts("Root bootstrap response tests passed");
    return 0;
}
