#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ss4s.h>

typedef struct VideoContext VideoContext;

typedef enum VideoInitResult
{
    VIDEO_INIT_OK = 0,
    VIDEO_INIT_UNSUPPORTED_CODEC,
    VIDEO_INIT_ERROR,
} VideoInitResult;

VideoContext *video_init(SS4S_Player *player, int width, int height, int fps,
                         int chiaki_codec, VideoInitResult *result);
void          video_fini(VideoContext *ctx);

/* Matches ChiakiVideoSampleCallback from chiaki-ng 1.10.0 exactly. */
bool video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost,
                     bool frame_recovered, void *user);
