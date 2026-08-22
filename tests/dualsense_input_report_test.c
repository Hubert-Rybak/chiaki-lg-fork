#include "dualsense-input.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_bluetooth_enhanced_report(void)
{
    unsigned char report[DUALSENSE_INPUT_REPORT_BT_SIZE] = {0};
    struct dualsense_compat_gamepad_report state;
    unsigned char *packet = report + 2;

    report[0] = DUALSENSE_INPUT_REPORT_BT;
    packet[0] = 11;
    packet[1] = 12;
    packet[2] = 13;
    packet[3] = 14;
    packet[4] = 15;
    packet[5] = 16;
    packet[7] = 0x20 | 0x06; /* Cross + left on the hat. */
    packet[8] = 0x01 | 0x20; /* L1 + Options. */
    packet[9] = 0x01;        /* PS. */

    assert(dualsense_compat_decode_gamepad(
        true, DUALSENSE_INPUT_REPORT_BT, report, sizeof(report), &state));
    assert(state.left_x == 11 && state.left_y == 12);
    assert(state.right_x == 13 && state.right_y == 14);
    assert(state.left_trigger == 15 && state.right_trigger == 16);
    assert(state.buttons0 == (0x20 | 0x06));
    assert(state.buttons1 == (0x01 | 0x20));
    assert(state.buttons2 == 0x01);
    assert(state.hat_x == -1 && state.hat_y == 0);
}

static void test_bluetooth_simple_report(void)
{
    unsigned char report[10] = {0};
    struct dualsense_compat_gamepad_report state;
    unsigned char *packet = report + 1;

    report[0] = DUALSENSE_INPUT_REPORT_SIMPLE;
    packet[0] = 21;
    packet[1] = 22;
    packet[2] = 23;
    packet[3] = 24;
    packet[4] = 0x80 | 0x01; /* Triangle + up-right on the hat. */
    packet[5] = 0x10;
    packet[6] = 0x01;
    packet[7] = 25;
    packet[8] = 26;

    assert(dualsense_compat_decode_gamepad(
        true, DUALSENSE_INPUT_REPORT_SIMPLE, report, sizeof(report), &state));
    assert(state.left_x == 21 && state.left_y == 22);
    assert(state.right_x == 23 && state.right_y == 24);
    assert(state.left_trigger == 25 && state.right_trigger == 26);
    assert(state.buttons0 == (0x80 | 0x01));
    assert(state.buttons1 == 0x10 && state.buttons2 == 0x01);
    assert(state.hat_x == 1 && state.hat_y == -1);
}

static void test_usb_and_invalid_reports(void)
{
    unsigned char report[DUALSENSE_INPUT_REPORT_USB_SIZE] = {0};
    struct dualsense_compat_gamepad_report state;
    unsigned char *packet = report + 1;

    report[0] = DUALSENSE_INPUT_REPORT_USB;
    packet[7] = 0x0f; /* Invalid hat values must become centered. */
    assert(dualsense_compat_decode_gamepad(
        false, DUALSENSE_INPUT_REPORT_USB, report, sizeof(report), &state));
    assert(state.hat_x == 0 && state.hat_y == 0);

    assert(!dualsense_compat_decode_gamepad(
        true, DUALSENSE_INPUT_REPORT_BT, report, sizeof(report), &state));
    assert(!dualsense_compat_decode_gamepad(
        false, 0x31, report, sizeof(report), &state));
    assert(!dualsense_compat_decode_gamepad(
        false, DUALSENSE_INPUT_REPORT_USB, NULL, sizeof(report), &state));
    assert(!dualsense_compat_decode_gamepad(
        false, DUALSENSE_INPUT_REPORT_USB, report, sizeof(report), NULL));
}

int main(void)
{
    test_bluetooth_enhanced_report();
    test_bluetooth_simple_report();
    test_usb_and_invalid_reports();
    puts("DualSense input report tests passed");
    return 0;
}
