#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chiaki/session.h>
#include <ss4s.h>

#include "audio.h"
#include "stats.h"
#include "video.h"

static SS4S_VideoOpenResult video_open_result = SS4S_VIDEO_OPEN_OK;
static SS4S_VideoFeedResult video_feed_result = SS4S_VIDEO_FEED_OK;
static SS4S_AudioOpenResult audio_open_result = SS4S_AUDIO_OPEN_OK;
static SS4S_AudioFeedResult audio_feed_result = SS4S_AUDIO_FEED_OK;
static SS4S_VideoFeedFlags last_video_flags;
static SS4S_AudioInfo last_audio_info;
static unsigned int video_close_count;
static unsigned int audio_open_count;
static unsigned int audio_close_count;

void app_log(const char *fmt, ...)
{
    (void)fmt;
}

SS4S_VideoOpenResult SS4S_PlayerVideoOpen(SS4S_Player *player,
                                           const SS4S_VideoInfo *info)
{
    (void)player;
    (void)info;
    return video_open_result;
}

SS4S_VideoFeedResult SS4S_PlayerVideoFeed(SS4S_Player *player,
                                           const unsigned char *data,
                                           size_t size,
                                           SS4S_VideoFeedFlags flags)
{
    (void)player;
    (void)data;
    (void)size;
    last_video_flags = flags;
    return video_feed_result;
}

bool SS4S_PlayerVideoClose(SS4S_Player *player)
{
    (void)player;
    video_close_count++;
    return true;
}

SS4S_AudioOpenResult SS4S_PlayerAudioOpen(SS4S_Player *player,
                                           const SS4S_AudioInfo *info)
{
    (void)player;
    last_audio_info = *info;
    audio_open_count++;
    return audio_open_result;
}

SS4S_AudioFeedResult SS4S_PlayerAudioFeed(SS4S_Player *player,
                                           const unsigned char *data,
                                           size_t size)
{
    (void)player;
    (void)data;
    (void)size;
    return audio_feed_result;
}

bool SS4S_PlayerAudioClose(SS4S_Player *player)
{
    (void)player;
    audio_close_count++;
    return true;
}

static void fail(const char *message)
{
    fprintf(stderr, "av_callbacks_test: %s\n", message);
    exit(1);
}

static uint64_t counter(atomic_uint_fast64_t *value)
{
    return atomic_load_explicit(value, memory_order_relaxed);
}

