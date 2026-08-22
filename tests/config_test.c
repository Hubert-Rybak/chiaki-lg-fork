#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static void fail(const char *message)
{
    fprintf(stderr, "config_test: %s\n", message);
    exit(1);
}

static void write_config(const char *path, const char *json)
{
    FILE *file = fopen(path, "w");
    if (!file || fputs(json, file) == EOF || fclose(file) != 0)
        fail("could not write fixture");
}

int main(void)
{
    char path[] = "/tmp/chiaki-config-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        fail("mkstemp failed");
    close(fd);

    write_config(path,
        "{\n"
        "  \"host\": \"192.0.2.1\",\n"
        "  \"video_width\": 3840,\n"
        "  \"video_height\": 2160,\n"
        "  \"video_fps\": 120,\n"
        "  \"video_bitrate\": 1,\n"
        "  \"video_codec\": \"av1\",\n"
        "  \"packet_loss_max\": \"bad\",\n"
        "  \"idr_on_fec_failure\": \"true\"\n"
        "}\n");

    AppConfig config;
    if (config_load(&config, path) != 0)
        fail("invalid-values fixture did not load");
    if (config.video_width != 1920 || config.video_height != 1080)
        fail("oversized resolution was not normalized");
    if (config.video_fps != 60 || config.video_bitrate != 15000)
        fail("invalid video values were not normalized");
    if (!config.video_codec || strcmp(config.video_codec, "h265"))
        fail("invalid codec was not normalized");
    if (fabs(config.packet_loss_max - 0.05) > 0.0001 ||
        !config.idr_on_fec_failure)
        fail("invalid advanced settings did not use balanced defaults");
    config_free(&config);

    write_config(path,
        "{\n"
        "  \"host\": \"192.0.2.1\",\n"
        "  \"video_width\": 1280,\n"
        "  \"video_height\": 720,\n"
        "  \"packet_loss_max\": 0.25,\n"
        "  \"idr_on_fec_failure\": false\n"
        "}\n");
    if (config_load(&config, path) != 0)
        fail("valid-values fixture did not load");
    if (fabs(config.packet_loss_max - 0.25) > 0.0001 ||
        config.idr_on_fec_failure)
        fail("valid advanced settings were not retained");
    config_free(&config);

    write_config(path,
        "{\n"
        "  \"host\": \"192.0.2.1\",\n"
        "  \"video_width\": 1280,\n"
        "  \"video_height\": 720,\n"
        "  \"probe_width\": 2560,\n"
        "  \"probe_height\": 1440\n"
        "}\n");
    if (config_load(&config, path) != 0)
        fail("probe-values fixture did not load");
    if (config.video_width != 2560 || config.video_height != 1440)
        fail("experimental probe keys did not override stream resolution");
    if (config.probe_width != 2560 || config.probe_height != 1440)
        fail("probe keys were not retained on the config struct");
    config_free(&config);

    unlink(path);
    puts("config_test: passed");
    return 0;
}
