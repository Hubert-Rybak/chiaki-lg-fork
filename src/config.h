#pragma once
#include <stdbool.h>

typedef struct {
    char    *host;
    char    *psn_account_id_b64;
    char    *registered_key_b64;
    char    *rp_key_b64;
    int      rp_key_type;
    int      video_width;
    int      video_height;
    int      video_fps;
    int      video_bitrate;
    int      probe_width;       // EXPERIMENTAL: override stream width sent to console (0 = off)
    int      probe_height;      // EXPERIMENTAL: override stream height sent to console (0 = off)
    double   packet_loss_max;   // maximum loss reported to Chiaki congestion control (0.0-1.0)
    bool     idr_on_fec_failure;
    bool     ps5;
    bool     hw_decode;
    char    *video_codec;       // "h265" (default), "h265_hdr" (PS5 HDR/HEVC), or "h264"
    int      audio_volume;
    bool     wakeup;
    char    *ps5_mac;
    int      wakeup_delay_ms;
    bool     sleep_on_exit;
    char    *ss4s_module;
    int      log_level;         // chiaki bitmask: DEBUG=1 VERBOSE=2 INFO=4 WARNING=8 ERROR=16
    // PSN cloud wakeup — needed when PS5 ignores local UDP wakeup (port 987).
    // Copy from: %APPDATA%\Roaming\Chiaki\Chiaki.conf  →  psn_refresh_token = v3.xxxxx
    char    *psn_refresh_token; // long-lived OAuth2 refresh token (optional)
} AppConfig;

int  config_load(AppConfig *cfg, const char *path);
void config_free(AppConfig *cfg);
