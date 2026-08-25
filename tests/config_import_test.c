#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Include the implementation so the host-side test needs no webOS libraries. */
#include "../src/config_import.c"

void app_log(const char *fmt, ...)
{
    (void)fmt;
}

void app_log_always(const char *fmt, ...)
{
    (void)fmt;
}

static void fail(const char *message)
{
    fprintf(stderr, "config_import_test: %s\n", message);
    exit(1);
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (!file) fail("could not create fixture");
    if (fputs(text, file) == EOF) fail("could not write fixture");
    if (fclose(file) != 0) fail("could not close fixture");
}

static char *read_text(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) fail("could not open generated config");
    if (fseek(file, 0, SEEK_END) != 0) fail("could not seek generated config");
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0)
        fail("could not size generated config");

    char *text = calloc((size_t)size + 1, 1);
    if (!text) fail("out of memory");
    if (fread(text, 1, (size_t)size, file) != (size_t)size)
        fail("could not read generated config");
    fclose(file);
    return text;
}

static void assert_host(const char *config_path, const char *expected_host)
{
    char *generated = read_text(config_path);
    char expected[320];
    snprintf(expected, sizeof(expected), "\"host\": \"%s\"", expected_host);
    if (!strstr(generated, expected)) {
        free(generated);
        fail("generated config contains the wrong host");
    }
    free(generated);
}

static void assert_safe_wakeup_delay(const char *config_path)
{
    char *generated = read_text(config_path);
    if (!strstr(generated, "\"wakeup_delay_ms\": 60000")) {
        free(generated);
        fail("import did not use the safe wakeup delay");
    }
    free(generated);
}

static void assert_advanced_stream_settings(const char *config_path)
{
    char *generated = read_text(config_path);
    if (!strstr(generated, "\"packet_loss_max\": 0.20") ||
        !strstr(generated, "\"idr_on_fec_failure\": false")) {
        free(generated);
        fail("advanced stream settings were not imported");
    }
    free(generated);
}

static void assert_dualsense_enhanced(const char *config_path, bool expected)
{
    char *generated = read_text(config_path);
    const char *needle = expected
        ? "\"dualsense_bluetooth_enhanced\": true"
        : "\"dualsense_bluetooth_enhanced\": false";
    if (!strstr(generated, needle)) {
        free(generated);
        fail("DualSense Bluetooth safety setting was not preserved");
    }
    free(generated);
}

static void assert_qt_named_escapes(void)
{
    static const uint8_t expected[] = {
        0x07, 0x08, 0x0c, 0x0a, 0x0d, 0x09, 0x0b, 0x22, 0x5c, 0x41,
    };
    uint8_t decoded[sizeof(expected)] = {0};
    int decoded_len = parse_qt_bytearray(
        "@ByteArray(\\a\\b\\f\\n\\r\\t\\v\\\"\\\\\\x41)",
        decoded, sizeof(decoded));
    if (decoded_len != (int)sizeof(expected) ||
        memcmp(decoded, expected, sizeof(expected)) != 0)
        fail("Qt named ByteArray escapes were decoded incorrectly");
}

static void assert_strict_safety_bool(void)
{
    const char *key = "dualsense_bluetooth_enhanced";
    if (!extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\": true}", key, false))
        fail("exact safety opt-in was not accepted");
    if (extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\": false}", key, true))
        fail("exact false safety value was not accepted");
    if (extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\": trueXYZ}", key, false))
        fail("malformed safety opt-in did not fail closed");
    if (extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\": true garbage}", key, false))
        fail("safety opt-in with trailing garbage did not fail closed");
    if (extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\": true} garbage", key, false))
        fail("JSON with trailing garbage did not fail closed");
    if (extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\": true,"
            "\"dualsense_bluetooth_enhanced\": false}", key, false))
        fail("duplicate safety opt-in did not fail closed");
    if (extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\": true,"
            "\"\\u0064ualsense_bluetooth_enhanced\": false}", key, false))
        fail("escaped duplicate safety opt-in did not fail closed");
    if (!extract_unique_json_bool(
            "{\"dualsense_bluetooth_enhanced\":true,"
            "\"note\":\"\\\"dualsense_bluetooth_enhanced\\\": false\"}",
            key, false))
        fail("key-like string content hid a valid top-level safety opt-in");
    if (!extract_unique_json_bool(
            "{\"nested\":{\"dualsense_bluetooth_enhanced\":false},"
            "\"values\":[1,null,\"x\"],"
            "\"dualsense_bluetooth_enhanced\":true}", key, false))
        fail("nested key or unrelated values hid the top-level safety opt-in");
    if (extract_unique_json_bool(
            "{\"nested\":{\"dualsense_bluetooth_enhanced\":true}}",
            key, false))
        fail("nested-only safety key was treated as a top-level opt-in");
    if (extract_unique_json_bool("{}", key, false))
        fail("missing safety opt-in did not default off");
}

