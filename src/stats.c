#define _POSIX_C_SOURCE 200809L

#include "stats.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <chiaki/session.h> // for CHIAKI_CODEC_* constants

#ifdef HAVE_NDL_DIRECTMEDIA
#include "NDL_directmedia.h"
#endif

StreamStatsCounters g_stream_stats;

void stats_reset(StreamStatsCounters *c)
{
    if(!c)
        return;
    atomic_store_explicit(&c->video_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_samples, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_frames_lost, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_frames_recovered, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_fec_failures, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_feed_not_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_feed_keyframe_requests, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_feed_errors, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_idr_requests, 0, memory_order_relaxed);
    atomic_store_explicit(&c->reconnects, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_last_frame_ms, 0, memory_order_relaxed);

    atomic_store_explicit(&c->audio_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&c->audio_packets, 0, memory_order_relaxed);
    atomic_store_explicit(&c->audio_feed_not_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&c->audio_feed_overflows, 0, memory_order_relaxed);
    atomic_store_explicit(&c->audio_feed_errors, 0, memory_order_relaxed);

    atomic_store_explicit(&c->video_latency_ms, -1, memory_order_relaxed);

    // Don't force format to 0 if the caller already set it, but it's fine either way.
    // Keep current format as-is.
}

void stats_set_video_format(StreamStatsCounters *c, int w, int h, int fps, int codec)
{
    if(!c)
        return;
    atomic_store_explicit(&c->video_w, w, memory_order_relaxed);
    atomic_store_explicit(&c->video_h, h, memory_order_relaxed);
    atomic_store_explicit(&c->video_fps, fps, memory_order_relaxed);
    atomic_store_explicit(&c->video_codec, codec, memory_order_relaxed);
}

void stats_overlay_init(StatsOverlay *o)
{
    memset(o, 0, sizeof(*o));
    o->enabled = false;
}

void stats_overlay_toggle(StatsOverlay *o)
{
    if(!o)
        return;
    o->enabled = !o->enabled;
    // force immediate refresh
    o->last_sample_ms = 0;
}

static const char *codec_to_str(int codec)
{
    switch(codec)
    {
        case CHIAKI_CODEC_H265: return "H.265";
        case CHIAKI_CODEC_H265_HDR: return "H.265 HDR";
        case CHIAKI_CODEC_H264: return "H.264";
        default: return "Unknown";
    }
}

static int query_video_render_buf_frames(void)
{
#ifdef HAVE_NDL_DIRECTMEDIA
    int len = -1;
    int rc = NDL_DirectVideoGetRenderBufferLength(&len);
    if(rc != 0 || len < 0)
        return -1;
    return len;
#else
    return -1;
#endif
}