int main(void)
{
    SS4S_Player *player = (SS4S_Player *)(uintptr_t)1;
    ChiakiVideoSampleCallback callback = video_sample_cb;
    (void)callback;

    stats_reset(&g_stream_stats);
    VideoInitResult init_result = VIDEO_INIT_ERROR;
    video_open_result = SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC;
    if (video_init(player, 1920, 1080, 60, CHIAKI_CODEC_H265,
                   &init_result) != NULL ||
        init_result != VIDEO_INIT_UNSUPPORTED_CODEC)
        fail("unsupported video open result was not classified");
    video_open_result = SS4S_VIDEO_OPEN_ERROR;
    if (video_init(player, 1920, 1080, 60, CHIAKI_CODEC_H265,
                   &init_result) != NULL || init_result != VIDEO_INIT_ERROR)
        fail("video open error was not classified");
    video_open_result = SS4S_VIDEO_OPEN_OK;
    VideoContext *video = video_init(player, 1920, 1080, 60,
                                     CHIAKI_CODEC_H265, &init_result);
    if (!video || init_result != VIDEO_INIT_OK)
        fail("video context did not open");

    const uint8_t h265_header[] = {0, 0, 0, 1, (uint8_t)(32u << 1), 1, 2, 3};
    if (!video_sample_cb((uint8_t *)h265_header, sizeof(h265_header),
                         0, false, video))
        fail("accepted codec header was rejected");
    if (counter(&g_stream_stats.video_frames) != 0 ||
        counter(&g_stream_stats.video_last_frame_ms) != 0)
        fail("codec header was incorrectly counted as decoder progress");

    const uint8_t h265_p[] = {0, 0, 0, 1, (uint8_t)(1u << 1), 1, 2, 3};
    if (!video_sample_cb((uint8_t *)h265_p, sizeof(h265_p), 3, true, video))
        fail("accepted video sample was rejected");
    if (last_video_flags & SS4S_VIDEO_FEED_DATA_KEYFRAME)
        fail("recovered H.265 P frame was incorrectly marked as a keyframe");
    if (counter(&g_stream_stats.video_frames_lost) != 3 ||
        counter(&g_stream_stats.video_frames_recovered) != 1 ||
        counter(&g_stream_stats.video_frames) != 1)
        fail("Chiaki loss/recovery metadata was interpreted incorrectly");

    const uint8_t h265_idr[] = {0, 0, 0, 1, (uint8_t)(19u << 1), 1, 2, 3};
    if (!video_sample_cb((uint8_t *)h265_idr, sizeof(h265_idr), 0, false, video) ||
        !(last_video_flags & SS4S_VIDEO_FEED_DATA_KEYFRAME) ||
        counter(&g_stream_stats.video_frames) != 2)
        fail("H.265 IDR was not marked as a keyframe");

    video_feed_result = SS4S_VIDEO_FEED_NOT_READY;
    if (video_sample_cb((uint8_t *)h265_idr, sizeof(h265_idr), 0, false, video) ||
        counter(&g_stream_stats.video_feed_not_ready) != 1)
        fail("video not-ready result was not classified");
    video_feed_result = SS4S_VIDEO_FEED_REQUEST_KEYFRAME;
    if (video_sample_cb((uint8_t *)h265_idr, sizeof(h265_idr), 0, false, video) ||
        counter(&g_stream_stats.video_feed_keyframe_requests) != 1)
        fail("video keyframe request was not classified");
    video_feed_result = SS4S_VIDEO_FEED_ERROR;
    if (video_sample_cb((uint8_t *)h265_idr, sizeof(h265_idr), 0, false, video) ||
        counter(&g_stream_stats.video_feed_errors) != 1)
        fail("video error was not classified");
    video_fini(video);
    if (video_close_count != 1)
        fail("video decoder was not closed");

    stats_reset(&g_stream_stats);
    video_feed_result = SS4S_VIDEO_FEED_OK;
    video = video_init(player, 1920, 1080, 60,
                       CHIAKI_CODEC_H264, &init_result);
    if (!video || init_result != VIDEO_INIT_OK)
        fail("H.264 video context did not open");
    const uint8_t h264_header[] = {0, 0, 0, 1, 0x67, 1, 2, 3};
    video_sample_cb((uint8_t *)h264_header, sizeof(h264_header), 0, false, video);
    if (counter(&g_stream_stats.video_frames) != 0)
        fail("H.264 codec header was counted as a frame");
    const uint8_t h264_p[] = {0, 0, 1, 0x41, 1, 2, 3};
    video_sample_cb((uint8_t *)h264_p, sizeof(h264_p), 0, false, video);
    if ((last_video_flags & SS4S_VIDEO_FEED_DATA_KEYFRAME) ||
        counter(&g_stream_stats.video_frames) != 1)
        fail("H.264 P frame was classified incorrectly");
    const uint8_t h264_idr[] = {0, 0, 1, 0x65, 1, 2, 3};
    video_sample_cb((uint8_t *)h264_idr, sizeof(h264_idr), 0, false, video);
    if (!(last_video_flags & SS4S_VIDEO_FEED_DATA_KEYFRAME) ||
        counter(&g_stream_stats.video_frames) != 2)
        fail("H.264 IDR was classified incorrectly");
    video_fini(video);
    if (video_close_count != 2)
        fail("H.264 video decoder was not closed");

    AudioContext *audio = audio_init(player);
    if (!audio)
        fail("audio context allocation failed");
    ChiakiAudioSink sink = audio_make_sink(audio);
    ChiakiAudioHeader header = {
        .channels = 2,
        .bits = 16,
        .rate = 48000,
        .frame_size = 480,
    };
    sink.header_cb(&header, sink.user);
    sink.header_cb(&header, sink.user);
    if (audio_open_count != 1 || last_audio_info.samplesPerFrame != 480)
        fail("duplicate audio header handling or frame size is wrong");

    unsigned char opus[] = {1, 2, 3, 4};
    audio_feed_result = SS4S_AUDIO_FEED_NOT_READY;
    sink.frame_cb(opus, sizeof(opus), sink.user);
    audio_feed_result = SS4S_AUDIO_FEED_OVERFLOW;
    sink.frame_cb(opus, sizeof(opus), sink.user);
    audio_feed_result = SS4S_AUDIO_FEED_ERROR;
    sink.frame_cb(opus, sizeof(opus), sink.user);
    if (counter(&g_stream_stats.audio_feed_not_ready) != 1 ||
        counter(&g_stream_stats.audio_feed_overflows) != 1 ||
        counter(&g_stream_stats.audio_feed_errors) != 1)
        fail("audio feed outcomes were not classified");

    header.frame_size = 960;
    sink.header_cb(&header, sink.user);
    if (audio_open_count != 2 || audio_close_count != 1)
        fail("audio format change did not close and reopen the decoder");
    audio_fini(audio);
    if (audio_close_count != 2)
        fail("audio decoder was not closed");

    puts("av_callbacks_test: passed");
    return 0;
}
