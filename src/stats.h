#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

// Thread-safe counters updated from Chiaki callback threads (video/audio)
// and sampled from the SDL/main thread for display.
typedef struct StreamStatsCounters
{
    atomic_uint_fast64_t video_bytes;
    atomic_uint_fast64_t video_samples;
    atomic_uint_fast64_t video_frames;
    atomic_uint_fast64_t video_frames_lost;
    atomic_uint_fast64_t video_frames_recovered;
    atomic_uint_fast64_t video_fec_failures;
    atomic_uint_fast64_t video_feed_not_ready;
    atomic_uint_fast64_t video_feed_keyframe_requests;
    atomic_uint_fast64_t video_feed_errors;
    atomic_uint_fast64_t video_idr_requests;
    atomic_uint_fast64_t reconnects;
    atomic_uint_fast64_t video_last_frame_ms;

    atomic_uint_fast64_t audio_bytes;
    atomic_uint_fast64_t audio_packets;
    atomic_uint_fast64_t audio_feed_not_ready;
    atomic_uint_fast64_t audio_feed_overflows;
    atomic_uint_fast64_t audio_feed_errors;

    atomic_int video_w;
    atomic_int video_h;
    atomic_int video_fps;   // configured/nominal fps (used for buffer-latency estimate)
    atomic_int video_codec; // CHIAKI_CODEC_* if available

    // Optional latency reported by SS4S video backend (ms). Updated from the main thread.
    // -1 means unknown/unavailable.
    atomic_int video_latency_ms;
} StreamStatsCounters;

extern StreamStatsCounters g_stream_stats;

void stats_reset(StreamStatsCounters *c);
void stats_set_video_format(StreamStatsCounters *c, int w, int h, int fps, int codec);

// Overlay state (main thread only)
typedef struct StatsOverlay
{
    bool enabled;

    uint64_t last_sample_ms;
    uint64_t last_loss_sample_ms;
    uint64_t last_video_bytes;
    uint64_t last_audio_bytes;
    uint64_t last_video_frames;
    uint64_t last_video_samples;
    uint64_t last_video_lost;

    // computed
    float mbps_total;
    float mbps_video;
    float mbps_audio;
    float fps;
    int   video_buf_frames;
    int   latency_ms;       // estimated pipeline buffer latency (video render queue)
    uint64_t last_frame_age_ms;
    float loss_percent;
    uint64_t feed_fail_total;

    char text[768];
} StatsOverlay;

void stats_overlay_init(StatsOverlay *o);
void stats_overlay_toggle(StatsOverlay *o);
void stats_overlay_update(StatsOverlay *o, const StreamStatsCounters *c, uint64_t now_ms);

uint64_t stats_monotonic_ms(void);
