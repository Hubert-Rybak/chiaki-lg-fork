#include "stream_health.h"

#include <string.h>

void stream_health_init(StreamHealth *health, uint64_t now_ms)
{
    if (!health)
        return;

    memset(health, 0, sizeof(*health));
    health->session_started_ms = now_ms;
    health->last_latency_sample_ms = now_ms;
}
bool stream_health_allow_idr(StreamHealth *health, uint64_t now_ms)
{
    if (!health)
        return false;

    if (health->idr_requested &&
        now_ms - health->last_idr_request_ms < STREAM_HEALTH_IDR_COOLDOWN_MS)
        return false;

    health->idr_requested = true;
    health->last_idr_request_ms = now_ms;
    return true;
}

StreamHealthAction stream_health_update(
    StreamHealth *health,
    uint64_t now_ms,
    bool have_video_frame,
    uint64_t last_frame_ms,
    bool latency_valid,
    int latency_ms)
{
    if (!health)
        return STREAM_HEALTH_ACTION_NONE;

    const uint64_t progress_age_ms = have_video_frame
        ? now_ms - last_frame_ms
        : now_ms - health->session_started_ms;

    if (!have_video_frame)
    {
        health->high_latency_samples = 0;
        if (progress_age_ms >= STREAM_HEALTH_STARTUP_RECONNECT_MS)
            return STREAM_HEALTH_ACTION_RECONNECT_STARTUP;
        if (progress_age_ms >= STREAM_HEALTH_STARTUP_IDR_MS &&
            stream_health_allow_idr(health, now_ms))
            return STREAM_HEALTH_ACTION_REQUEST_IDR;
        return STREAM_HEALTH_ACTION_NONE;
    }

    if (progress_age_ms >= STREAM_HEALTH_ACTIVE_RECONNECT_MS)
        return STREAM_HEALTH_ACTION_RECONNECT_STALL;

    if (now_ms - health->last_latency_sample_ms >= STREAM_HEALTH_LATENCY_SAMPLE_MS)
    {
        health->last_latency_sample_ms = now_ms;
        if (latency_valid && latency_ms > STREAM_HEALTH_HIGH_LATENCY_MS)
            health->high_latency_samples++;
        else
            health->high_latency_samples = 0;

        if (health->high_latency_samples >= STREAM_HEALTH_HIGH_LATENCY_SAMPLES)
            return STREAM_HEALTH_ACTION_RECONNECT_LATENCY;
    }

    if (progress_age_ms >= STREAM_HEALTH_ACTIVE_IDR_MS &&
        stream_health_allow_idr(health, now_ms))
        return STREAM_HEALTH_ACTION_REQUEST_IDR;

    return STREAM_HEALTH_ACTION_NONE;
}

uint32_t stream_retry_delay_ms(unsigned int consecutive_failures)
{
    static const uint32_t delays_ms[] = {2000u, 4000u, 8000u, 15000u};
    const unsigned int last =
        (unsigned int)(sizeof(delays_ms) / sizeof(delays_ms[0])) - 1u;
    return delays_ms[consecutive_failures < last ? consecutive_failures : last];
}

uint32_t stream_watchdog_retry_delay_ms(unsigned int watchdog_retries)
{
    return watchdog_retries == 0
        ? STREAM_HEALTH_WATCHDOG_GRACE_MS
        : STREAM_HEALTH_WATCHDOG_RETRY_MS;
}

bool stream_watchdog_can_retry_session_in_use(
    bool watchdog_recovery_active,
    unsigned int watchdog_retries)
{
    return watchdog_recovery_active &&
        watchdog_retries < STREAM_HEALTH_WATCHDOG_RP_IN_USE_RETRIES;
}
