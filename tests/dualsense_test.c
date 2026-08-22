#include "dualsense.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* dualsense.c logs through the app logger; protocol tests do not need output. */
void app_log(const char *fmt, ...) { (void)fmt; }
void app_log_always(const char *fmt, ...) { (void)fmt; }

static void test_crc(void)
{
    static const uint8_t vector[] = "123456789";
    assert(dualsense_crc32(vector, sizeof(vector) - 1) == 0xcbf43926u);
}

static void test_report(void)
{
    DualSenseOutputState state;
    memset(&state, 0, sizeof(state));
    state.lightbar_set = 1;
    state.lightbar[0] = 1;
    state.lightbar[1] = 2;
    state.lightbar[2] = 3;
    state.player_leds_set = 1;
    state.player_leds = 0xff;
    state.triggers_owned = 1;
    state.right_trigger[0] = 1;
    state.left_trigger[0] = 2;
    state.intensity_set = 1;
    state.intensity = 0x62;

    uint8_t report[DUALSENSE_REPORT_LEN];
    dualsense_build_report(3, &state, report);
    assert(report[0] == 0x31);
    assert(report[1] == 0x30);
    assert(report[2] == 0x10);
    assert(report[3] == (0x04 | 0x08));
    assert(report[4] == (0x04 | 0x10 | 0x40));
    assert(report[3 + 10] == 1);
    assert(report[3 + 21] == 2);
    assert(report[3 + 36] == 0x62);
    assert(report[3 + 43] == 0x1f);
    assert(report[3 + 44] == 1 && report[3 + 45] == 2 && report[3 + 46] == 3);
    assert((report[3] & 0x01) == 0); /* never fight SDL/evdev rumble */

    uint8_t signed_data[DUALSENSE_REPORT_LEN - 3];
    signed_data[0] = 0xa2;
    memcpy(signed_data + 1, report, DUALSENSE_REPORT_LEN - 4);
    uint32_t crc = dualsense_crc32(signed_data, sizeof(signed_data));
    uint32_t encoded = (uint32_t)report[74] |
                       (uint32_t)report[75] << 8 |
                       (uint32_t)report[76] << 16 |
                       (uint32_t)report[77] << 24;
    assert(encoded == crc);
}

static void test_release_and_payload(void)
{
    DualSenseOutputState released;
    memset(&released, 0, sizeof(released));
    released.triggers_owned = 1;
    uint8_t report[DUALSENSE_REPORT_LEN];
    dualsense_build_report(0, &released, report);
    assert(report[3] == (0x04 | 0x08));
    assert(report[3 + 10] == 0 && report[3 + 21] == 0);

    char payload[512];
    assert(dualsense_build_payload("aa:bb", report, payload, sizeof(payload)));
    assert(strstr(payload, "reportId") == NULL);
    const char *prefix = "{\"address\":\"aa:bb\",\"reportData\":[49,";
    assert(strncmp(payload, prefix, strlen(prefix)) == 0);
    assert(payload[strlen(payload) - 2] == ']');
    assert(payload[strlen(payload) - 1] == '}');
}

static void test_old_webos_bluetooth_address(void)
{
    static const char block[] =
        "I: Bus=0005 Vendor=054c Product=0ce6 Version=0100\n"
        "N: Name=\"DualSense Wireless Controller\"\n"
        "P: Phys=D4:2F:4B:9E:09:57\n"
        "U: Uniq=57:09:9E:4B:2F:D4\n";
    char address[32];
    assert(dualsense_extract_bluetooth_address(block, address,
                                               sizeof(address)));
    assert(strcmp(address, "d4:2f:4b:9e:09:57") == 0);

    static const char fallback[] =
        "N: Name=\"DualSense Wireless Controller\"\n"
        "P: Phys=usb-0000:00:14.0-1/input0\n"
        "U: Uniq=AA:BB:CC:DD:EE:FF\n";
    assert(dualsense_extract_bluetooth_address(fallback, address,
                                               sizeof(address)));
    assert(strcmp(address, "aa:bb:cc:dd:ee:ff") == 0);
    assert(!dualsense_extract_bluetooth_address(block, address, 17));
}

static void test_old_webos_event_path(void)
{
    static const char block[] =
        "I: Bus=0005 Vendor=054c Product=0ce6 Version=0100\n"
        "N: Name=\"DualSense Wireless Controller\"\n"
        "H: Handlers=js5 event41 \n";
    char path[64];
    assert(dualsense_extract_event_path(block, path, sizeof(path)));
    assert(strcmp(path, "/dev/input/event41") == 0);

    static const char sensor[] =
        "N: Name=\"DualSense Wireless Controller Motion Sensors\"\n"
        "H: Handlers=event42 \n";
    assert(!dualsense_extract_event_path(sensor, path, sizeof(path)));

    static const char remote[] =
        "N: Name=\"LGE M-RCU\"\n"
        "H: Handlers=js0 event3 \n";
    assert(!dualsense_extract_event_path(remote, path, sizeof(path)));
    assert(!dualsense_extract_event_path(block, path, 8));
}

int main(void)
{
    test_crc();
    test_report();
    test_release_and_payload();
    test_old_webos_bluetooth_address();
    test_old_webos_event_path();
    puts("DualSense protocol tests passed");
    return 0;
}
