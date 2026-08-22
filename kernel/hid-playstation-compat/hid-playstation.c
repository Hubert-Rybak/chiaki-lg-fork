// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal DualSense compatibility driver for older LG webOS kernels.
 *
 * Linux gained the full hid-playstation driver after the 4.4 kernel used by
 * webOS 5/6 televisions.  This module intentionally keeps the kernel's normal
 * HID descriptor parsing (and therefore the input mapping already exposed by
 * hid-generic), while adding the initialization and FF_RUMBLE output path used
 * by DualSense controllers.
 */

#include <linux/crc32.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "dualsense-input.h"

#define SONY_VENDOR_ID                         0x054c
#define DUALSENSE_PRODUCT_ID                   0x0ce6
#define DUALSENSE_EDGE_PRODUCT_ID              0x0df2
#define HID_PLAYSTATION_VERSION_PATCH          0x8000

#define DS_OUTPUT_REPORT_USB                   0x02
#define DS_OUTPUT_REPORT_USB_SIZE              63
#define DS_OUTPUT_REPORT_BT                    0x31
#define DS_OUTPUT_REPORT_BT_SIZE               78
#define DS_OUTPUT_TAG                          0x10

#define DS_FEATURE_REPORT_CALIBRATION          0x05
#define DS_FEATURE_REPORT_CALIBRATION_SIZE     41
#define DS_FEATURE_REPORT_PAIRING_INFO         0x09
#define DS_FEATURE_REPORT_PAIRING_INFO_SIZE    20
#define DS_FEATURE_REPORT_FIRMWARE_INFO        0x20
#define DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE   64

#define DS_VALID_COMPATIBLE_VIBRATION          0x01
#define DS_VALID_HAPTICS_SELECT                0x02
#define DS_VALID_LIGHTBAR_SETUP                0x02
#define DS_VALID_COMPATIBLE_VIBRATION2         0x04
#define DS_LIGHTBAR_SETUP_LIGHT_OUT            0x02
#define DS_OUTPUT_CRC32_SEED                   0xa2
#define DS_INPUT_CRC32_SEED                    0xa1
#define DS_FEATURE_VERSION(major, minor)        (((major) << 8) | (minor))

#define DS_BUTTONS0_HAT_SWITCH                 0x0f
#define DS_BUTTONS0_SQUARE                     0x10
#define DS_BUTTONS0_CROSS                      0x20
#define DS_BUTTONS0_CIRCLE                     0x40
#define DS_BUTTONS0_TRIANGLE                   0x80
#define DS_BUTTONS1_L1                         0x01
#define DS_BUTTONS1_R1                         0x02
#define DS_BUTTONS1_L2                         0x04
#define DS_BUTTONS1_R2                         0x08
#define DS_BUTTONS1_CREATE                     0x10
#define DS_BUTTONS1_OPTIONS                    0x20
#define DS_BUTTONS1_L3                         0x40
#define DS_BUTTONS1_R3                         0x80
#define DS_BUTTONS2_PS_HOME                    0x01

/* Offsets within the 47-byte common portion of USB/Bluetooth output reports. */
#define DS_COMMON_VALID_FLAG0                  0
#define DS_COMMON_MOTOR_RIGHT                  2
#define DS_COMMON_MOTOR_LEFT                   3
#define DS_COMMON_VALID_FLAG2                  38
#define DS_COMMON_LIGHTBAR_SETUP               41

struct dualsense_compat {
	struct hid_device *hdev;
	struct input_dev *gamepad;
	struct work_struct output_worker;
	spinlock_t lock;
	u8 *output_report;
	u8 output_seq;
	u8 motor_left;
	u8 motor_right;
	bool use_vibration_v2;
	bool opened;
};

static const int dualsense_gamepad_buttons[] = {
	BTN_WEST,
	BTN_NORTH,
	BTN_EAST,
	BTN_SOUTH,
	BTN_TL,
	BTN_TR,
	BTN_TL2,
	BTN_TR2,
	BTN_SELECT,
	BTN_START,
	BTN_THUMBL,
	BTN_THUMBR,
	BTN_MODE,
};

static void dualsense_put_crc(u8 *report, size_t size)
{
	u8 seed = DS_OUTPUT_CRC32_SEED;
	u32 crc;

	crc = crc32_le(0xffffffff, &seed, 1);
	crc = ~crc32_le(crc, report, size - 4);
	report[size - 4] = crc;
	report[size - 3] = crc >> 8;
	report[size - 2] = crc >> 16;
	report[size - 1] = crc >> 24;
}