static void run_case(const char *dir, const char *name,
                     const char *existing_host,
                     const char *manual_mac,
                     int existing_enhanced,
                     const char *expected_host,
                     ChiakiImportResult expected_result,
                     int verify_reimport)
{
    char ini_path[512];
    char imported_path[544];
    char config_path[512];
    char config[640];
    char ini[4096];
    const char *enhanced_line = existing_enhanced < 0
        ? ""
        : (existing_enhanced
            ? "  \"dualsense_bluetooth_enhanced\": true,\n"
            : "  \"dualsense_bluetooth_enhanced\": false,\n");

    snprintf(ini_path, sizeof(ini_path), "%s/%s.ini", dir, name);
    snprintf(imported_path, sizeof(imported_path), "%s.imported", ini_path);
    snprintf(config_path, sizeof(config_path), "%s/%s.json", dir, name);
    snprintf(config, sizeof(config),
             "{\n  \"host\": \"%s\",\n  \"video_width\": 1920,\n"
             "  \"video_height\": 1080,\n  \"video_fps\": 60,\n"
             "%s"
             "  \"packet_loss_max\": 0.15,\n"
             "  \"idr_on_fec_failure\": true,\n"
             "  \"psn_refresh_token\": \"\"\n}\n",
             existing_host, enhanced_line);
    snprintf(ini, sizeof(ini),
             "[registered_hosts]\n"
             "1\\target=1000100\n"
             "1\\server_mac=@ByteArray(\\x01\\x02\\x03\\x04\\x05\\x06)\n"
             "1\\rp_regist_key=@ByteArray(12345678\\0\\0\\0\\0\\0\\0\\0\\0)\n"
             "1\\rp_key=@ByteArray(abcdefghijklmnop)\n"
             "1\\rp_key_type=2\n"
             "size=1\n"
             "[manual_hosts]\n"
             "1\\host=192.168.50.20\n"
             "1\\registered=true\n"
             "1\\registered_mac=%s\n"
             "size=1\n"
             "[settings]\n"
             "psn_account_id=AAAAAAAAAAA=\n"
             "packet_loss_reported_max=0.20\n"
             "idr_on_fec_failure=false\n",
             manual_mac);

    write_text(config_path, config);
    write_text(ini_path, ini);

    if (config_try_import_chiaki_ini(ini_path, config_path) != expected_result)
        fail("import returned the wrong result");
    assert_host(config_path, expected_host);
    assert_safe_wakeup_delay(config_path);
    assert_advanced_stream_settings(config_path);
    assert_dualsense_enhanced(config_path, existing_enhanced > 0);

    if (verify_reimport) {
        /* The TV keeps the export as .imported after the first successful run. */
        write_text(config_path, config);
        if (config_try_import_chiaki_ini(ini_path, config_path) != expected_result)
            fail(".imported fallback returned the wrong result");
        assert_host(config_path, expected_host);
        assert_safe_wakeup_delay(config_path);
        assert_advanced_stream_settings(config_path);
        assert_dualsense_enhanced(config_path, existing_enhanced > 0);
    }

    unlink(imported_path);
    unlink(config_path);
}

int main(void)
{
    assert_qt_named_escapes();
    assert_strict_safety_bool();

    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/chiaki-config-import-test-%ld", (long)getpid());
    if (mkdir(dir, 0700) != 0) fail("could not create temporary directory");

    const char *matching_mac =
        "@ByteArray(\\x01\\x02\\x03\\x04\\x05\\x06)";
    const char *different_mac =
        "@ByteArray(\\x01\\x02\\x03\\x04\\x05\\x07)";

    run_case(dir, "matching-overrides", "203.0.113.10",
             matching_mac, 1, "192.168.50.20", CI_SUCCESS, 1);
    run_case(dir, "matching-fills-empty", "",
             matching_mac, -1, "192.168.50.20", CI_SUCCESS, 0);
    run_case(dir, "mismatch-preserves", "192.168.50.99",
             different_mac, 0, "192.168.50.99", CI_SUCCESS, 0);
    run_case(dir, "mismatch-needs-host", "",
             different_mac, -1, "", CI_SUCCESS_NEEDS_HOST, 0);

    if (rmdir(dir) != 0) fail("could not remove temporary directory");
    puts("config_import_test: passed");
    return 0;
}