void stats_overlay_update(StatsOverlay *o, const StreamStatsCounters *c, uint64_t now_ms)
{
    if(!o || !c || !o->enabled)
        return;

    const uint64_t sample_period_ms = 500;
    if(o->last_sample_ms != 0 && now_ms - o->last_sample_ms < sample_period_ms)
        return;

    uint64_t vbytes = atomic_load_explicit(&c->video_bytes, memory_order_relaxed);
    uint64_t abytes = atomic_load_explicit(&c->audio_bytes, memory_order_relaxed);
    uint64_t vframes = atomic_load_explicit(&c->video_frames, memory_order_relaxed);
    uint64_t vsamples = atomic_load_explicit(&c->video_samples, memory_order_relaxed);
    uint64_t vlost = atomic_load_explicit(&c->video_frames_lost, memory_order_relaxed);
    uint64_t vrecovered = atomic_load_explicit(&c->video_frames_recovered, memory_order_relaxed);
    uint64_t vfec = atomic_load_explicit(&c->video_fec_failures, memory_order_relaxed);
    uint64_t vnotready = atomic_load_explicit(&c->video_feed_not_ready, memory_order_relaxed);
    uint64_t vkeyreq = atomic_load_explicit(&c->video_feed_keyframe_requests, memory_order_relaxed);
    uint64_t verrors = atomic_load_explicit(&c->video_feed_errors, memory_order_relaxed);
    uint64_t idr_requests = atomic_load_explicit(&c->video_idr_requests, memory_order_relaxed);
    uint64_t reconnects = atomic_load_explicit(&c->reconnects, memory_order_relaxed);
    uint64_t last_frame_ms = atomic_load_explicit(&c->video_last_frame_ms, memory_order_relaxed);
    uint64_t anotready = atomic_load_explicit(&c->audio_feed_not_ready, memory_order_relaxed);
    uint64_t aoverflows = atomic_load_explicit(&c->audio_feed_overflows, memory_order_relaxed);
    uint64_t aerrors = atomic_load_explicit(&c->audio_feed_errors, memory_order_relaxed);

    int vw = atomic_load_explicit(&c->video_w, memory_order_relaxed);
    int vh = atomic_load_explicit(&c->video_h, memory_order_relaxed);
    int vfps_cfg = atomic_load_explicit(&c->video_fps, memory_order_relaxed);
    int vcodec = atomic_load_explicit(&c->video_codec, memory_order_relaxed);
    int vlat_ms_reported = atomic_load_explicit(&c->video_latency_ms, memory_order_relaxed);

    float dt = 0.0f;
    if(o->last_sample_ms != 0)
        dt = (float)(now_ms - o->last_sample_ms) / 1000.0f;

    if(dt > 0.0f)
    {
        double dv = (double)(vbytes - o->last_video_bytes);
        double da = (double)(abytes - o->last_audio_bytes);
        double df = (double)(vframes - o->last_video_frames);

        o->mbps_video = (float)((dv * 8.0) / (dt * 1000.0 * 1000.0));
        o->mbps_audio = (float)((da * 8.0) / (dt * 1000.0 * 1000.0));
        o->mbps_total = o->mbps_video + o->mbps_audio;
        o->fps = (float)(df / dt);
    }

    o->feed_fail_total = vnotready + vkeyreq + verrors;
    o->last_frame_age_ms = last_frame_ms > 0 && now_ms >= last_frame_ms
        ? now_ms - last_frame_ms
        : 0;

    const uint64_t loss_sample_period_ms = 2000;
    if(o->last_loss_sample_ms == 0 ||
       now_ms - o->last_loss_sample_ms >= loss_sample_period_ms)
    {
        uint64_t sample_delta = vsamples - o->last_video_samples;
        uint64_t lost_delta = vlost - o->last_video_lost;
        uint64_t total = sample_delta + lost_delta;
        o->loss_percent = total > 0
            ? (float)((double)lost_delta * 100.0 / (double)total)
            : 0.0f;
        o->last_loss_sample_ms = now_ms;
        o->last_video_samples = vsamples;
        o->last_video_lost = vlost;
    }

    // Estimated pipeline buffer latency (video render queue). This is only a proxy for
    // "how much video is buffered", not a full end-to-end network latency.
    o->video_buf_frames = query_video_render_buf_frames();
    o->latency_ms = -1;
    if(o->video_buf_frames >= 0)
    {
        float fps_for_lat = (o->fps > 1.0f) ? o->fps : (float)vfps_cfg;
        if(fps_for_lat > 1.0f)
        {
            float buf_frames = (o->video_buf_frames > 0) ? (float)o->video_buf_frames : 0.5f;
            o->latency_ms = (int)((buf_frames * 1000.0f / fps_for_lat) + 0.5f);
        }
    }

    o->last_sample_ms = now_ms;
    o->last_video_bytes = vbytes;
    o->last_audio_bytes = abytes;
    o->last_video_frames = vframes;

    // Build display string (kept compact for TV readability)
    // Note: total bitrate here is based on payload sizes from Chiaki callbacks.
    char latency_line[96];
    if(vlat_ms_reported >= 0)
        snprintf(latency_line, sizeof(latency_line), "Latency: ~%d ms (SS4S avg)\n", vlat_ms_reported);
    else if(o->latency_ms >= 0)
        snprintf(latency_line, sizeof(latency_line), "Latency: ~%d ms (buf %d f)\n", o->latency_ms, o->video_buf_frames);
    else
        snprintf(latency_line, sizeof(latency_line), "Latency: n/a\n");

    snprintf(o->text, sizeof(o->text),
        "Stream Stats (UP to toggle)\n"
        "Video: %dx%d  %s\n"
        "Bitrate: %.2f Mbps  (V %.2f / A %.2f)\n"
        "FPS: %.1f\n"
        "%s"
        "Frame age: %llu ms\n"
        "Loss (2s): %.2f%%  lost %llu  recovered %llu  FEC %llu\n"
        "Decoder: not-ready %llu  keyframe %llu  errors %llu\n"
        "Audio: not-ready %llu  overflow %llu  errors %llu\n"
        "Recovery: IDR %llu  reconnects %llu\n",
        vw, vh, codec_to_str(vcodec),
        o->mbps_total, o->mbps_video, o->mbps_audio,
        o->fps,
        latency_line,
        (unsigned long long)o->last_frame_age_ms,
        o->loss_percent,
        (unsigned long long)vlost,
        (unsigned long long)vrecovered,
        (unsigned long long)vfec,
        (unsigned long long)vnotready,
        (unsigned long long)vkeyreq,
        (unsigned long long)verrors,
        (unsigned long long)anotready,
        (unsigned long long)aoverflows,
        (unsigned long long)aerrors,
        (unsigned long long)idr_requests,
        (unsigned long long)reconnects);
}

uint64_t stats_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}