static bool dualsense_check_input_crc(const u8 *report, size_t size)
{
	u8 seed = DS_INPUT_CRC32_SEED;
	u32 expected;
	u32 crc;

	if (size < 4)
		return false;
	expected = (u32)report[size - 4] |
		   ((u32)report[size - 3] << 8) |
		   ((u32)report[size - 2] << 16) |
		   ((u32)report[size - 1] << 24);
	crc = crc32_le(0xffffffff, &seed, 1);
	crc = ~crc32_le(crc, report, size - 4);
	return crc == expected;
}

static size_t dualsense_prepare_report(struct dualsense_compat *ds, u8 **common)
{
	struct hid_device *hdev = ds->hdev;
	u8 *report = ds->output_report;
	size_t size;

	memset(report, 0, DS_OUTPUT_REPORT_BT_SIZE);
	if (hdev->bus == BUS_BLUETOOTH) {
		size = DS_OUTPUT_REPORT_BT_SIZE;
		report[0] = DS_OUTPUT_REPORT_BT;
		report[1] = (ds->output_seq++ & 0x0f) << 4;
		report[2] = DS_OUTPUT_TAG;
		*common = report + 3;
	} else {
		size = DS_OUTPUT_REPORT_USB_SIZE;
		report[0] = DS_OUTPUT_REPORT_USB;
		*common = report + 1;
	}

	return size;
}

static int dualsense_send_report(struct dualsense_compat *ds, bool reset_lightbar)
{
	u8 *report = ds->output_report;
	u8 *common;
	size_t size;
	u8 left;
	u8 right;
	unsigned long flags;

	size = dualsense_prepare_report(ds, &common);
	if (reset_lightbar) {
		common[DS_COMMON_VALID_FLAG2] |= DS_VALID_LIGHTBAR_SETUP;
		common[DS_COMMON_LIGHTBAR_SETUP] = DS_LIGHTBAR_SETUP_LIGHT_OUT;
	} else {
		spin_lock_irqsave(&ds->lock, flags);
		left = ds->motor_left;
		right = ds->motor_right;
		spin_unlock_irqrestore(&ds->lock, flags);

		common[DS_COMMON_VALID_FLAG0] = DS_VALID_HAPTICS_SELECT;
		if (ds->use_vibration_v2)
			common[DS_COMMON_VALID_FLAG2] |=
				DS_VALID_COMPATIBLE_VIBRATION2;
		else
			common[DS_COMMON_VALID_FLAG0] |=
				DS_VALID_COMPATIBLE_VIBRATION;
		common[DS_COMMON_MOTOR_RIGHT] = right;
		common[DS_COMMON_MOTOR_LEFT] = left;
	}

	if (ds->hdev->bus == BUS_BLUETOOTH)
		dualsense_put_crc(report, size);

	return hid_hw_output_report(ds->hdev, report, size);
}

static void dualsense_output_worker(struct work_struct *work)
{
	struct dualsense_compat *ds =
		container_of(work, struct dualsense_compat, output_worker);
	int ret = dualsense_send_report(ds, false);

	if (ret < 0)
		hid_warn(ds->hdev, "failed to send rumble report: %d\n", ret);
}

static int dualsense_play_effect(struct input_dev *input, void *data,
				 struct ff_effect *effect)
{
	struct dualsense_compat *ds = data;
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	spin_lock_irqsave(&ds->lock, flags);
	ds->motor_left = effect->u.rumble.strong_magnitude / 256;
	ds->motor_right = effect->u.rumble.weak_magnitude / 256;
	spin_unlock_irqrestore(&ds->lock, flags);
	schedule_work(&ds->output_worker);
	return 0;
}

/*
 * The upstream driver retrieves these features before its first output report.
 * Some Bluetooth firmware only accepts advanced output after that handshake.
 * Results are deliberately optional: input and USB rumble remain useful when a
 * vendor Bluetooth service does not implement UHID_GET_REPORT correctly.
 */
