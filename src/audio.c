#include "audio.h"
#include "stats.h"
#include "app_id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration — defined in main.c, shared via app_log.h */
extern void app_log(const char *fmt, ...);

struct AudioContext {
    SS4S_Player *player;
    bool         opened;
    int          channels;
    int          sample_rate;
    int          samples_per_frame;
    uint64_t     feed_count;
};

/* ChiakiAudioSink.header_cb signature:
 *   void (*)(ChiakiAudioHeader *header, void *user)
 */
static void audio_header_cb(ChiakiAudioHeader *header, void *user)
{
    AudioContext *ctx = (AudioContext *)user;

    if (!ctx || !header)
        return;

    app_log("[AUDIO/SS4S] Header: rate=%u ch=%u bits=%u frame_size=%u\n",
            header->rate, header->channels, header->bits, header->frame_size);

    if (ctx->opened && ctx->channels == (int)header->channels &&
        ctx->sample_rate == (int)header->rate &&
        ctx->samples_per_frame == (int)header->frame_size)
    {
        app_log("[AUDIO/SS4S] Duplicate audio header ignored\n");
        return;
    }

    if (ctx->opened)
    {
        app_log("[AUDIO/SS4S] Audio format changed; reopening decoder\n");
        SS4S_PlayerAudioClose(ctx->player);
        ctx->opened = false;
    }

    ctx->channels = header->channels;
    ctx->sample_rate = (int)header->rate;
    ctx->samples_per_frame = (int)header->frame_size;

    SS4S_AudioInfo info = {
        .codec         = SS4S_AUDIO_OPUS,
        .numOfChannels = (int)header->channels,
        .sampleRate    = (int)header->rate,
        .samplesPerFrame = (int)header->frame_size,
        .appName       = CHIAKI_APP_ID,
        .streamName    = "Remote Play",
    };

    SS4S_AudioOpenResult rc = SS4S_PlayerAudioOpen(ctx->player, &info);
    if (rc == SS4S_AUDIO_OPEN_OK) {
        ctx->opened = true;
        app_log("[AUDIO/SS4S] PlayerAudioOpen OK ch=%d rate=%d frame=%d\n",
                info.numOfChannels, info.sampleRate, info.samplesPerFrame);
    } else {
        app_log("[AUDIO/SS4S] PlayerAudioOpen FAILED rc=%d\n", rc);
        atomic_fetch_add_explicit(&g_stream_stats.audio_feed_errors, 1,
                                  memory_order_relaxed);
    }
}

/* ChiakiAudioSink.frame_cb — chiaki-ng 1.10.0 typedef: ChiakiAudioSinkFrame
 *   void (*)(unsigned char *buf, size_t buf_size, void *user)
 *
 * chiaki passes raw Opus-encoded packets — it does NOT decode to PCM first.
 * buf_size is the byte count of the Opus packet (not a PCM sample count).
 * Evidence: audio header reports frame_size=480 (10ms at 48kHz) but
 * buf_size=80 in the callback — 80 is a typical Opus packet byte size,
 * not a sample count.
 *
 * NDL on webOS has native Opus hardware decoding, so we feed raw Opus bytes
 * directly with codec SS4S_AUDIO_OPUS.
 */
static void audio_frame_cb(unsigned char *buf, size_t buf_size, void *user)
{
    AudioContext *ctx = (AudioContext *)user;

    if (!ctx || !ctx->opened || !buf || buf_size == 0) return;

    ctx->feed_count++;
    atomic_fetch_add_explicit(&g_stream_stats.audio_bytes, (uint64_t)buf_size, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stream_stats.audio_packets, 1, memory_order_relaxed);

    SS4S_AudioFeedResult rc =
        SS4S_PlayerAudioFeed(ctx->player, buf, buf_size);
    switch (rc)
    {
    case SS4S_AUDIO_FEED_OK:
        return;
    case SS4S_AUDIO_FEED_NOT_READY:
        atomic_fetch_add_explicit(&g_stream_stats.audio_feed_not_ready, 1,
                                  memory_order_relaxed);
        break;
    case SS4S_AUDIO_FEED_OVERFLOW:
        atomic_fetch_add_explicit(&g_stream_stats.audio_feed_overflows, 1,
                                  memory_order_relaxed);
        break;
    case SS4S_AUDIO_FEED_ERROR:
    default:
        atomic_fetch_add_explicit(&g_stream_stats.audio_feed_errors, 1,
                                  memory_order_relaxed);
        break;
    }

    if (ctx->feed_count <= 5 || ctx->feed_count % 1000 == 0)
        app_log("[AUDIO/SS4S] PlayerAudioFeed rejected packet rc=%d\n", (int)rc);
}

AudioContext *audio_init(SS4S_Player *player)
{
    AudioContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->player = player;
    return ctx;
}

void audio_fini(AudioContext *ctx)
{
    if (!ctx) return;
    if (ctx->opened) SS4S_PlayerAudioClose(ctx->player);
    free(ctx);
}

ChiakiAudioSink audio_make_sink(AudioContext *ctx)
{
    ChiakiAudioSink sink;
    memset(&sink, 0, sizeof(sink));
    sink.header_cb = audio_header_cb;
    sink.frame_cb  = audio_frame_cb;
    sink.user      = ctx;
    return sink;
}
