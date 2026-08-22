#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <json-c/json.h>
#include <sys/stat.h>

static char *json_get_str(struct json_object *obj, const char *key)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val)) return NULL;
    const char *s = json_object_get_string(val);
    return s ? strdup(s) : NULL;
}

static int json_get_int(struct json_object *obj, const char *key, int def)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val)) return def;
    return json_object_get_int(val);
}

static bool json_get_bool(struct json_object *obj, const char *key, bool def)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val)) return def;
    return json_object_get_boolean(val);
}

static bool json_get_bool_strict(struct json_object *obj, const char *key, bool def)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val))
        return def;
    if (json_object_get_type(val) != json_type_boolean) {
        fprintf(stderr, "[CONFIG] Warning: %s must be a boolean; using %s\n",
                key, def ? "true" : "false");
        return def;
    }
    return json_object_get_boolean(val);
}

static double json_get_double_range(struct json_object *obj, const char *key,
                                    double def, double min, double max)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val))
        return def;

    enum json_type type = json_object_get_type(val);
    if (type != json_type_double && type != json_type_int) {
        fprintf(stderr, "[CONFIG] Warning: %s must be numeric; using %.2f\n",
                key, def);
        return def;
    }

    double value = json_object_get_double(val);
    if (!isfinite(value) || value < min || value > max) {
        fprintf(stderr,
                "[CONFIG] Warning: %s must be between %.2f and %.2f; using %.2f\n",
                key, min, max, def);
        return def;
    }
    return value;
}

static int config_write_defaults(const char *path)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f,
        "{\n"
        "  \"host\": \"\",\n"
        "  \"ps5\": true,\n"
        "  \"video_width\": 1920,\n"
        "  \"video_height\": 1080,\n"
        "  \"video_fps\": 60,\n"
        "  \"video_bitrate\": 15000,\n"
        "  \"video_codec\": \"h265\",\n"
        "  \"packet_loss_max\": 0.05,\n"
        "  \"idr_on_fec_failure\": true,\n"
        "  \"audio_volume\": 100,\n"
        "  \"wakeup\": true,\n"
        "  \"ps5_mac\": \"\",\n"
        "  \"wakeup_delay_ms\": 60000,\n"
        "  \"sleep_on_exit\": true,\n"
        "  \"log_level\": \"warning\",\n"
        "  \"psn_refresh_token\": \"\"\n"
        "}\n");
    fclose(f);
    fprintf(stderr, "[CONFIG] Created default config at %s\n", path);
    return 0;
}

