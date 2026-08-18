#include "audio.h"
#include "stats.h"
#include "app_id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration — defined in main.c, shared via app_log.h */
extern void app_log(const char *fmt, ...);

/* ------------------------------------------------------------------------- */
/* SS4S compatibility layer                                                   */
/* ------------------------------------------------------------------------- */

#ifndef SS4S_OK
#ifdef SS4S_RESULT_OK
#define SS4S_OK SS4S_RESULT_OK
#else
#define SS4S_OK 0
#endif
#endif

/* ------------------------------------------------------------------------- */

struct AudioContext {
    SS4S_Player *player;
    bool         opened;
    int          channels;
};

/* ChiakiAudioSink.header_cb signature:
 *   void (*)(ChiakiAudioHeader *header, void *user)
 */
static void audio_header_cb(ChiakiAudioHeader *header, void *user)
{
    AudioContext *ctx = (AudioContext *)user;

    app_log("[AUDIO/SS4S] Header: rate=%u ch=%u bits=%u frame_size=%u\n",
            header->rate, header->channels, header->bits, header->frame_size);

    ctx->channels = header->channels;  /* kept for reference / future use */

    SS4S_AudioInfo info = {
        .codec         = SS4S_AUDIO_OPUS,
        .numOfChannels = (int)header->channels,
        .sampleRate    = (int)header->rate,
        .appName       = CHIAKI_APP_ID,
    };

    int rc = SS4S_PlayerAudioOpen(ctx->player, &info);
    if (rc == SS4S_OK) {
        ctx->opened = true;
        app_log("[AUDIO/SS4S] PlayerAudioOpen OK  ch=%d rate=%d\n",
                info.numOfChannels, info.sampleRate);
    } else {
        app_log("[AUDIO/SS4S] PlayerAudioOpen FAILED rc=%d\n", rc);
    }
}

/* ChiakiAudioSink.frame_cb — actual typedef: ChiakiAudioSinkFrame
 *   void (*)(unsigned char *buf, unsigned int samples_count, void *user)
 *
 * chiaki passes raw Opus-encoded packets — it does NOT decode to PCM first.
 * samples_count is the BYTE COUNT of the Opus packet (not a PCM sample count).
 * Evidence: audio header reports frame_size=480 (10ms at 48kHz) but
 * samples_count=80 in the callback — 80 is a typical Opus packet byte size,
 * not a sample count.
 *
 * NDL on webOS has native Opus hardware decoding, so we feed raw Opus bytes
 * directly with codec SS4S_AUDIO_OPUS.
 */
static void audio_frame_cb(unsigned char *buf, unsigned int samples_count, void *user)
{
    AudioContext *ctx = (AudioContext *)user;

    if (!ctx->opened || !buf || samples_count == 0) return;

    // Stats overlay counters (callback thread)
    atomic_fetch_add_explicit(&g_stream_stats.audio_bytes, (uint64_t)samples_count, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stream_stats.audio_packets, 1, memory_order_relaxed);

    /* samples_count IS the byte count of the raw Opus packet — feed it directly. */
    SS4S_PlayerAudioFeed(ctx->player, buf, (size_t)samples_count);
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
