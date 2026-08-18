#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STREAM_HEALTH_IDR_COOLDOWN_MS          2000u
#define STREAM_HEALTH_ACTIVE_IDR_MS            1500u
#define STREAM_HEALTH_ACTIVE_RECONNECT_MS      5000u
#define STREAM_HEALTH_STARTUP_IDR_MS            5000u
#define STREAM_HEALTH_STARTUP_RECONNECT_MS     15000u
#define STREAM_HEALTH_LATENCY_SAMPLE_MS         1000u
#define STREAM_HEALTH_HIGH_LATENCY_MS            750
#define STREAM_HEALTH_HIGH_LATENCY_SAMPLES          5u
#define STREAM_HEALTH_STABLE_RESET_MS          60000u

typedef enum StreamHealthAction
{
    STREAM_HEALTH_ACTION_NONE = 0,
    STREAM_HEALTH_ACTION_REQUEST_IDR,
    STREAM_HEALTH_ACTION_RECONNECT_STARTUP,
    STREAM_HEALTH_ACTION_RECONNECT_STALL,
    STREAM_HEALTH_ACTION_RECONNECT_LATENCY,
} StreamHealthAction;

typedef struct StreamHealth
{
    uint64_t session_started_ms;
    uint64_t last_idr_request_ms;
    uint64_t last_latency_sample_ms;
    unsigned int high_latency_samples;
    bool idr_requested;
} StreamHealth;

void stream_health_init(StreamHealth *health, uint64_t now_ms);

/* Returns true and consumes the IDR cooldown when a request may be sent. */
bool stream_health_allow_idr(StreamHealth *health, uint64_t now_ms);

/*
 * Evaluate stream progress. last_frame_ms must use the same monotonic clock as
 * now_ms and is ignored until have_video_frame is true.
 */
StreamHealthAction stream_health_update(
    StreamHealth *health,
    uint64_t now_ms,
    bool have_video_frame,
    uint64_t last_frame_ms,
    bool latency_valid,
    int latency_ms);

/* 2s, 4s, 8s, then a 15s cap. consecutive_failures is zero-based. */
uint32_t stream_retry_delay_ms(unsigned int consecutive_failures);
