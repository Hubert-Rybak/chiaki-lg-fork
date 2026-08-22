/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CHIAKI_DUALSENSE_INPUT_H
#define CHIAKI_DUALSENSE_INPUT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#endif

#define DUALSENSE_INPUT_REPORT_SIMPLE          0x01
#define DUALSENSE_INPUT_REPORT_USB             0x01
#define DUALSENSE_INPUT_REPORT_USB_SIZE        64
#define DUALSENSE_INPUT_REPORT_BT              0x31
#define DUALSENSE_INPUT_REPORT_BT_SIZE         78

struct dualsense_compat_gamepad_report {
	unsigned char left_x;
	unsigned char left_y;
	unsigned char right_x;
	unsigned char right_y;
	unsigned char left_trigger;
	unsigned char right_trigger;
	unsigned char buttons0;
	unsigned char buttons1;
	unsigned char buttons2;
	int hat_x;
	int hat_y;
};

/* Decode only the standardized gamepad portion. Motion, touch coordinates,
 * battery state, and audio controls are deliberately outside this small webOS
 * compatibility driver. */
static inline bool dualsense_compat_decode_gamepad(
	bool bluetooth, unsigned char report_id,
	const unsigned char *data, size_t size,
	struct dualsense_compat_gamepad_report *out)
{
	static const signed char hat[][2] = {
		{ 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
		{ 0,  1 }, {-1,  1 }, {-1, 0 }, {-1,-1 },
		{ 0,  0 },
	};
	const unsigned char *packet;
	unsigned char hat_value;
	bool simple = false;

	if (!data || !out)
		return false;

	if (bluetooth && report_id == DUALSENSE_INPUT_REPORT_BT &&
	    size == DUALSENSE_INPUT_REPORT_BT_SIZE) {
		packet = data + 2;
	} else if (bluetooth && report_id == DUALSENSE_INPUT_REPORT_SIMPLE &&
		   (size == 10 || size == DUALSENSE_INPUT_REPORT_BT_SIZE)) {
		packet = data + 1;
		simple = true;
	} else if (!bluetooth && report_id == DUALSENSE_INPUT_REPORT_USB &&
		   size == DUALSENSE_INPUT_REPORT_USB_SIZE) {
		packet = data + 1;
	} else {
		return false;
	}

	out->left_x = packet[0];
	out->left_y = packet[1];
	out->right_x = packet[2];
	out->right_y = packet[3];
	if (simple) {
		out->buttons0 = packet[4];
		out->buttons1 = packet[5];
		out->buttons2 = packet[6];
		out->left_trigger = packet[7];
		out->right_trigger = packet[8];
	} else {
		out->left_trigger = packet[4];
		out->right_trigger = packet[5];
		out->buttons0 = packet[7];
		out->buttons1 = packet[8];
		out->buttons2 = packet[9];
	}

	hat_value = out->buttons0 & 0x0f;
	if (hat_value > 8)
		hat_value = 8;
	out->hat_x = hat[hat_value][0];
	out->hat_y = hat[hat_value][1];
	return true;
}

#endif