static void dualsense_prime_features(struct dualsense_compat *ds)
{
	static const struct {
		u8 id;
		u8 size;
	} features[] = {
		{ DS_FEATURE_REPORT_PAIRING_INFO, DS_FEATURE_REPORT_PAIRING_INFO_SIZE },
		{ DS_FEATURE_REPORT_FIRMWARE_INFO, DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE },
		{ DS_FEATURE_REPORT_CALIBRATION, DS_FEATURE_REPORT_CALIBRATION_SIZE },
	};
	u8 *buf;
	int i;

	buf = kzalloc(DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE, GFP_KERNEL);
	if (!buf)
		return;

	for (i = 0; i < ARRAY_SIZE(features); i++) {
		int ret;

		memset(buf, 0, DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE);
		ret = hid_hw_raw_request(ds->hdev, features[i].id, buf,
					 features[i].size, HID_FEATURE_REPORT,
					 HID_REQ_GET_REPORT);
		if (ret < 0)
			hid_dbg(ds->hdev, "feature 0x%02x unavailable: %d\n",
				features[i].id, ret);
		else if (features[i].id == DS_FEATURE_REPORT_FIRMWARE_INFO &&
			 ret > 45 && ds->hdev->product == DUALSENSE_PRODUCT_ID) {
			u16 update_version = buf[44] | (buf[45] << 8);

			ds->use_vibration_v2 =
				update_version >= DS_FEATURE_VERSION(2, 21);
			hid_dbg(ds->hdev, "feature version %u.%u, vibration v2=%d\n",
				 update_version >> 8, update_version & 0xff,
				 ds->use_vibration_v2);
		}
	}

	kfree(buf);
}

static struct input_dev *dualsense_create_gamepad(struct dualsense_compat *ds)
{
	struct hid_device *hdev = ds->hdev;
	struct input_dev *input;
	int i;
	int ret;

	input = devm_input_allocate_device(&hdev->dev);
	if (!input)
		return ERR_PTR(-ENOMEM);

	input->name = hdev->name;
	input->phys = hdev->phys;
	input->uniq = hdev->uniq;
	input->id.bustype = hdev->bus;
	input->id.vendor = hdev->vendor;
	input->id.product = hdev->product;
	input->id.version = hdev->version;
	input_set_drvdata(input, ds);

	input_set_abs_params(input, ABS_X, 0, 255, 0, 0);
	input->absinfo[ABS_X].value = 128;
	input_set_abs_params(input, ABS_Y, 0, 255, 0, 0);
	input->absinfo[ABS_Y].value = 128;
	input_set_abs_params(input, ABS_Z, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_RX, 0, 255, 0, 0);
	input->absinfo[ABS_RX].value = 128;
	input_set_abs_params(input, ABS_RY, 0, 255, 0, 0);
	input->absinfo[ABS_RY].value = 128;
	input_set_abs_params(input, ABS_RZ, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);

	for (i = 0; i < ARRAY_SIZE(dualsense_gamepad_buttons); i++)
		input_set_capability(input, EV_KEY, dualsense_gamepad_buttons[i]);
	input_set_capability(input, EV_FF, FF_RUMBLE);
	ret = input_ff_create_memless(input, ds, dualsense_play_effect);
	if (ret)
		return ERR_PTR(ret);

	ret = input_register_device(input);
	if (ret)
		return ERR_PTR(ret);
	return input;
}

static int dualsense_raw_event(struct hid_device *hdev,
			       struct hid_report *report, u8 *data, int size)
{
	struct dualsense_compat *ds = hid_get_drvdata(hdev);
	struct dualsense_compat_gamepad_report state;
	bool bluetooth = hdev->bus == BUS_BLUETOOTH;

	if (!ds || !ds->gamepad || !report || size <= 0)
		return 0;
	if (!dualsense_compat_decode_gamepad(bluetooth, report->id, data,
					      size, &state))
		return 0;
	if (bluetooth && report->id == DUALSENSE_INPUT_REPORT_BT &&
	    !dualsense_check_input_crc(data, size)) {
		hid_dbg(hdev, "discarding DualSense input report with invalid CRC\n");
		return -EILSEQ;
	}

	input_report_abs(ds->gamepad, ABS_X, state.left_x);
	input_report_abs(ds->gamepad, ABS_Y, state.left_y);
	input_report_abs(ds->gamepad, ABS_RX, state.right_x);
	input_report_abs(ds->gamepad, ABS_RY, state.right_y);
	input_report_abs(ds->gamepad, ABS_Z, state.left_trigger);
	input_report_abs(ds->gamepad, ABS_RZ, state.right_trigger);
	input_report_abs(ds->gamepad, ABS_HAT0X, state.hat_x);
	input_report_abs(ds->gamepad, ABS_HAT0Y, state.hat_y);