int config_load(AppConfig *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));

    struct json_object *root = json_object_from_file(path);
    if (!root)
    {
        fprintf(stderr, "[CONFIG] Config not found at %s — creating defaults\n", path);
        if (config_write_defaults(path) != 0) {
            fprintf(stderr, "[CONFIG] Could not create config directory at %s\n", path);
            return -1;
        }
        root = json_object_from_file(path);
        if (!root) {
            fprintf(stderr, "[CONFIG] Still cannot parse %s after creation\n", path);
            return -1;
        }
    }

    cfg->host               = json_get_str(root, "host");
    cfg->psn_account_id_b64 = json_get_str(root, "psn_account_id");
    cfg->registered_key_b64 = json_get_str(root, "registered_key");
    cfg->rp_key_b64         = json_get_str(root, "rp_key");

    cfg->rp_key_type      = json_get_int(root,  "rp_key_type",      0);
    cfg->video_width      = json_get_int(root,  "video_width",      1920);
    cfg->video_height     = json_get_int(root,  "video_height",     1080);
    cfg->video_fps        = json_get_int(root,  "video_fps",        60);
    cfg->video_bitrate    = json_get_int(root,  "video_bitrate",    15000);
    cfg->probe_width      = json_get_int(root,  "probe_width",      0);
    cfg->probe_height     = json_get_int(root,  "probe_height",     0);
    cfg->packet_loss_max  = json_get_double_range(root, "packet_loss_max",
                                                   0.05, 0.0, 1.0);
    cfg->idr_on_fec_failure = json_get_bool_strict(root, "idr_on_fec_failure", true);
    cfg->audio_volume     = json_get_int(root,  "audio_volume",     100);
    cfg->ps5              = json_get_bool(root, "ps5",              true);
    cfg->hw_decode        = json_get_bool(root, "hw_decode",        false);
    cfg->video_codec      = json_get_str(root,  "video_codec");
    cfg->wakeup           = json_get_bool(root, "wakeup",           true);
    cfg->ps5_mac          = json_get_str(root,  "ps5_mac");
    cfg->wakeup_delay_ms  = json_get_int(root,  "wakeup_delay_ms",  60000);
    cfg->sleep_on_exit    = json_get_bool(root, "sleep_on_exit",    true);

    if (cfg->video_width <= 0 || cfg->video_height <= 0) {
        fprintf(stderr, "[CONFIG] Warning: invalid video dimensions; using 1920x1080\n");
        cfg->video_width = 1920;
        cfg->video_height = 1080;
    } else if (cfg->video_width > 1920 || cfg->video_height > 1080) {
        fprintf(stderr,
                "[CONFIG] Warning: %dx%d exceeds Chiaki's 1080p ceiling; using 1920x1080\n",
                cfg->video_width, cfg->video_height);
        cfg->video_width = 1920;
        cfg->video_height = 1080;
    }

    if (cfg->probe_width > 0 && cfg->probe_height > 0 &&
        (cfg->probe_width != cfg->video_width ||
         cfg->probe_height != cfg->video_height)) {
        fprintf(stderr,
                "[CONFIG] EXPERIMENTAL resolution probe: overriding %dx%d with %dx%d "
                "(PS5 may clamp or refuse; remove probe_width/probe_height to restore)\n",
                cfg->video_width, cfg->video_height,
                cfg->probe_width, cfg->probe_height);
        cfg->video_width = cfg->probe_width;
        cfg->video_height = cfg->probe_height;
    }
    if (cfg->video_fps != 30 && cfg->video_fps != 60) {
        fprintf(stderr, "[CONFIG] Warning: video_fps must be 30 or 60; using 60\n");
        cfg->video_fps = 60;
    }
    if (cfg->video_bitrate < 2000 || cfg->video_bitrate > 100000) {
        fprintf(stderr,
                "[CONFIG] Warning: video_bitrate must be 2000-100000 kbps; using 15000\n");
        cfg->video_bitrate = 15000;
    }

    if (!cfg->video_codec ||
        (strcmp(cfg->video_codec, "h264") != 0 &&
         strcmp(cfg->video_codec, "h265") != 0 &&
         strcmp(cfg->video_codec, "h265_hdr") != 0))
    {
        if (cfg->video_codec)
            fprintf(stderr, "[CONFIG] Warning: unknown video_codec '%s'; using h265\n",
                    cfg->video_codec);
        free(cfg->video_codec);
        cfg->video_codec = strdup("h265");
    }

    /* Early fork imports wrote 30 seconds even though the application default
     * and documentation use 60. Keep those existing installations reliable
     * without rewriting their config file. */
    if (cfg->wakeup_delay_ms == 30000)
        cfg->wakeup_delay_ms = 60000;

    // log_level: string or legacy bool
    {
        struct json_object *lv;
        if (json_object_object_get_ex(root, "log_level", &lv))
        {
            if (json_object_get_type(lv) == json_type_boolean) {
                cfg->log_level = json_object_get_boolean(lv) ? (8|16) : 0;
            } else {
                const char *s = json_object_get_string(lv);
                if      (!s || !strcmp(s,"off"))     cfg->log_level = 0;
                else if (!strcmp(s,"error"))         cfg->log_level = 16;
                else if (!strcmp(s,"warning"))       cfg->log_level = 16|8;
                else if (!strcmp(s,"info"))          cfg->log_level = 16|8|4;
                else if (!strcmp(s,"verbose"))       cfg->log_level = 16|8|4|2;
                else if (!strcmp(s,"debug"))         cfg->log_level = 16|8|4|2|1;
                else                                 cfg->log_level = 16|8;
            }
        }
        else if (json_object_object_get_ex(root, "logging", &lv))
            cfg->log_level = json_object_get_boolean(lv) ? (8|16) : 0;
        else
            cfg->log_level = 16|8;
    }

    cfg->ss4s_module = json_get_str(root, "ss4s_module");
    if (!cfg->ss4s_module || !cfg->ss4s_module[0]) {
        free(cfg->ss4s_module);
        // "auto" triggers runtime webOS version detection in main.c.
        // Users can override by setting ss4s_module explicitly in config.json
        // ("ndl-webos5" or "ndl-webos4").
        cfg->ss4s_module = strdup("auto");
    }

    // PSN cloud wakeup token — treat empty string as not set
    cfg->psn_refresh_token = json_get_str(root, "psn_refresh_token");
    if (cfg->psn_refresh_token && !cfg->psn_refresh_token[0]) {
        free(cfg->psn_refresh_token);
        cfg->psn_refresh_token = NULL;
    }

    if (cfg->wakeup && (!cfg->host || !cfg->host[0])) {
        fprintf(stderr, "[CONFIG] Warning: wakeup=true but host not set — wakeup disabled\n");
        cfg->wakeup = false;
    }

    json_object_put(root);

    fprintf(stderr, "[CONFIG] Loaded: host=%s ps5=%d %dx%d@%dfps %dkbps "
            "loss_cap=%.2f idr_on_fec=%d wakeup=%d psn_wakeup=%s "
            "sleep_on_exit=%d log_level=0x%x\n",
            cfg->host ? cfg->host : "(null)", cfg->ps5,
            cfg->video_width, cfg->video_height,
            cfg->video_fps, cfg->video_bitrate,
            cfg->packet_loss_max, cfg->idr_on_fec_failure,
            cfg->wakeup,
            cfg->psn_refresh_token ? "YES" : "NO",
            cfg->sleep_on_exit, cfg->log_level);
    return 0;
}

void config_free(AppConfig *cfg)
{
    free(cfg->host);
    free(cfg->psn_account_id_b64);
    free(cfg->registered_key_b64);
    free(cfg->rp_key_b64);
    free(cfg->ps5_mac);
    free(cfg->ss4s_module);
    free(cfg->video_codec);
    free(cfg->psn_refresh_token);
    memset(cfg, 0, sizeof(*cfg));
}
