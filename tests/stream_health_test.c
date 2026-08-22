#include <stdio.h>
#include <stdlib.h>

#include "stream_health.h"

static void expect_action(StreamHealthAction actual, StreamHealthAction expected,
                          const char *message)
{
    if (actual == expected)
        return;
    fprintf(stderr, "stream_health_test: %s (got %d, expected %d)\n",
            message, (int)actual, (int)expected);
    exit(1);
}

static void expect_delay(unsigned int failure, uint32_t expected)
{
    uint32_t actual = stream_retry_delay_ms(failure);
    if (actual == expected)
        return;
    fprintf(stderr, "stream_health_test: retry %u was %u ms, expected %u ms\n",
            failure, actual, expected);
    exit(1);
}

static void expect_watchdog_delay(unsigned int retry, uint32_t expected)
{
    uint32_t actual = stream_watchdog_retry_delay_ms(retry);
    if (actual == expected)
        return;
    fprintf(stderr, "stream_health_test: watchdog retry %u was %u ms, expected %u ms\n",
            retry, actual, expected);
    exit(1);
}

static void expect_in_use_retry(bool active, unsigned int retry, bool expected)
{
    bool actual = stream_watchdog_can_retry_session_in_use(active, retry);
    if (actual == expected)
        return;
    fprintf(stderr,
            "stream_health_test: watchdog in-use retry active=%d retry=%u was %d, expected %d\n",
            active, retry, actual, expected);
    exit(1);
}

int main(void)
{
    StreamHealth health;

    stream_health_init(&health, 1000);
    expect_action(stream_health_update(&health, 5999, false, 0, false, -1),
                  STREAM_HEALTH_ACTION_NONE, "startup IDR fired early");
    expect_action(stream_health_update(&health, 6000, false, 0, false, -1),
                  STREAM_HEALTH_ACTION_REQUEST_IDR, "startup IDR missing");
    expect_action(stream_health_update(&health, 7000, false, 0, false, -1),
                  STREAM_HEALTH_ACTION_NONE, "IDR cooldown ignored");
    expect_action(stream_health_update(&health, 8000, false, 0, false, -1),
                  STREAM_HEALTH_ACTION_REQUEST_IDR, "startup IDR retry missing");
    expect_action(stream_health_update(&health, 16000, false, 0, false, -1),
                  STREAM_HEALTH_ACTION_RECONNECT_STARTUP,
                  "startup reconnect threshold missing");

    stream_health_init(&health, 1000);
    expect_action(stream_health_update(&health, 3499, true, 2000, false, -1),
                  STREAM_HEALTH_ACTION_NONE, "active IDR fired early");
    expect_action(stream_health_update(&health, 3500, true, 2000, false, -1),
                  STREAM_HEALTH_ACTION_REQUEST_IDR, "active IDR missing");
    expect_action(stream_health_update(&health, 7000, true, 2000, false, -1),
                  STREAM_HEALTH_ACTION_RECONNECT_STALL,
                  "active stall reconnect missing");

    stream_health_init(&health, 0);
    for (unsigned int sample = 1; sample < STREAM_HEALTH_HIGH_LATENCY_SAMPLES; sample++)
        expect_action(stream_health_update(&health, sample * 1000u, true,
                                           sample * 1000u, true, 751),
                      STREAM_HEALTH_ACTION_NONE, "latency reconnect fired early");
    expect_action(stream_health_update(&health, 5000, true, 5000, true, 751),
                  STREAM_HEALTH_ACTION_RECONNECT_LATENCY,
                  "latency reconnect threshold missing");

    stream_health_init(&health, 0);
    expect_action(stream_health_update(&health, 1000, true, 1000, true, 900),
                  STREAM_HEALTH_ACTION_NONE, "first latency sample failed");
    expect_action(stream_health_update(&health, 2000, true, 2000, true, 200),
                  STREAM_HEALTH_ACTION_NONE, "healthy latency sample failed");
    for (unsigned int sample = 3; sample < 7; sample++)
        expect_action(stream_health_update(&health, sample * 1000u, true,
                                           sample * 1000u, true, 900),
                      STREAM_HEALTH_ACTION_NONE, "latency streak did not reset");
    expect_action(stream_health_update(&health, 7000, true, 7000, true, 900),
                  STREAM_HEALTH_ACTION_RECONNECT_LATENCY,
                  "latency streak after reset missing");

    expect_delay(0, 2000);
    expect_delay(1, 4000);
    expect_delay(2, 8000);
    expect_delay(3, 15000);
    expect_delay(20, 15000);

    expect_watchdog_delay(0, 10000);
    expect_watchdog_delay(1, 15000);
    expect_watchdog_delay(20, 15000);
    expect_in_use_retry(false, 0, false);
    expect_in_use_retry(true, 0, true);
    expect_in_use_retry(true, 2, true);
    expect_in_use_retry(true, 3, false);

    puts("stream_health_test: passed");
    return 0;
}
