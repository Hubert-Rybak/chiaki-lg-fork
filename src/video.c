#include "video.h"
#include "stats.h"
#include <chiaki/session.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration — defined in main.c */
extern void app_log(const char *fmt, ...);
struct VideoContext {
    SS4S_Player *player;
    int          chiaki_codec;
    bool         use_h265;      // true for H.265 or H.265 HDR sessions
    bool         opened;
    uint64_t     sample_count;
};

typedef struct VideoSampleInfo
{
    bool has_frame;
    bool is_keyframe;
} VideoSampleInfo;

static VideoSampleInfo video_sample_info(const uint8_t *buf, size_t size, bool h265)
{
    VideoSampleInfo info = {0};
    if (!buf || size < 4)
        return info;

    for (size_t i = 0; i + 3 < size; i++)
    {
        size_t header = 0;
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            header = i + 3;
        else if (i + 4 < size && buf[i] == 0 && buf[i + 1] == 0 &&
                 buf[i + 2] == 0 && buf[i + 3] == 1)
            header = i + 4;
        else
            continue;

        if (header >= size)
            continue;

        if (h265)
        {
            const unsigned int nal_type = (buf[header] >> 1) & 0x3fu;
            if (nal_type <= 31u)
                info.has_frame = true;
            if (nal_type >= 16u && nal_type <= 23u)
                info.is_keyframe = true;
        }
        else
        {
            const unsigned int nal_type = buf[header] & 0x1fu;
            if (nal_type >= 1u && nal_type <= 5u)
                info.has_frame = true;
            if (nal_type == 5u)
                info.is_keyframe = true;
        }
    }
    return info;
}

VideoContext *video_init(SS4S_Player *player, int width, int height, int fps,
                         int chiaki_codec, VideoInitResult *result)
{
    if (result)
        *result = VIDEO_INIT_ERROR;

    VideoContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->player = player;
    ctx->chiaki_codec = chiaki_codec;
    ctx->use_h265 = (chiaki_codec == CHIAKI_CODEC_H265 || chiaki_codec == CHIAKI_CODEC_H265_HDR);

    stats_set_video_format(&g_stream_stats, width, height, fps, chiaki_codec);

    SS4S_VideoInfo info = {
        .codec  = ctx->use_h265 ? SS4S_VIDEO_H265 : SS4S_VIDEO_H264,
        .width  = width,
        .height = height,
        .frameRateNumerator   = fps,
        .frameRateDenominator = 1,
    };

    SS4S_VideoOpenResult rc = SS4S_PlayerVideoOpen(ctx->player, &info);
    if (rc != SS4S_VIDEO_OPEN_OK) {
        app_log("[VIDEO/SS4S] PlayerVideoOpen FAILED rc=%d  codec=%s %dx%d@%dfps\n",
                rc, ctx->use_h265 ? "H265" : "H264", width, height, fps);
        if (result && rc == SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC)
            *result = VIDEO_INIT_UNSUPPORTED_CODEC;
        free(ctx);
        return NULL;
    }

    app_log("[VIDEO/SS4S] PlayerVideoOpen OK  codec=%s %dx%d@%dfps\n",
            ctx->use_h265 ? "H265" : "H264", width, height, fps);

    ctx->opened = true;
    if (result)
        *result = VIDEO_INIT_OK;
    return ctx;
}

void video_fini(VideoContext *ctx)
{
    if (!ctx) return;
    if (ctx->opened) SS4S_PlayerVideoClose(ctx->player);
    free(ctx);
}

bool video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost,
                     bool frame_recovered, void *user)
{
    VideoContext *ctx = (VideoContext *)user;

    if (!ctx || !ctx->opened || !buf || buf_size == 0)
        return false;

    ctx->sample_count++;
    atomic_fetch_add_explicit(&g_stream_stats.video_bytes, (uint64_t)buf_size, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stream_stats.video_samples, 1, memory_order_relaxed);
    if (frames_lost > 0)
        atomic_fetch_add_explicit(&g_stream_stats.video_frames_lost,
                                  (uint64_t)frames_lost, memory_order_relaxed);
    if (frame_recovered)
        atomic_fetch_add_explicit(&g_stream_stats.video_frames_recovered, 1, memory_order_relaxed);

    SS4S_VideoFeedFlags flags =
        SS4S_VIDEO_FEED_DATA_FRAME_START |
        SS4S_VIDEO_FEED_DATA_FRAME_END;

    const VideoSampleInfo sample_info =
        video_sample_info(buf, buf_size, ctx->use_h265);
    if (sample_info.is_keyframe)
        flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;

    if (ctx->sample_count <= 5 || ctx->sample_count % 600 == 0) {
        app_log("[VIDEO/SS4S] sample=%llu size=%zu keyframe=%d lost=%d recovered=%d\n",
                (unsigned long long)ctx->sample_count, buf_size,
                sample_info.is_keyframe,
                frames_lost, frame_recovered ? 1 : 0);
    }

    SS4S_VideoFeedResult rc = SS4S_PlayerVideoFeed(ctx->player, buf, buf_size, flags);
    switch (rc)
    {
    case SS4S_VIDEO_FEED_OK:
        /* chiaki-ng sends codec headers through this callback before the first
         * picture. Count only VCL NAL units as decoder progress so the startup
         * watchdog cannot be satisfied by SPS/PPS/VPS metadata alone. */
        if (sample_info.has_frame)
        {
            atomic_store_explicit(&g_stream_stats.video_last_frame_ms,
                                  stats_monotonic_ms(), memory_order_relaxed);
            atomic_fetch_add_explicit(&g_stream_stats.video_frames, 1,
                                      memory_order_release);
        }
        return true;
    case SS4S_VIDEO_FEED_NOT_READY:
        atomic_fetch_add_explicit(&g_stream_stats.video_feed_not_ready, 1,
                                  memory_order_relaxed);
        break;
    case SS4S_VIDEO_FEED_REQUEST_KEYFRAME:
        atomic_fetch_add_explicit(&g_stream_stats.video_feed_keyframe_requests, 1,
                                  memory_order_relaxed);
        break;
    case SS4S_VIDEO_FEED_ERROR:
    default:
        atomic_fetch_add_explicit(&g_stream_stats.video_feed_errors, 1,
                                  memory_order_relaxed);
        break;
    }

    if (ctx->sample_count <= 5 || ctx->sample_count % 600 == 0)
        app_log("[VIDEO/SS4S] PlayerVideoFeed rejected sample rc=%d\n", (int)rc);

    /* Returning false tells libchiaki that the decoder needs a clean frame. */
    return false;
}