	input_report_key(ds->gamepad, BTN_WEST,
			 state.buttons0 & DS_BUTTONS0_SQUARE);
	input_report_key(ds->gamepad, BTN_SOUTH,
			 state.buttons0 & DS_BUTTONS0_CROSS);
	input_report_key(ds->gamepad, BTN_EAST,
			 state.buttons0 & DS_BUTTONS0_CIRCLE);
	input_report_key(ds->gamepad, BTN_NORTH,
			 state.buttons0 & DS_BUTTONS0_TRIANGLE);
	input_report_key(ds->gamepad, BTN_TL,
			 state.buttons1 & DS_BUTTONS1_L1);
	input_report_key(ds->gamepad, BTN_TR,
			 state.buttons1 & DS_BUTTONS1_R1);
	input_report_key(ds->gamepad, BTN_TL2,
			 state.buttons1 & DS_BUTTONS1_L2);
	input_report_key(ds->gamepad, BTN_TR2,
			 state.buttons1 & DS_BUTTONS1_R2);
	input_report_key(ds->gamepad, BTN_SELECT,
			 state.buttons1 & DS_BUTTONS1_CREATE);
	input_report_key(ds->gamepad, BTN_START,
			 state.buttons1 & DS_BUTTONS1_OPTIONS);
	input_report_key(ds->gamepad, BTN_THUMBL,
			 state.buttons1 & DS_BUTTONS1_L3);
	input_report_key(ds->gamepad, BTN_THUMBR,
			 state.buttons1 & DS_BUTTONS1_R3);
	input_report_key(ds->gamepad, BTN_MODE,
			 state.buttons2 & DS_BUTTONS2_PS_HOME);
	input_sync(ds->gamepad);
	return 0;
}

static int dualsense_probe(struct hid_device *hdev,
			   const struct hid_device_id *id)
{
	struct dualsense_compat *ds;
	int ret;

	ds = devm_kzalloc(&hdev->dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;

	ds->hdev = hdev;
	hdev->version |= HID_PLAYSTATION_VERSION_PATCH;
	/* Current DualSense and all Edge firmware use Sony's vibration-v2 flag.
	 * A successfully retrieved old firmware report can opt back into v1. */
	ds->use_vibration_v2 = true;
	spin_lock_init(&ds->lock);
	INIT_WORK(&ds->output_worker, dualsense_output_worker);
	ds->output_report = devm_kzalloc(&hdev->dev,
					  DS_OUTPUT_REPORT_BT_SIZE, GFP_KERNEL);
	if (!ds->output_report)
		return -ENOMEM;
	hid_set_drvdata(hdev, ds);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "failed to parse report descriptor: %d\n", ret);
		return ret;
	}

	/* Enhanced Bluetooth reports require the PlayStation raw parser. */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "failed to start HID device: %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "failed to open HID device: %d\n", ret);
		goto err_stop;
	}
	ds->opened = true;

	dualsense_prime_features(ds);
	ds->gamepad = dualsense_create_gamepad(ds);
	if (IS_ERR(ds->gamepad)) {
		ret = PTR_ERR(ds->gamepad);
		ds->gamepad = NULL;
		hid_err(hdev, "failed to register DualSense gamepad: %d\n", ret);
		goto err_close;
	}
	ret = dualsense_send_report(ds, true);
	if (ret < 0)
		hid_warn(hdev, "initial output report failed: %d\n", ret);

	hid_info(hdev, "DualSense compatibility driver active\n");
	return 0;

err_close:
	hid_hw_close(hdev);
	ds->opened = false;
err_stop:
	cancel_work_sync(&ds->output_worker);
	hid_hw_stop(hdev);
	return ret;
}

static void dualsense_remove(struct hid_device *hdev)
{
	struct dualsense_compat *ds = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&ds->lock, flags);
	ds->motor_left = 0;
	ds->motor_right = 0;
	spin_unlock_irqrestore(&ds->lock, flags);
	cancel_work_sync(&ds->output_worker);
	if (ds->opened)
		dualsense_send_report(ds, false);
	if (ds->opened)
		hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id dualsense_devices[] = {
	{ HID_BLUETOOTH_DEVICE(SONY_VENDOR_ID, DUALSENSE_PRODUCT_ID) },
	{ HID_USB_DEVICE(SONY_VENDOR_ID, DUALSENSE_PRODUCT_ID) },
	{ HID_BLUETOOTH_DEVICE(SONY_VENDOR_ID, DUALSENSE_EDGE_PRODUCT_ID) },
	{ HID_USB_DEVICE(SONY_VENDOR_ID, DUALSENSE_EDGE_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, dualsense_devices);

static struct hid_driver dualsense_driver = {
	.name = "playstation",
	.id_table = dualsense_devices,
	.probe = dualsense_probe,
	.remove = dualsense_remove,
	.raw_event = dualsense_raw_event,
};

static int __init dualsense_init(void)
{
	return hid_register_driver(&dualsense_driver);
}

static void __exit dualsense_exit(void)
{
	hid_unregister_driver(&dualsense_driver);
}

module_init(dualsense_init);
module_exit(dualsense_exit);

MODULE_AUTHOR("chiaki-lg-fork contributors");
MODULE_DESCRIPTION("DualSense force feedback compatibility for webOS Linux 4.4");
MODULE_LICENSE("GPL");
