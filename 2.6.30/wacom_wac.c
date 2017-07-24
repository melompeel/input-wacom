/*
 * drivers/input/tablet/wacom_wac.c
 *
 *  USB Wacom tablet support - Wacom specific code
 *
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "wacom_wac.h"
#include "wacom.h"
#include <linux/hid.h>

# ifndef LINUX_VERSION_CODE
# include <linux/version.h>
# endif

/* Newer Cintiq and DTU have an offset between tablet and screen areas */
#define WACOM_DTU_OFFSET       200
#define WACOM_CINTIQ_OFFSET    400

static void wacom_report_numbered_buttons(struct input_dev *input_dev,
				int button_count, int mask);

static int find_slot_from_contactid(struct wacom_wac *wacom, int contactid);

static int wacom_penpartner_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	switch (data[0]) {
	case 1:
		if (data[5] & 0x80) {
			wacom->tool[0] = (data[5] & 0x20) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
			wacom->id[0] = (data[5] & 0x20) ? ERASER_DEVICE_ID : STYLUS_DEVICE_ID;
			input_report_key(input, wacom->tool[0], 1);
			input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
			input_report_abs(input, ABS_X, get_unaligned_le16(&data[1]));
			input_report_abs(input, ABS_Y, get_unaligned_le16(&data[3]));
			input_report_abs(input, ABS_PRESSURE, (signed char)data[6] + 127);
			input_report_key(input, BTN_TOUCH, ((signed char)data[6] > -127));
			input_report_key(input, BTN_STYLUS, (data[5] & 0x40));
		} else {
			input_report_key(input, wacom->tool[0], 0);
			input_report_abs(input, ABS_MISC, 0); /* report tool id */
			input_report_abs(input, ABS_PRESSURE, -1);
			input_report_key(input, BTN_TOUCH, 0);
		}
		break;

	case 2:
		input_report_key(input, BTN_TOOL_PEN, 1);
		input_report_abs(input, ABS_MISC, STYLUS_DEVICE_ID); /* report tool id */
		input_report_abs(input, ABS_X, get_unaligned_le16(&data[1]));
		input_report_abs(input, ABS_Y, get_unaligned_le16(&data[3]));
		input_report_abs(input, ABS_PRESSURE, (signed char)data[6] + 127);
		input_report_key(input, BTN_TOUCH, ((signed char)data[6] > -80) && !(data[5] & 0x20));
		input_report_key(input, BTN_STYLUS, (data[5] & 0x40));
		break;

	default:
		printk(KERN_INFO "wacom_penpartner_irq: received unknown report #%d\n", data[0]);
		return 0;
        }

	return 1;
}
static int wacom_dtu_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int prox = data[1] & 0x20;

	dbg("wacom_dtu_irq: received report #%d", data[0]);

	if (prox) {
		/* Going into proximity select tool */
		wacom->tool[0] = (data[1] & 0x0c) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
		if (wacom->tool[0] == BTN_TOOL_PEN)
			wacom->id[0] = STYLUS_DEVICE_ID;
		else
			wacom->id[0] = ERASER_DEVICE_ID;
	}
	input_report_key(input, BTN_STYLUS, data[1] & 0x02);
	input_report_key(input, BTN_STYLUS2, data[1] & 0x10);
	input_report_abs(input, ABS_PRESSURE, ((data[7] & 0x01) << 8) | data[6]);
	input_report_key(input, BTN_TOUCH, data[1] & 0x05);
	if (!prox) { /* out-prox */
		wacom->id[0] = 0;
		input_report_abs(input, ABS_X, 0);
		input_report_abs(input, ABS_Y, 0);
	} else {
		input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
		input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
	}
	input_report_key(input, wacom->tool[0], prox);
	input_report_abs(input, ABS_MISC, wacom->id[0]);
	return 1;
}

static int wacom_pl_irq(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int prox, pressure;

	if (data[0] != WACOM_REPORT_PENABLED) {
		dbg("wacom_pl_irq: received unknown report #%d", data[0]);
		return 0;
	}

	prox = data[1] & 0x40;

	if (prox) {
		wacom->id[0] = ERASER_DEVICE_ID;
		pressure = (signed char)((data[7] << 1) | ((data[4] >> 2) & 1));
		if (features->pressure_max > 255)
			pressure = (pressure << 1) | ((data[4] >> 6) & 1);
		pressure += (features->pressure_max + 1) / 2;

		/*
		 * if going from out of proximity into proximity select between the eraser
		 * and the pen based on the state of the stylus2 button, choose eraser if
		 * pressed else choose pen. if not a proximity change from out to in, send
		 * an out of proximity for previous tool then a in for new tool.
		 */
		if (!wacom->tool[0]) {
			/* Eraser bit set for DTF */
			if (data[1] & 0x10)
				wacom->tool[1] = BTN_TOOL_RUBBER;
			else
				/* Going into proximity select tool */
				wacom->tool[1] = (data[4] & 0x20) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
		} else {
			/* was entered with stylus2 pressed */
			if (wacom->tool[1] == BTN_TOOL_RUBBER && !(data[4] & 0x20)) {
				/* report out proximity for previous tool */
				input_report_key(input, wacom->tool[1], 0);
				input_sync(input);
				wacom->tool[1] = BTN_TOOL_PEN;
				return 0;
			}
		}
		if (wacom->tool[1] != BTN_TOOL_RUBBER) {
			/* Unknown tool selected default to pen tool */
			wacom->tool[1] = BTN_TOOL_PEN;
			wacom->id[0] = STYLUS_DEVICE_ID;
		}
		input_report_key(input, wacom->tool[1], prox); /* report in proximity for tool */
		input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
		input_report_abs(input, ABS_X, data[3] | (data[2] << 7) | ((data[1] & 0x03) << 14));
		input_report_abs(input, ABS_Y, data[6] | (data[5] << 7) | ((data[4] & 0x03) << 14));
		input_report_abs(input, ABS_PRESSURE, pressure);

		input_report_key(input, BTN_TOUCH, data[4] & 0x08);
		input_report_key(input, BTN_STYLUS, data[4] & 0x10);
		/* Only allow the stylus2 button to be reported for the pen tool. */
		input_report_key(input, BTN_STYLUS2, (wacom->tool[1] == BTN_TOOL_PEN) && (data[4] & 0x20));
	} else {
		/* report proximity-out of a (valid) tool */
		if (wacom->tool[1] != BTN_TOOL_RUBBER) {
			/* Unknown tool selected default to pen tool */
			wacom->tool[1] = BTN_TOOL_PEN;
		}
		input_report_abs(input, ABS_X, 0);
		input_report_abs(input, ABS_Y, 0);
		input_report_key(input, wacom->tool[1], prox);
	}

	wacom->tool[0] = prox; /* Save proximity state */
	return 1;
}

static int wacom_ptu_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	if (data[0] != WACOM_REPORT_PENABLED) {
		printk(KERN_INFO "wacom_ptu_irq: received unknown report #%d\n", data[0]);
		return 0;
	}

	if (data[1] & 0x04) {
		input_report_key(input, BTN_TOOL_RUBBER, data[1] & 0x20);
		input_report_key(input, BTN_TOUCH, data[1] & 0x08);
		wacom->id[0] = ERASER_DEVICE_ID;
	} else {
		input_report_key(input, BTN_TOOL_PEN, data[1] & 0x20);
		input_report_key(input, BTN_TOUCH, data[1] & 0x01);
		wacom->id[0] = STYLUS_DEVICE_ID;
	}
	input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
	input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
	input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
	input_report_abs(input, ABS_PRESSURE, le16_to_cpup((__le16 *)&data[6]));
	input_report_key(input, BTN_STYLUS, data[1] & 0x02);
	input_report_key(input, BTN_STYLUS2, data[1] & 0x10);
	return 1;
}

static int wacom_bpt_touch(struct wacom_wac *wacom)
{
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int i, touch_count = 0;

	if (data[0] != 0x02)
	    return 0;

	for (i = 0; i < 1; i++) {
		int offset = (data[1] & 0x80) ? (8 * 0) : (9 * 0);
		bool touch = data[offset + 3] & 0x80;

		/*
		 * Touch events need to be disabled while stylus is
		 * in proximity because user's hand is resting on touchpad
		 * and sending unwanted events.  User expects tablet buttons
		 * to continue working though.
		 */
		touch = touch && !wacom->shared->stylus_in_proximity;

		if (touch)
			touch_count++;

		/* Only 1st touch is supported in kernels without MT */
		if (i > 0)
			continue;

		if (touch) {
			int x = get_unaligned_be16(&data[offset + 3]) & 0x7ff;
			int y = get_unaligned_be16(&data[offset + 5]) & 0x7ff;
			x <<= 5;
			y <<= 5;
			input_report_abs(input, ABS_X, x);
			input_report_abs(input, ABS_Y, y);
		}
	}

	wacom->shared->touch_down = touch_count > 0;
	input_report_key(input, BTN_TOUCH, touch_count);
	input_report_key(input, BTN_TOOL_FINGER, touch_count == 1);
	input_report_key(input, BTN_TOOL_DOUBLETAP, touch_count == 2);

	input_report_key(input, BTN_LEFT, (data[1] & 0x08) != 0);
	input_report_key(input, BTN_FORWARD, (data[1] & 0x04) != 0);
	input_report_key(input, BTN_BACK, (data[1] & 0x02) != 0);
	input_report_key(input, BTN_RIGHT, (data[1] & 0x01) != 0);

	input_sync(input);

	return 0;
}

static void wacom_bpt3_touch_msg(struct wacom_wac *wacom, unsigned char *data)
{
	struct input_dev *input = wacom->input;
	bool touch = data[1] & 0x80;

	touch = touch && !wacom->shared->stylus_in_proximity;

	if (touch) {
		int x = (data[2] << 4) | (data[4] >> 4);
		int y = (data[3] << 4) | (data[4] & 0x0f);
		int w = data[6] * 100;

		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
		input_report_abs(input, ABS_TOOL_WIDTH, w);
	}
}

static void wacom_bpt3_button_msg(struct wacom_wac *wacom, unsigned char *data)
{
	struct input_dev *input = wacom->input;
	struct wacom_features *features = &wacom->features;

	if (features->type == INTUOSHT || features->type == INTUOSHT2) {
		input_report_key(input, BTN_LEFT, (data[1] & 0x02) != 0);
		input_report_key(input, BTN_BACK, (data[1] & 0x08) != 0);
	} else {
		input_report_key(input, BTN_BACK, (data[1] & 0x02) != 0);
		input_report_key(input, BTN_LEFT, (data[1] & 0x08) != 0);
	}
	input_report_key(input, BTN_FORWARD, (data[1] & 0x04) != 0);
	input_report_key(input, BTN_RIGHT, (data[1] & 0x01) != 0);
}

static int wacom_bpt3_touch(struct wacom_wac *wacom)
{
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int count = data[1] & 0x07;
	int i, saw_touch = 0, touch_count = 0;

	if (data[0] != 0x02)
	    return 0;

	/* data has up to 7 fixed sized 8-byte messages starting at data[2] */
	for (i = 0; i < count; i++) {
		int offset = (8 * i) + 2;
		int msg_id = data[offset];

		if (msg_id >= 2 && msg_id <= 17)
		{
			bool touch = data[offset+1] & 0x80 && !wacom->shared->stylus_in_proximity;
			if (touch)
				touch_count++;

			/* Only 1st touch support on kernels without MT */
			if (msg_id == 2)
				wacom_bpt3_touch_msg(wacom, data + offset);
			saw_touch++;
		} else if (msg_id == 128)
			wacom_bpt3_button_msg(wacom, data + offset);

	}

	if (saw_touch) {
		input_report_key(input, BTN_TOUCH, touch_count);
		input_report_key(input, BTN_TOOL_FINGER, touch_count == 1);
		input_report_key(input, BTN_TOOL_DOUBLETAP, touch_count == 2);
		input_report_key(input, BTN_TOOL_TRIPLETAP, touch_count == 3);
		input_report_key(input, BTN_TOOL_QUADTAP, touch_count >= 4);
	}
	wacom->shared->touch_down = touch_count > 0;

	input_sync(input);

	return 0;
}

static int wacom_bpt_pen(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int prox = 0, x = 0, y = 0, p = 0, d = 0, pen = 0, btn1 = 0, btn2 = 0;

	if (data[0] != WACOM_REPORT_PENABLED)
	    return 0;

	prox = (data[1] & 0x20) == 0x20;

	/*
	 * All reports shared between PEN and RUBBER tool must be
	 * forced to a known starting value (zero) when transitioning to
	 * out-of-prox.
	 *
	 * If not reset then, to userspace, it will look like lost events
	 * if new tool comes in-prox with same values as previous tool sent.
	 *
	 * Hardware does report zero in most out-of-prox cases but not all.
	 */
	if (!wacom->shared->stylus_in_proximity) {
		if (data[1] & 0x08) {
			wacom->tool[0] = BTN_TOOL_RUBBER;
			wacom->id[0] = ERASER_DEVICE_ID;
		} else {
			wacom->tool[0] = BTN_TOOL_PEN;
			wacom->id[0] = STYLUS_DEVICE_ID;
		}
		wacom->shared->stylus_in_proximity = true;
	}

	wacom->shared->stylus_in_proximity = prox;
	if (wacom->shared->touch_down)
		return 0;

	if (prox) {
		x = le16_to_cpup((__le16 *)&data[2]);
		y = le16_to_cpup((__le16 *)&data[4]);
		p = le16_to_cpup((__le16 *)&data[6]);
		/*
		 * Convert distance from out prox to distance from tablet.
		 * distance will be greater than distance_max once
		 * touching and applying pressure; do not report negative
		 * distance.
		 */
		if (data[8] <= features->distance_max)
			d = features->distance_max - data[8];

		pen = data[1] & 0x01;
		btn1 = data[1] & 0x02;
		btn2 = data[1] & 0x04;
	} else {
		wacom->id[0] = 0;
	}

	input_report_key(input, BTN_TOUCH, pen);
	input_report_key(input, BTN_STYLUS, btn1);
	input_report_key(input, BTN_STYLUS2, btn2);

	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_report_abs(input, ABS_PRESSURE, p);
	input_report_abs(input, ABS_DISTANCE, d);

	input_report_key(input, wacom->tool[0], prox); /* PEN or RUBBER */

	return 1;
}

static int wacom_dtus_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	unsigned short prox, pressure = 0;

	if (data[0] != WACOM_REPORT_DTUS && data[0] != WACOM_REPORT_DTUSPAD) {
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d", __func__, data[0]);
		return 0;
	} else if (data[0] == WACOM_REPORT_DTUSPAD) {
		input_report_key(input, BTN_0, (data[1] & 0x01));
		input_report_key(input, BTN_1, (data[1] & 0x02));
		input_report_key(input, BTN_2, (data[1] & 0x04));
		input_report_key(input, BTN_3, (data[1] & 0x08));
		input_report_abs(input, ABS_MISC,
				 data[1] & 0x0f ? PAD_DEVICE_ID : 0);
		/*
		 * Serial number is required when expresskeys are
		 * reported through pen interface.
		 */
		input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
		return 1;
	} else {
		prox = data[1] & 0x80;
		if (prox) {
			switch ((data[1] >> 3) & 3) {
			case 1: /* Rubber */
				wacom->tool[0] = BTN_TOOL_RUBBER;
				wacom->id[0] = ERASER_DEVICE_ID;
				break;

			case 2: /* Pen */
				wacom->tool[0] = BTN_TOOL_PEN;
				wacom->id[0] = STYLUS_DEVICE_ID;
				break;
			}
		}

		input_report_key(input, BTN_STYLUS, data[1] & 0x20);
		input_report_key(input, BTN_STYLUS2, data[1] & 0x40);
		input_report_abs(input, ABS_X, get_unaligned_be16(&data[3]));
		input_report_abs(input, ABS_Y, get_unaligned_be16(&data[5]));
		pressure = ((data[1] & 0x03) << 8) | (data[2] & 0xff);
		input_report_abs(input, ABS_PRESSURE, pressure);
		input_report_key(input, BTN_TOUCH, pressure > 10);

		if (!prox) /* out-prox */
			wacom->id[0] = 0;
		input_report_key(input, wacom->tool[0], prox);
		input_report_abs(input, ABS_MISC, wacom->id[0]);
		input_event(input, EV_MSC, MSC_SERIAL, 1);
		return 1;
	}
}

static int wacom_dth1152_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	unsigned short prox, pressure = 0;

	if (data[0] != WACOM_REPORT_DTUS) {
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d", __func__, data[0]);
		return 0;
	} else {
		prox = data[1] & 0x80;
		if (prox) {
			wacom->tool[0] = BTN_TOOL_PEN;
			wacom->id[0] = STYLUS_DEVICE_ID;
		}
		input_report_key(input, BTN_STYLUS, data[1] & 0x20);
		input_report_abs(input, ABS_X, get_unaligned_le16(&data[4]));
		input_report_abs(input, ABS_Y, get_unaligned_le16(&data[6]));
		pressure = data[2] | (data[3] << 8);
		input_report_abs(input, ABS_PRESSURE, pressure);
		input_report_key(input, BTN_TOUCH, data[1] & 0x10);

		if (!prox)
			wacom->id[0] = 0;
		input_report_key(input, wacom->tool[0], prox);
		input_report_abs(input, ABS_MISC, wacom->id[0]);
		return 1;
	}
}

static int wacom_graphire_irq(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int prox;
	int rw = 0;
	int retval = 0;

	if (data[0] != WACOM_REPORT_PENABLED) {
		dbg("wacom_graphire_irq: received unknown report #%d", data[0]);
		goto exit;
	}

	prox = data[1] & 0x80;
	if (prox || wacom->id[0]) {
		if (prox) {
			switch ((data[1] >> 5) & 3) {

			case 0:	/* Pen */
				wacom->tool[0] = BTN_TOOL_PEN;
				wacom->id[0] = STYLUS_DEVICE_ID;
				break;

			case 1: /* Rubber */
				wacom->tool[0] = BTN_TOOL_RUBBER;
				wacom->id[0] = ERASER_DEVICE_ID;
				break;

			case 2: /* Mouse with wheel */
				input_report_key(input, BTN_MIDDLE, data[1] & 0x04);
				/* fall through */

			case 3: /* Mouse without wheel */
				wacom->tool[0] = BTN_TOOL_MOUSE;
				wacom->id[0] = CURSOR_DEVICE_ID;
				break;
			}
		}
		if (wacom->tool[0] != BTN_TOOL_MOUSE) {
			input_report_abs(input, ABS_PRESSURE, data[6] | ((data[7] & 0x01) << 8));
			input_report_key(input, BTN_TOUCH, data[1] & 0x01);
			input_report_key(input, BTN_STYLUS, data[1] & 0x02);
			input_report_key(input, BTN_STYLUS2, data[1] & 0x04);
		} else {
			input_report_key(input, BTN_LEFT, data[1] & 0x01);
			input_report_key(input, BTN_RIGHT, data[1] & 0x02);
			if (features->type == WACOM_G4 ||
					features->type == WACOM_MO) {
				input_report_abs(input, ABS_DISTANCE, data[6] & 0x3f);
				rw = (data[7] & 0x04) - (data[7] & 0x03);
			} else {
				input_report_abs(input, ABS_DISTANCE, data[7] & 0x3f);
				rw = -(signed char)data[6];
			}
			input_report_rel(input, REL_WHEEL, rw);
		}

		if (!prox) {
			wacom->id[0] = 0;
			input_report_abs(input, ABS_X, 0);
			input_report_abs(input, ABS_Y, 0);
		} else {
			input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
			input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
		}
		input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
		input_event(input, EV_MSC, MSC_SERIAL, 1);
		input_report_key(input, wacom->tool[0], prox);
		input_sync(input); /* sync last event */
	}

	/* send pad data */
	switch (features->type) {
	case WACOM_G4:
		prox = data[7] & 0xf8;
		if (prox || wacom->id[1]) {
			wacom->id[1] = PAD_DEVICE_ID;
			input_report_key(input, BTN_0, (data[7] & 0x40));
			input_report_key(input, BTN_4, (data[7] & 0x80));
			rw = ((data[7] & 0x18) >> 3) - ((data[7] & 0x20) >> 3);
			input_report_rel(input, REL_WHEEL, rw);
			if (!prox)
				wacom->id[1] = 0;
			input_report_abs(input, ABS_MISC, wacom->id[1]);
			input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
			retval = 1;
		}
		break;

	case WACOM_MO:
		prox = (data[7] & 0x78) || (data[8] & 0x7f);
		if (prox || wacom->id[1]) {
			wacom->id[1] = PAD_DEVICE_ID;
			input_report_key(input, BTN_0, (data[7] & 0x08));
			input_report_key(input, BTN_1, (data[7] & 0x20));
			input_report_key(input, BTN_4, (data[7] & 0x10));
			input_report_key(input, BTN_5, (data[7] & 0x40));
			input_report_abs(input, ABS_WHEEL, (data[8] & 0x7f));
			if (!prox)
				wacom->id[1] = 0;
			input_report_abs(input, ABS_MISC, wacom->id[1]);
			input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
			retval = 1;
		}
		break;
	}
exit:
	return retval;
}

static int wacom_intuos_id_mangle(int tool_id)
{
	return (tool_id & ~0xFFF) << 4 | (tool_id & 0xFFF);
}

static int wacom_intuos_get_tool_type(int tool_id)
{
	int tool_type;

	switch (tool_id) {
	case 0x812: /* Inking pen */
	case 0x801: /* Intuos3 Inking pen */
	case 0x12802: /* Intuos4/5 Inking Pen */
	case 0x012:
		tool_type = BTN_TOOL_PENCIL;
		break;

	case 0x822: /* Pen */
	case 0x842:
	case 0x852:
	case 0x823: /* Intuos3 Grip Pen */
	case 0x813: /* Intuos3 Classic Pen */
	case 0x885: /* Intuos3 Marker Pen */
	case 0x802: /* Intuos4/5 13HD/24HD General Pen */
	case 0x804: /* Intuos4/5 13HD/24HD Marker Pen */
	case 0x8e2: /* IntuosHT2 pen */
	case 0x022:
	case 0x10804: /* Intuos4/5 13HD/24HD Art Pen */
	case 0x14802: /* Intuos4/5 13HD/24HD Classic Pen */
	case 0x16802: /* Cintiq 13HD Pro Pen */
	case 0x18802: /* DTH2242 Pen */
	case 0x10802: /* Intuos4/5 13HD/24HD General Pen */
		tool_type = BTN_TOOL_PEN;
		break;

	case 0x832: /* Stroke pen */
	case 0x032:
		tool_type = BTN_TOOL_BRUSH;
		break;

	case 0x007: /* Mouse 4D and 2D */
	case 0x09c:
	case 0x094:
	case 0x017: /* Intuos3 2D Mouse */
	case 0x806: /* Intuos4 Mouse */
		tool_type = BTN_TOOL_MOUSE;
		break;

	case 0x096: /* Lens cursor */
	case 0x097: /* Intuos3 Lens cursor */
	case 0x006: /* Intuos4 Lens cursor */
		tool_type = BTN_TOOL_LENS;
		break;

	case 0x82a: /* Eraser */
	case 0x84a:
	case 0x85a:
	case 0x91a:
	case 0xd1a:
	case 0x0fa:
	case 0x82b: /* Intuos3 Grip Pen Eraser */
	case 0x81b: /* Intuos3 Classic Pen Eraser */
	case 0x91b: /* Intuos3 Airbrush Eraser */
	case 0x80c: /* Intuos4/5 13HD/24HD Marker Pen Eraser */
	case 0x80a: /* Intuos4/5 13HD/24HD General Pen Eraser */
	case 0x90a: /* Intuos4/5 13HD/24HD Airbrush Eraser */
	case 0x1480a: /* Intuos4/5 13HD/24HD Classic Pen Eraser */
	case 0x1090a: /* Intuos4/5 13HD/24HD Airbrush Eraser */
	case 0x1080c: /* Intuos4/5 13HD/24HD Art Pen Eraser */
	case 0x1680a: /* Cintiq 13HD Pro Pen Eraser */
	case 0x1880a: /* DTH2242 Eraser */
	case 0x1080a: /* Intuos4/5 13HD/24HD General Pen Eraser */
		tool_type = BTN_TOOL_RUBBER;
		break;

	case 0xd12:
	case 0x912:
	case 0x112:
	case 0x913: /* Intuos3 Airbrush */
	case 0x902: /* Intuos4/5 13HD/24HD Airbrush */
	case 0x10902: /* Intuos4/5 13HD/24HD Airbrush */
		tool_type = BTN_TOOL_AIRBRUSH;
		break;

	default: /* Unknown tool */
		tool_type = BTN_TOOL_PEN;
		break;
	}
	return tool_type;
}

static int wacom_intuos_pad(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int i;
	int buttons = 0, nbuttons = features->numbered_buttons;
	int keys = 0, nkeys = 0;
	int ring1 = 0, ring2 = 0;
	int strip1 = 0, strip2 = 0;
	bool prox = false;

	/* pad packets. Works as a second tool and is always in prox */
	if (!(data[0] == WACOM_REPORT_INTUOSPAD || data[0] == WACOM_REPORT_INTUOS5PAD ||
	      data[0] == WACOM_REPORT_CINTIQPAD))
		return 0;

	if (features->type >= INTUOS4S && features->type <= INTUOS4L) {
		buttons = (data[3] << 1) | (data[2] & 0x01);
		ring1 = data[1];
	} else if (features->type == DTK) {
		buttons = data[6];
	} else if (features->type == WACOM_13HD) {
		buttons = (data[4] << 1) | (data[3] & 0x01);
	} else if (features->type == WACOM_24HD) {
		buttons = (data[8] << 8) | data[6];
		ring1 = data[1];
		ring2 = data[2];

		/*
		 * Three "buttons" are available on the 24HD which are
		 * physically implemented as a touchstrip. Each button
		 * is approximately 3 bits wide with a 2 bit spacing.
		 * The raw touchstrip bits are stored at:
		 *    ((data[3] & 0x1f) << 8) | data[4])
		 */
		nkeys = 3;
		keys = ((data[3] & 0x1C) ? 1<<2 : 0) |
		       ((data[4] & 0xE0) ? 1<<1 : 0) |
		       ((data[4] & 0x07) ? 1<<0 : 0);
	} else if (features->type == WACOM_27QHD) {
		nkeys = 3;
		keys = data[2] & 0x07;

		input_report_abs(input, ABS_X, be16_to_cpup((__be16 *)&data[4]));
		input_report_abs(input, ABS_Y, be16_to_cpup((__be16 *)&data[6]));
		input_report_abs(input, ABS_Z, be16_to_cpup((__be16 *)&data[8]));
		if ((data[2] & 0x07) | data[4] | data[5] | data[6] | data[7] | data[8] | data[9]) {
			input_report_abs(input, ABS_MISC, PAD_DEVICE_ID);
		} else {
			input_report_abs(input, ABS_MISC, 0);
		}
	} else if (features->type >= INTUOS5S && features->type <= INTUOSPL) {
		/*
		 * ExpressKeys on Intuos5/Intuos Pro have a capacitive sensor in
		 * addition to the mechanical switch. Switch data is
		 * stored in data[4], capacitive data in data[5].
		 *
		 * Touch ring mode switch (data[3]) has no capacitive sensor
		 */
		buttons = (data[4] << 1) | (data[3] & 0x01);
		ring1 = data[2];
	} else {
		if (features->type == WACOM_21UX2 || features->type == WACOM_22HD) {
			buttons = (data[8] << 10) | ((data[7] & 0x01) << 9) |
			          (data[6] << 1) | (data[5] & 0x01);

			if (features->type == WACOM_22HD) {
				nkeys = 3;
				keys = data[9] & 0x07;
			}
		} else {
			buttons = ((data[6] & 0x10) << 10) |
			          ((data[5] & 0x10) << 9)  |
			          ((data[6] & 0x0F) << 4)  |
			          (data[5] & 0x0F);
		}
		strip1 = ((data[1] & 0x1f) << 8) | data[2];
		strip2 = ((data[3] & 0x1f) << 8) | data[4];
	}

	prox = (buttons & ~(~0 << nbuttons)) | (keys & ~(~0 << nkeys)) |
	       (ring1 & 0x80) | (ring2 & 0x80) | strip1 | strip2;

	wacom_report_numbered_buttons(input, nbuttons, buttons);

	for (i = 0; i < nkeys; i++)
		input_report_key(input, KEY_PROG1 + i, keys & (1 << i));

	input_report_abs(input, ABS_RX, strip1);
	input_report_abs(input, ABS_RY, strip2);

	input_report_abs(input, ABS_WHEEL,    (ring1 & 0x80) ? (ring1 & 0x7f) : 0);
	input_report_abs(input, ABS_THROTTLE, (ring2 & 0x80) ? (ring2 & 0x7f) : 0);

	input_report_key(input, wacom->tool[1], prox ? 1 : 0);
	input_report_abs(input, ABS_MISC, prox ? PAD_DEVICE_ID : 0);

	input_event(input, EV_MSC, MSC_SERIAL, 0xffffffff);

	return 1;
}

static int wacom_intuos_inout(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int idx = (features->type == INTUOS) ? (data[1] & 0x01) : 0;

	if (!(((data[1] & 0xfc) == 0xc0) ||  /* in prox */
	    ((data[1] & 0xfe) == 0x20) ||    /* in range */
	    ((data[1] & 0xfe) == 0x80)))     /* out prox */
		return 0;

	/* Enter report */
	if ((data[1] & 0xfc) == 0xc0) {
		/* serial number of the tool */
		wacom->serial[idx] = ((data[3] & 0x0f) << 28) +
			(data[4] << 20) + (data[5] << 12) +
			(data[6] << 4) + (data[7] >> 4);

		wacom->id[idx] = (data[2] << 4) | (data[3] >> 4) |
		     ((data[7] & 0x0f) << 16) | ((data[8] & 0xf0) << 8);

		wacom->tool[idx] = wacom_intuos_get_tool_type(wacom->id[idx]);

		wacom->shared->stylus_in_proximity = true;
		return 1;
	}

	/* in Range */
	if ((data[1] & 0xfe) == 0x20) {
		if (features->type != INTUOSHT2)
			wacom->shared->stylus_in_proximity = true;

		/* in Range while exiting */
		if (wacom->reporting_data) {
			input_report_key(input, BTN_TOUCH, 0);
			input_report_abs(input, ABS_PRESSURE, 0);
			input_report_abs(input, ABS_DISTANCE, wacom->features.distance_max);
			return 2;
		}
		return 1;
	}

	/* Exit report */
	if ((data[1] & 0xfe) == 0x80) {
		wacom->shared->stylus_in_proximity = false;
		wacom->reporting_data = false;

		/* don't report exit if we don't know the ID */
		if (!wacom->id[idx])
			return 1;

		/*
		 * Reset all states otherwise we lose the initial states
		 * when in-prox next time
		 */
		input_report_abs(input, ABS_DISTANCE, 0);
		input_report_abs(input, ABS_X, 0);
		input_report_abs(input, ABS_Y, 0);
		input_report_abs(input, ABS_TILT_X, 0);
		input_report_abs(input, ABS_TILT_Y, 0);
		if (wacom->tool[idx] >= BTN_TOOL_MOUSE) {
			input_report_key(input, BTN_LEFT, 0);
			input_report_key(input, BTN_MIDDLE, 0);
			input_report_key(input, BTN_RIGHT, 0);
			input_report_key(input, BTN_SIDE, 0);
			input_report_key(input, BTN_EXTRA, 0);
			input_report_abs(input, ABS_THROTTLE, 0);
			input_report_abs(input, ABS_RZ, 0);
		} else {
			input_report_abs(input, ABS_PRESSURE, 0);
			input_report_key(input, BTN_STYLUS, 0);
			input_report_key(input, BTN_STYLUS2, 0);
			input_report_key(input, BTN_TOUCH, 0);
			input_report_abs(input, ABS_WHEEL, 0);
			if (features->type >= INTUOS3S)
				input_report_abs(input, ABS_Z, 0);
		}
		input_report_key(input, wacom->tool[idx], 0);
		input_report_abs(input, ABS_MISC, 0); /* reset tool id */
		input_event(input, EV_MSC, MSC_SERIAL, wacom->serial[idx]);
		wacom->id[idx] = 0;
		return 2;
	}

	/* don't report other events if we don't know the ID */
	if (!wacom->id[idx])
		return 1;

	return 0;
}

static int wacom_intuos_general(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int idx = (features->type == INTUOS) ? (data[1] & 0x01) : 0;
	unsigned char type = (data[1] >> 1) & 0x0F;
	unsigned int x, y, distance, t;

	if (wacom->shared->touch_down)
		return 1;

	/*
	 * don't report events for invalid data
	 */
	/* older I4 styli don't work with new Cintiqs */
	if ((!((wacom->id[idx] >> 16) & 0x01) &&
			(features->type == WACOM_21UX2)) ||
	    /* Only large Intuos support Lense Cursor */
	    (wacom->tool[idx] == BTN_TOOL_LENS &&
		(features->type == INTUOS3 ||
		 features->type == INTUOS3S ||
		 features->type == INTUOS4 ||
		 features->type == INTUOS4S ||
		 features->type == INTUOS5 ||
		 features->type == INTUOS5S ||
		 features->type == INTUOSPM ||
		 features->type == INTUOSPS)) ||
	   /* Cintiq doesn't send data when RDY bit isn't set */
	   (features->type == CINTIQ && !(data[1] & 0x40)))
		return 1;

	if (data[0] != WACOM_REPORT_PENABLED && data[0] != WACOM_REPORT_CINTIQ &&
		data[0] != WACOM_REPORT_INTUOS_PEN)
		return 0;

	x = (be16_to_cpup((__be16 *)&data[2]) << 1) | ((data[9] >> 1) & 1);
	y = (be16_to_cpup((__be16 *)&data[4]) << 1) | (data[9] & 1);
	distance = data[9] >> 2;
	if (features->type < INTUOS3S) {
		x >>= 1;
		y >>= 1;
		distance >>= 1;
	}
	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_report_abs(input, ABS_DISTANCE, distance);

	switch (type) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
		/* general pen packet */
		t = (data[6] << 3) | ((data[7] & 0xC0) >> 5) | (data[1] & 1);
		if (features->pressure_max < 2047)
			t >>= 1;
		input_report_abs(input, ABS_PRESSURE, t);
		if (features->type != INTUOSHT2) {
			input_report_abs(input, ABS_TILT_X,
				((data[7] << 1) & 0x7e) | (data[8] >> 7));
			input_report_abs(input, ABS_TILT_Y, data[8] & 0x7f);
		}
		input_report_key(input, BTN_STYLUS, data[1] & 2);
		input_report_key(input, BTN_STYLUS2, data[1] & 4);
		input_report_key(input, BTN_TOUCH, t > 10);
		break;

	case 0x0a:
		/* airbrush second packet */
		input_report_abs(input, ABS_WHEEL,
				(data[6] << 2) | ((data[7] >> 6) & 3));
		input_report_abs(input, ABS_TILT_X,
				((data[7] << 1) & 0x7e) | (data[8] >> 7));
		input_report_abs(input, ABS_TILT_Y, data[8] & 0x7f);
		break;

	case 0x05:
		/* Rotation packet */
		if (features->type >= INTUOS3S) {
			/* I3 marker pen rotation */
			t = (data[6] << 3) | ((data[7] >> 5) & 7);
			t = (data[7] & 0x20) ? ((t > 900) ? ((t-1) / 2 - 1350) :
				((t-1) / 2 + 450)) : (450 - t / 2) ;
			input_report_abs(input, ABS_Z, t);
		} else {
			/* 4D mouse 2nd packet */
			t = (data[6] << 3) | ((data[7] >> 5) & 7);
			input_report_abs(input, ABS_RZ, (data[7] & 0x20) ?
				((t - 1) / 2) : -t / 2);
		}
		break;

	/* 4D mouse, 2D mouse, marker pen rotation, tilt mouse, or Lens cursor packets */
	case 0x04:
		/* 4D mouse 1st packet */
		input_report_key(input, BTN_LEFT,   data[8] & 0x01);
		input_report_key(input, BTN_MIDDLE, data[8] & 0x02);
		input_report_key(input, BTN_RIGHT,  data[8] & 0x04);

		input_report_key(input, BTN_SIDE,   data[8] & 0x20);
		input_report_key(input, BTN_EXTRA,  data[8] & 0x10);
		t = (data[6] << 2) | ((data[7] >> 6) & 3);
		input_report_abs(input, ABS_THROTTLE, (data[8] & 0x08) ? -t : t);
		break;

	case 0x06:
		/* I4 mouse */
		input_report_key(input, BTN_LEFT,   data[6] & 0x01);
		input_report_key(input, BTN_MIDDLE, data[6] & 0x02);
		input_report_key(input, BTN_RIGHT,  data[6] & 0x04);
		input_report_rel(input, REL_WHEEL, ((data[7] & 0x80) >> 7)
				 - ((data[7] & 0x40) >> 6));
		input_report_key(input, BTN_SIDE,   data[6] & 0x08);
		input_report_key(input, BTN_EXTRA,  data[6] & 0x10);
		input_report_abs(input, ABS_TILT_X,
			(((data[7] << 1) & 0x7e) | (data[8] >> 7)) - 64);
		input_report_abs(input, ABS_TILT_Y, (data[8] & 0x7f) - 64);
		break;

	case 0x08:
		if (wacom->tool[idx] == BTN_TOOL_MOUSE) {
			/* 2D mouse packet */
			input_report_key(input, BTN_LEFT,   data[8] & 0x04);
			input_report_key(input, BTN_MIDDLE, data[8] & 0x08);
			input_report_key(input, BTN_RIGHT,  data[8] & 0x10);
			input_report_rel(input, REL_WHEEL, (data[8] & 0x01)
					 - ((data[8] & 0x02) >> 1));

			/* I3 2D mouse side buttons */
			if (features->type >= INTUOS3S && features->type <= INTUOS3L) {
				input_report_key(input, BTN_SIDE,   data[8] & 0x40);
				input_report_key(input, BTN_EXTRA,  data[8] & 0x20);
			}
		}
		else if (wacom->tool[idx] == BTN_TOOL_LENS) {
			/* Lens cursor packets */
			input_report_key(input, BTN_LEFT,   data[8] & 0x01);
			input_report_key(input, BTN_MIDDLE, data[8] & 0x02);
			input_report_key(input, BTN_RIGHT,  data[8] & 0x04);
			input_report_key(input, BTN_SIDE,   data[8] & 0x10);
			input_report_key(input, BTN_EXTRA,  data[8] & 0x08);
		}
		break;

	case 0x07:
	case 0x09:
	case 0x0b:
	case 0x0c:
	case 0x0d:
	case 0x0e:
	case 0x0f:
		/* unhandled */
		break;
	}

	input_report_abs(input, ABS_MISC,
			 wacom_intuos_id_mangle(wacom->id[idx])); /* report tool id */
	input_report_key(input, wacom->tool[idx], 1);
	input_event(input, EV_MSC, MSC_SERIAL, wacom->serial[idx]);
	wacom->reporting_data = true;
	return 2;
}

static int wacom_intuos_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	int result;

	if (data[0] != WACOM_REPORT_PENABLED && data[0] != WACOM_REPORT_INTUOS_ID1
		&& data[0] != WACOM_REPORT_INTUOS_ID2 && data[0] != WACOM_REPORT_INTUOSPAD
		&& data[0] != WACOM_REPORT_INTUOS_PEN
	        && data[0] != WACOM_REPORT_CINTIQ && data[0] != WACOM_REPORT_CINTIQPAD
		&& data[0] != WACOM_REPORT_INTUOS5PAD) {
		dbg("wacom_intuos_irq: received unknown report #%d", data[0]);
                return 0;
	}

	/* process pad events */
	result = wacom_intuos_pad(wacom);
	if (result)
		return result;

	/* process in/out prox events */
	result = wacom_intuos_inout(wacom);
	if (result)
                return result - 1;

	/* process general packets */
	result = wacom_intuos_general(wacom);
	if (result)
		return result - 1;

	return 0;
}

static void wacom_multitouch_generic_finger(struct wacom_wac *wacom,
					    int contact_id, bool prox,
					    int x, int y)
{
	struct input_dev *input = wacom->input;
	int slot = find_slot_from_contactid(wacom, contact_id);

	if (slot < 0)
		return;

	wacom->slots[slot] = prox ? contact_id : -1;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
	if (wacom->last_finger != slot) {
		if (x == input->abs[ABS_X])
			x++;
		if (y == input->abs[ABS_Y])
			y++;
	}
#endif

	if (wacom->shared)
		prox = prox && !wacom->shared->stylus_in_proximity;

	if (prox) {
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
	}

	input_report_abs(input, ABS_MISC, wacom->id[0]);
	input_report_key(input, wacom->tool[slot+1], prox);
	wacom->last_finger = slot + 1;
	if (slot == 0)
		input_report_key(input, BTN_TOUCH, prox);
	input_event(input, EV_MSC, MSC_SERIAL, slot + 1);
	input_sync(input);
}

static int wacom_multitouch_generic(struct wacom_wac *wacom)
{
	struct input_dev *input = wacom->input;
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	int i, current_num_contacts, contacts_to_send;

	wacom->tool[1] = BTN_TOOL_DOUBLETAP;
	wacom->id[0] = TOUCH_DEVICE_ID;
	wacom->tool[2] = BTN_TOOL_TRIPLETAP;

	switch (features->type) {
	case WACOM_MSPROT:
	case DTH1152T:
		current_num_contacts = data[2];
		break;
	case INTUOSP2:
		current_num_contacts = data[1];
		break;
	default:
		return 0;
	}

	if (current_num_contacts)
		wacom->num_contacts_left = current_num_contacts;

	contacts_to_send = min(5, wacom->num_contacts_left);

	for (i = 0; i < contacts_to_send; i++) {
		int contact_id = -1;
		int x = -1;
		int y = -1;
		int prox = -1;
		int offset;

		switch (features->type) {
		case WACOM_MSPROT:
			offset = WACOM_BYTES_PER_MSPROT_PACKET * i + 3;
			prox = data[offset] & 0x01;
			contact_id = get_unaligned_le16(&data[offset + 1]);
			x = get_unaligned_le16(&data[offset + 3]);
			y = get_unaligned_le16(&data[offset + 5]);
			break;

		case INTUOSP2:
			offset = WACOM_BYTES_PER_INTUOSP2_PACKET * i + 2;
			contact_id = data[offset];
			prox = data[offset + 1] & 0x01;
			x = get_unaligned_le16(&data[offset + 2]);
			y = get_unaligned_le16(&data[offset + 4]);
			break;

		case DTH1152T:
			offset = WACOM_BYTES_PER_MSPROT_PACKET * i + 3;
			prox = data[offset] & 0x1;
			contact_id = get_unaligned_le16(&data[offset + 1]);
			x = get_unaligned_le16(&data[offset + 3]);
			y = get_unaligned_le16(&data[offset + 5]);
			break;

		default:
			continue;
		}

		wacom_multitouch_generic_finger(wacom, contact_id, prox, x, y);
	}

	wacom->num_contacts_left -= contacts_to_send;
	if (wacom->num_contacts_left <= 0) {
		wacom->num_contacts_left = 0;
		wacom->shared->touch_down = test_bit(BTN_TOOL_DOUBLETAP, input->key) ||
					    test_bit(BTN_TOOL_TRIPLETAP, input->key);
	}

	return 0;
}

static int wacom_bpt_irq(struct wacom_wac *wacom, size_t len)
{
	struct wacom_features *features = &wacom->features;

	if ((features->type == INTUOSHT2) &&
	    (features->device_type == BTN_TOOL_PEN))
		return wacom_intuos_irq(wacom);
	else if (len == WACOM_PKGLEN_BBTOUCH)
		return wacom_bpt_touch(wacom);
	else if (len == WACOM_PKGLEN_BBTOUCH3)
		return wacom_bpt3_touch(wacom);
	else if (len == WACOM_PKGLEN_BBFUN || len == WACOM_PKGLEN_BBPEN)
		return wacom_bpt_pen(wacom);

	return 0;
}

static void wacom_tpc_finger_in(struct wacom_wac *wacom, unsigned char *data, int idx)
{
	struct input_dev *input = wacom->input;
	int finger = idx + 1;
	int x = le16_to_cpup((__le16 *)&data[finger * 2]) & 0x7fff;
	int y = le16_to_cpup((__le16 *)&data[4 + finger * 2]) & 0x7fff;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
	if (wacom->last_finger != finger) {
		if (x == input->abs[ABS_X])
			x++;

		if (y == input->abs[ABS_Y])
			y++;
	}
#endif

	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_report_abs(input, ABS_MISC, wacom->id[0]);
	input_report_key(input, wacom->tool[finger], 1);
	if (!idx)
		input_report_key(input, BTN_TOUCH, 1);
	input_event(input, EV_MSC, MSC_SERIAL, finger);
	input_sync(input);

	wacom->last_finger = finger;
}

static void wacom_tpc_touch_out(struct wacom_wac *wacom, int idx)
{
	struct input_dev *input = wacom->input;
	int finger = idx + 1;

	input_report_abs(input, ABS_MISC, 0);
	input_report_key(input, wacom->tool[finger], 0);
	if (!idx)
		input_report_key(input, BTN_TOUCH, 0);
	input_event(input, EV_MSC, MSC_SERIAL, finger);
	input_sync(input);
}

static void wacom_tpc_touch_in(struct wacom_wac *wacom, size_t len)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	wacom->tool[1] = BTN_TOOL_DOUBLETAP;
	wacom->id[0] = TOUCH_DEVICE_ID;
	wacom->tool[2] = BTN_TOOL_TRIPLETAP;

	if (len == WACOM_PKGLEN_TPC1FG_B) {
		input_report_abs(input, ABS_X, get_unaligned_le16(&data[3]));
		input_report_abs(input, ABS_Y, get_unaligned_le16(&data[5]));
		input_report_key(input, BTN_TOUCH, 1);
		input_report_abs(input, ABS_MISC, wacom->id[0]);
		input_report_key(input, wacom->tool[1], 1);
		input_sync(input);
	}
	else if (len == WACOM_PKGLEN_TPC1FG) {
		input_report_abs(input, ABS_X, get_unaligned_le16(&data[1]));
		input_report_abs(input, ABS_Y, get_unaligned_le16(&data[3]));
		input_report_key(input, BTN_TOUCH, 1);
		input_report_abs(input, ABS_MISC, wacom->id[0]);
		input_report_key(input, wacom->tool[1], 1);
		input_sync(input);
	}
	else {

		switch (data[0]) {

		case WACOM_REPORT_TPC1FG:
			input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
			input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
			input_report_abs(input, ABS_PRESSURE, le16_to_cpup((__le16 *)&data[6]));
			input_report_key(input, BTN_TOUCH, le16_to_cpup((__le16 *)&data[6]));
			input_report_abs(input, ABS_MISC, wacom->id[0]);
			input_report_key(input, wacom->tool[1], 1);
			input_sync(input);
			break;

		case WACOM_REPORT_TPC2FG:
			if (data[1] & 0x01)
				wacom_tpc_finger_in(wacom, data, 0);
			else if (wacom->id[1] & 0x01)
				wacom_tpc_touch_out(wacom, 0);

			if (data[1] & 0x02)
				wacom_tpc_finger_in(wacom, data, 1);
			else if (wacom->id[1] & 0x02)
				wacom_tpc_touch_out(wacom, 1);
			break;
		}
	}
}

static int find_slot_from_contactid(struct wacom_wac *wacom, int contactid)
{
	int i;

	for (i = 0; i < 2; ++i) {
		if (wacom->slots[i] == contactid)
			return i;
	}

	for (i = 0; i < 2; ++i) {
		if (wacom->slots[i] == -1)
			return i;
	}

	return -1;
}

static void wacom_tpc_mt(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int current_num_contacts = data[2];
	int i = 0;
	int x_offset = 0;
	int number_touch = 0;

	wacom->tool[1] = BTN_TOOL_DOUBLETAP;
	wacom->id[0] = TOUCH_DEVICE_ID;
	wacom->tool[2] = BTN_TOOL_TRIPLETAP;

	/* MTTPC does not support Height and Width */
	if (wacom->features.type == MTTPC ||
	    wacom->features.type == MTTPC_B ||
	    wacom->features.type == MTTPC_C)
		x_offset = -4;

	/*
	 * First packet resets the counter since only the first
	 * packet in series will have non-zero current_num_contacts.
	 */
	if (current_num_contacts) /* we only process up to 2 fingeres */
	{
		wacom->num_contacts_left = current_num_contacts;

		if (current_num_contacts > 2 && wacom->contacts_to_send)
		{
			/* More than 2 fingers on tablet. Send both fingers up */
			wacom_tpc_touch_out(wacom, 0);
			wacom_tpc_touch_out(wacom, 1);
			wacom->contacts_to_send = 0;
			for (i = 0; i < 10; i++)
				wacom->slots[i] = -1;
			wacom->shared->touch_down = 0;
		} else if (current_num_contacts <= 2) {

			wacom->contacts_to_send = current_num_contacts;

			for (i = 0; i < wacom->contacts_to_send; i++) {
				int offset = (WACOM_BYTES_PER_MT_PACKET + x_offset) * i + 3;
				bool touch = (data[offset] & 0x1) && !wacom->shared->stylus_in_proximity;
				int id = get_unaligned_le16(&data[offset + 1]);
				int slot = find_slot_from_contactid(wacom, id);
				int x = get_unaligned_le16(&data[offset + x_offset + 7]);
				int y = get_unaligned_le16(&data[offset + x_offset + 9]);

				if (wacom->last_finger == id) {
					if (x == input->abs[ABS_X])
						x++;

					if (y == input->abs[ABS_Y])
						y++;
				}

				wacom->id[slot+1] = touch;
				if (!wacom->id[1] && !wacom->id[2])
					wacom->id[0] = 0;

				wacom->slots[slot] = touch ? id : -1;
				input_report_abs(input, ABS_X, x);
				input_report_abs(input, ABS_Y, y);
				input_report_abs(input, ABS_MISC, wacom->id[0]);
				input_report_key(input, wacom->tool[slot+1], touch);
				if (!slot)
					input_report_key(input, BTN_TOUCH, touch);
				input_event(input, EV_MSC, MSC_SERIAL, slot + 1);
				input_sync(input);
				wacom->last_finger = id;
				number_touch += touch;
			}
		}
	}
	wacom->shared->touch_down = number_touch > 0;

	/* There are at most 5 contacts per packet */
	wacom->num_contacts_left -= min(5, wacom->num_contacts_left);
	if (wacom->num_contacts_left < 0)
		wacom->num_contacts_left = 0;
}

static int wacom_tpc_irq(struct wacom_wac *wacom, size_t len)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int prox = 0;
	int retval = 0;

	dbg("wacom_tpc_irq: received report #%d", data[0]);

	if (wacom->features.device_type != BTN_TOOL_PEN) {
	
		if (wacom->shared->stylus_in_proximity) {
			if (wacom->id[1] & 0x01)
				wacom_tpc_touch_out(wacom, 0);

			if (wacom->id[1] & 0x02)
				wacom_tpc_touch_out(wacom, 1);

			wacom->id[1] = 0;
			wacom->shared->touch_down = false;
			return 0;
		}

		if (len == WACOM_PKGLEN_MTTPC) {
			wacom_tpc_mt(wacom);
			return 0;
		} else if (len == WACOM_PKGLEN_TPC1FG) {
			prox = data[0] & 0x01;
		} else if (len == WACOM_PKGLEN_TPC1FG_B) {
			prox = data[2] & 0x01;
		} else {  /* with capacity */
			if (data[0] == WACOM_REPORT_TPC1FG)
				/* single touch */
				prox = data[1] & 0x01;
			else
				/* 2FG touch data */
				prox = data[1] & 0x03;
		}

		if (prox) {
			/* initialize last touched finger */
			if (!wacom->id[1])
				wacom->last_finger = 1;

			wacom_tpc_touch_in(wacom, len);
		} else {
			if (data[0] == WACOM_REPORT_TPC2FG) {
				/* 2FGT out-prox */
				if (wacom->id[1] & 0x01)
					wacom_tpc_touch_out(wacom, 0);

				if (wacom->id[1] & 0x02)
					wacom_tpc_touch_out(wacom, 1);
			} else
				/* one finger touch */
				wacom_tpc_touch_out(wacom, 0);

			wacom->id[0] = 0;
		}
		/* keep prox bit to send proper out-prox event */
		wacom->id[1] = prox;
		wacom->shared->touch_down = prox;
	} else if (data[0] == WACOM_REPORT_PENABLED || len == WACOM_PKGLEN_PENABLED) { /* Penabled */
		prox = data[1] & 0x20;

		if (!wacom->shared->stylus_in_proximity) { /* first in prox */
			/* Going into proximity select tool */
			wacom->tool[0] = (data[1] & 0x0c) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
			if (wacom->tool[0] == BTN_TOOL_PEN)
				wacom->id[0] = STYLUS_DEVICE_ID;
			else
				wacom->id[0] = ERASER_DEVICE_ID;

		}

		wacom->shared->stylus_in_proximity = prox;

		if (wacom->shared->touch_down)
			return 0;

		input_report_key(input, BTN_STYLUS, data[1] & 0x02);
		input_report_key(input, BTN_STYLUS2, data[1] & 0x10);
		input_report_abs(input, ABS_PRESSURE, ((data[7] & 0x07) << 8) | data[6]);
		input_report_key(input, BTN_TOUCH, data[1] & 0x05);
		if (!prox) { /* out-prox */
			wacom->id[0] = 0;
			input_report_abs(input, ABS_X, 0);
			input_report_abs(input, ABS_Y, 0);
		} else {
			input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
			input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
		}
		input_report_key(input, wacom->tool[0], prox);
		input_report_abs(input, ABS_MISC, wacom->id[0]);
		retval = 1;
	}
	return retval;
}

static int wacom_mspro_pad_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int nbuttons = wacom->features.numbered_buttons;
	bool prox;
	int buttons, ring;

	switch (nbuttons) {
		case 11:
			buttons = (data[1] >> 1) | (data[3] << 6);
			break;
		case 13:
			buttons = data[1] | (data[3] << 8);
			break;
		default:
			if (nbuttons)
				dev_warn(input->dev.parent, "%s: unsupported device #%d\n", __func__, data[0]);
			return 0;
	}

	ring = le16_to_cpup((__le16 *)&data[4]);

	prox = buttons || ring;

	wacom_report_numbered_buttons(input, nbuttons, buttons);
	input_report_abs(input, ABS_WHEEL, (ring & 0x80) ? (ring & 0x7f) : 0);

	input_report_key(input, wacom->tool[1], prox ? 1 : 0);
	input_report_abs(input, ABS_MISC, prox ? PAD_DEVICE_ID : 0);

	input_event(input, EV_MSC, MSC_SERIAL, 0xffffffff);

	return 1;
}

static int wacom_intuosp2_pad_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int nbuttons = wacom->features.numbered_buttons;
	bool prox;
	int buttons, ring;
	bool active = false;

	switch (nbuttons) {
		case 9:
			buttons = (data[1]) | (data[3] << 8);
			break;
		default:
			dev_warn(input->dev.parent, "%s: unsupported device #%d\n", __func__, data[0]);
			return 0;
	}

	ring = le16_to_cpup((__le16 *)&data[4]);

	if (ring != WACOM_INTUOSP2_RING_UNTOUCHED)
		prox = buttons || ring;
	else
		prox = buttons;

	wacom_report_numbered_buttons(input, nbuttons, buttons);
	input_report_abs(input, ABS_WHEEL, (ring & 0x80) ? (ring & 0x7f) : 0);

	input_report_key(input, wacom->tool[1], prox ? 1 : 0);

	active = (ring ^ wacom->previous_ring) || (buttons ^ wacom->previous_buttons);

	input_report_abs(input, ABS_MISC, prox ? PAD_DEVICE_ID : 0);

	wacom->previous_buttons = buttons;
	wacom->previous_ring = ring;

	if (active)
		input_event(input, EV_MSC, MSC_SERIAL, 0xffffffff);
	else
		return 0;

	return 1;
}

static int wacom_mspro_pen_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	bool tip, sw1, sw2, range, proximity;
	unsigned int x, y;
	unsigned int pressure;
	int tilt_x, tilt_y;
	int rotation;
	unsigned int fingerwheel;
	unsigned int height;
	u64 tool_uid;
	unsigned int tool_type;

	if (wacom->shared->touch_down)
		return 1;

	tip         = data[1] & 0x01;
	sw1         = data[1] & 0x02;
	sw2         = data[1] & 0x04;
	/* eraser   = data[1] & 0x08; */
	/* invert   = data[1] & 0x10; */
	range       = data[1] & 0x20;
	proximity   = data[1] & 0x40;
	x           = le32_to_cpup((__le32 *)&data[2]) & 0xFFFFFF;
	y           = le32_to_cpup((__le32 *)&data[5]) & 0xFFFFFF;
	pressure    = le16_to_cpup((__le16 *)&data[8]);
	tilt_x      = data[10];
	tilt_y      = data[11];
	rotation    = le16_to_cpup((__le16 *)&data[12]);
	fingerwheel = le16_to_cpup((__le16 *)&data[14]);
	height      = data[16];
	tool_uid    = le64_to_cpup((__le64 *)&data[17]);
	tool_type   = le16_to_cpup((__le16 *)&data[25]);

	wacom->shared->stylus_in_proximity = proximity;

	if (range) {
		wacom->serial[0] = (tool_uid & 0xFFFFFFFF);
		wacom->id[0]     = (tool_uid >> 32) | tool_type;
		wacom->tool[0] = wacom_intuos_get_tool_type(wacom->id[0] & 0xFFFFF);
	}

	/* pointer going from fully "in range" to merely "in proximity" */
	if (!range && wacom->tool[0]) {
		height = wacom->features.distance_max;
	}

	/*
	 * only report data if there's a tool for userspace to associate
	 * the events with.
	 */
	if (wacom->tool[0]) {
		input_report_key(input, BTN_TOUCH, tip);
		input_report_key(input, BTN_STYLUS, sw1);
		input_report_key(input, BTN_STYLUS2, sw2);
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
		input_report_abs(input, ABS_PRESSURE, pressure);
		input_report_abs(input, ABS_TILT_X, tilt_x);
		input_report_abs(input, ABS_TILT_Y, tilt_y);
		input_report_abs(input, ABS_Z, rotation);
		input_report_abs(input, ABS_WHEEL, fingerwheel);
		input_report_abs(input, ABS_DISTANCE, height);
		input_event(input, EV_MSC, MSC_SERIAL, wacom->serial[0]);
		input_report_abs(input, ABS_MISC, range ? wacom_intuos_id_mangle(wacom->id[0]) : 0);
		input_report_key(input, wacom->tool[0], range ? 1 : 0);

		if (!range)
			wacom->tool[0] = 0;
	}

	wacom->shared->stylus_in_proximity = proximity;

	return 1;
}

static int wacom_mspro_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	switch (data[0]) {
		case WACOM_REPORT_MSPRO:
			return wacom_mspro_pen_irq(wacom);
		case WACOM_REPORT_MSPROPAD:
			return wacom_mspro_pad_irq(wacom);
		case WACOM_REPORT_MSPRODEVICE:
			return 0;
		default:
			dev_dbg(input->dev.parent,
				"%s: received unknown report #%d\n", __func__, data[0]);
			break;
	}
	return 0;
}

static int wacom_intuosp2_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	switch (data[0]) {
		case WACOM_REPORT_MSPRO:
			return wacom_mspro_pen_irq(wacom);
		case WACOM_REPORT_MSPROPAD:
			return wacom_intuosp2_pad_irq(wacom);
		case WACOM_REPORT_MSPRODEVICE:
			return 0;
		default:
			dev_dbg(input->dev.parent,
				"%s: received unknown report #%d\n", __func__, data[0]);
			break;
	}
	return 0;
}

void wacom_wac_irq(struct wacom_wac *wacom_wac, size_t len)
{
	bool sync;

	switch (wacom_wac->features.type) {
	case PENPARTNER:
		sync = wacom_penpartner_irq(wacom_wac);
		break;

	case DTU:
		sync = wacom_dtu_irq(wacom_wac);
		break;

	case PL:
		sync = wacom_pl_irq(wacom_wac);
		break;

	case BAMBOO_PT:
	case INTUOSHT:
	case INTUOSHT2:
		sync = wacom_bpt_irq(wacom_wac, len);
		break;

	case WACOM_G4:
	case GRAPHIRE:
	case WACOM_MO:
		sync = wacom_graphire_irq(wacom_wac);
		break;

	case PTU:
		sync = wacom_ptu_irq(wacom_wac);
		break;

	case DTUS:
	case DTUSX:
		sync = wacom_dtus_irq(wacom_wac);
		break;

	case DTH1152:
		sync = wacom_dth1152_irq(wacom_wac);
		break;

	case INTUOS:
	case INTUOS3S:
	case INTUOS3:
	case INTUOS3L:
	case INTUOS4S:
	case INTUOS4:
	case INTUOS4L:
	case CINTIQ:
	case WACOM_BEE:
	case WACOM_13HD:
	case WACOM_21UX2:
	case WACOM_22HD:
	case WACOM_24HD:
	case WACOM_27QHD:
	case DTK:
		sync = wacom_intuos_irq(wacom_wac);
		break;

	case WACOM_MSPRO:
		sync = wacom_mspro_irq(wacom_wac);
		break;

	case DTH1152T:
	case WACOM_MSPROT:
		sync = wacom_multitouch_generic(wacom_wac);
		break;

	case INTUOS5S:
	case INTUOS5:
	case INTUOS5L:
	case INTUOSPS:
	case INTUOSPM:
	case INTUOSPL:
		if (len == WACOM_PKGLEN_BBTOUCH3)
			sync = wacom_bpt3_touch(wacom_wac);
		else
			sync = wacom_intuos_irq(wacom_wac);
		break;

	case INTUOSP2:
		if (len == WACOM_PKGLEN_INTUOSP2T &&
		    wacom_wac->data[0] == WACOM_REPORT_VENDOR_DEF_TOUCH)
			sync = wacom_multitouch_generic(wacom_wac);
		else
			sync = wacom_intuosp2_irq(wacom_wac);
		break;

	case TABLETPC:
	case TABLETPC2FG:
	case MTTPC:
	case MTTPC_B:
	case MTTPC_C:
		sync = wacom_tpc_irq(wacom_wac, len);
		break;

	default:
		sync = false;
		break;
	}

	if (sync)
		input_sync(wacom_wac->input);
}

static void wacom_setup_basic_pro_pen(struct wacom_wac *wacom_wac)
{
	struct input_dev *input_dev = wacom_wac->input;

	input_set_capability(input_dev, EV_MSC, MSC_SERIAL);

	__set_bit(BTN_TOOL_PEN, input_dev->keybit);
	__set_bit(BTN_STYLUS, input_dev->keybit);
	__set_bit(BTN_STYLUS2, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_DISTANCE,
			     0, wacom_wac->features.distance_max, wacom_wac->features.distance_fuzz, 0);
}

static void wacom_setup_cintiq(struct wacom_wac *wacom_wac)
{
	struct input_dev *input_dev = wacom_wac->input;
	struct wacom_features *features = &wacom_wac->features;

	wacom_setup_basic_pro_pen(wacom_wac);

	__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
	__set_bit(BTN_TOOL_BRUSH, input_dev->keybit);
	__set_bit(BTN_TOOL_PENCIL, input_dev->keybit);
	__set_bit(BTN_TOOL_AIRBRUSH, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_WHEEL, 0, 1023, 0, 0);
	input_set_abs_params(input_dev, ABS_TILT_X, 0, 127, features->tilt_fuzz, 0);
	input_set_abs_params(input_dev, ABS_TILT_Y, 0, 127, features->tilt_fuzz, 0);
}

static void wacom_setup_intuos(struct wacom_wac *wacom_wac)
{
	struct input_dev *input_dev = wacom_wac->input;

	input_set_capability(input_dev, EV_REL, REL_WHEEL);

	wacom_setup_cintiq(wacom_wac);

	__set_bit(BTN_LEFT, input_dev->keybit);
	__set_bit(BTN_RIGHT, input_dev->keybit);
	__set_bit(BTN_MIDDLE, input_dev->keybit);
	__set_bit(BTN_SIDE, input_dev->keybit);
	__set_bit(BTN_EXTRA, input_dev->keybit);
	__set_bit(BTN_TOOL_MOUSE, input_dev->keybit);
	__set_bit(BTN_TOOL_LENS, input_dev->keybit);
	input_set_abs_params(input_dev, ABS_RZ, -900, 899, 0, 0);
	input_set_abs_params(input_dev, ABS_THROTTLE, -1023, 1023, 0, 0);
}

void wacom_setup_device_quirks(struct wacom *wacom)
{
	struct wacom_features *features = &wacom->wacom_wac.features;
	struct usb_endpoint_descriptor *endpoint =
			&(wacom->intf)->cur_altsetting->endpoint[0].desc;

	/* touch device found but size is not defined. use default */
	if (features->device_type == BTN_TOOL_FINGER && !features->x_max) {
		features->x_max = 1023;
		features->y_max = 1023;
	}

	/* Intuos5/Pro and Bamboo 3rd gen have no useful data about its
	 * touch interface in its HID descriptor. If this is the touch
	 * interface (wMaxPacketSize of WACOM_PKGLEN_BBTOUCH3), override
	 * the tablet values.
	 */
	if ((features->type >= INTUOS5S && features->type <= INTUOSPL) ||
		(features->type >= INTUOSHT && features->type <= BAMBOO_PT)) {
		if (endpoint->wMaxPacketSize == WACOM_PKGLEN_BBTOUCH3) {
                        features->device_type = BTN_TOOL_DOUBLETAP;
                        features->pktlen = WACOM_PKGLEN_BBTOUCH3;

                        features->x_max = 4096;
                        features->y_max = 4096;
		} else {
			features->device_type = BTN_TOOL_PEN;
		}
	}

	/* quirk for bamboo touch with 2 low res touches */
	if (features->type == BAMBOO_PT &&
	    features->pktlen == WACOM_PKGLEN_BBTOUCH) {
		features->x_max <<= 5;
		features->y_max <<= 5;
		features->x_fuzz <<= 5;
		features->y_fuzz <<= 5;
		features->quirks |= WACOM_QUIRK_BBTOUCH_LOWRES;
	}

	if (features->type == WIRELESS) {

		/* monitor never has input and pen/touch have delayed create */
		features->quirks |= WACOM_QUIRK_NO_INPUT;

		/* must be monitor interface if no device_type set */
		if (!features->device_type)
			features->quirks |= WACOM_QUIRK_MONITOR;
	}
}

static void wacom_abs_set_axis(struct input_dev *input_dev,
			       struct wacom_wac *wacom_wac)
{
	struct wacom_features *features = &wacom_wac->features;

	input_set_abs_params(input_dev, ABS_X, 0 + features->offset_left,
			     features->x_max - features->offset_right,
			     features->x_fuzz, 0);
	input_set_abs_params(input_dev, ABS_Y, 0 + features->offset_top,
			     features->y_max - features->offset_bottom,
			     features->y_fuzz, 0);

	if (features->device_type == BTN_TOOL_PEN) {
		input_set_abs_params(input_dev, ABS_PRESSURE, 0,
			features->pressure_max, features->pressure_fuzz, 0);
	} else {
		if (features->touch_max > 1) {
			input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0,
				features->x_max, features->x_fuzz, 0);
			input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0,
				features->y_max, features->y_fuzz, 0);
		}
	}
}

static int wacom_numbered_button_to_key(int n)
{
	if (n < 10)
		return BTN_0 + n;
	else if (n < 16)
		return BTN_A + (n-10);
	else if (n < 18)
		return BTN_BASE + (n-16);
	else
		return 0;
}

static void wacom_setup_numbered_buttons(struct input_dev *input_dev,
				int button_count)
{
	int i;

	for (i = 0; i < button_count; i++) {
		int key = wacom_numbered_button_to_key(i);

		if (key)
			__set_bit(key, input_dev->keybit);
	}
}

static void wacom_report_numbered_buttons(struct input_dev *input_dev,
				int button_count, int mask)
{
	int i;

	for (i = 0; i < button_count; i++) {
		int key = wacom_numbered_button_to_key(i);

		if (key)
			input_report_key(input_dev, key, mask & (1 << i));
	}
}

void wacom_setup_input_capabilities(struct input_dev *input_dev,
				    struct wacom_wac *wacom_wac)
{
	struct wacom_features *features = &wacom_wac->features;
	int i;

	input_dev->evbit[0] |= BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(ABS_MISC, input_dev->absbit);

	wacom_abs_set_axis(input_dev, wacom_wac);

	switch (features->type) {
	case INTUOSHT:
	case BAMBOO_PT:
	case INTUOSHT2:
		__clear_bit(ABS_MISC, input_dev->absbit);

		if (features->device_type == BTN_TOOL_DOUBLETAP) {

			__set_bit(BTN_LEFT, input_dev->keybit);
			__set_bit(BTN_FORWARD, input_dev->keybit);
			__set_bit(BTN_RIGHT, input_dev->keybit);
			__set_bit(BTN_BACK, input_dev->keybit);

			__set_bit(BTN_TOOL_FINGER, input_dev->keybit);
			__set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
                        if (features->pktlen == WACOM_PKGLEN_BBTOUCH3) {
                                __set_bit(BTN_TOOL_TRIPLETAP,
                                          input_dev->keybit);
                                __set_bit(BTN_TOOL_QUADTAP,
                                          input_dev->keybit);
				input_set_abs_params(input_dev, ABS_TOOL_WIDTH,
						     0, 255, 0, 0);
			}
		}
		if (features->device_type == BTN_TOOL_PEN) {
			if (features->type == INTUOSHT2) {
				__set_bit(ABS_MISC, input_dev->absbit);
				wacom_setup_basic_pro_pen(wacom_wac);
			} else {
				__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
				__set_bit(BTN_TOOL_PEN, input_dev->keybit);
				__set_bit(BTN_STYLUS, input_dev->keybit);
				__set_bit(BTN_STYLUS2, input_dev->keybit);
				input_set_abs_params(input_dev, ABS_DISTANCE, 0,
						     features->distance_max,
						     features->distance_fuzz, 0);
			}
		}
		break;

	case WACOM_MO:
		__set_bit(BTN_1, input_dev->keybit);
		__set_bit(BTN_5, input_dev->keybit);

		input_set_abs_params(input_dev, ABS_WHEEL, 0, 71, 0, 0);
		/* fall through */

	case WACOM_G4:
		input_set_capability(input_dev, EV_MSC, MSC_SERIAL);

		input_set_abs_params(input_dev, ABS_DISTANCE, 0, features->distance_max,
			features->distance_fuzz, 0);

		__set_bit(BTN_TOOL_FINGER, input_dev->keybit);
		__set_bit(BTN_0, input_dev->keybit);
		__set_bit(BTN_4, input_dev->keybit);
		/* fall through */

	case GRAPHIRE:
		input_set_capability(input_dev, EV_REL, REL_WHEEL);

		__set_bit(BTN_LEFT, input_dev->keybit);
		__set_bit(BTN_RIGHT, input_dev->keybit);
		__set_bit(BTN_MIDDLE, input_dev->keybit);

		__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
		__set_bit(BTN_TOOL_PEN, input_dev->keybit);
		__set_bit(BTN_TOOL_MOUSE, input_dev->keybit);
		__set_bit(BTN_STYLUS, input_dev->keybit);
		__set_bit(BTN_STYLUS2, input_dev->keybit);
		break;

	case WACOM_24HD:
		__set_bit(KEY_PROG1, input_dev->keybit);
		__set_bit(KEY_PROG2, input_dev->keybit);
		__set_bit(KEY_PROG3, input_dev->keybit);
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		input_set_abs_params(input_dev, ABS_THROTTLE, 0, 71, 0, 0);
		/* fall through */

	case DTK:
		wacom_setup_cintiq(wacom_wac);
		break;

	case WACOM_27QHD:
		__set_bit(KEY_PROG1, input_dev->keybit);
		__set_bit(KEY_PROG2, input_dev->keybit);
		__set_bit(KEY_PROG3, input_dev->keybit);
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);

		wacom_setup_cintiq(wacom_wac);
		break;

	case WACOM_22HD:
		__set_bit(KEY_PROG1, input_dev->keybit);
		__set_bit(KEY_PROG2, input_dev->keybit);
		__set_bit(KEY_PROG3, input_dev->keybit);
		/* fall through */

	case WACOM_21UX2:
		__set_bit(BTN_A, input_dev->keybit);
		__set_bit(BTN_B, input_dev->keybit);
		__set_bit(BTN_C, input_dev->keybit);
		__set_bit(BTN_X, input_dev->keybit);
		__set_bit(BTN_Y, input_dev->keybit);
		__set_bit(BTN_Z, input_dev->keybit);
		__set_bit(BTN_BASE, input_dev->keybit);
		__set_bit(BTN_BASE2, input_dev->keybit);
		/* fall through */

	case WACOM_BEE:
		__set_bit(BTN_8, input_dev->keybit);
		__set_bit(BTN_9, input_dev->keybit);
		/* fall through */

	case CINTIQ:
		for (i = 0; i < 8; i++)
			__set_bit(BTN_0 + i, input_dev->keybit);
		__set_bit(BTN_TOOL_FINGER, input_dev->keybit);

		input_set_abs_params(input_dev, ABS_RX, 0, 4096, 0, 0);
		input_set_abs_params(input_dev, ABS_RY, 0, 4096, 0, 0);
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		wacom_setup_cintiq(wacom_wac);
		break;

	case WACOM_MSPRO:
	case WACOM_13HD:
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		wacom_setup_cintiq(wacom_wac);
		break;

	case INTUOS3:
	case INTUOS3L:
		input_set_abs_params(input_dev, ABS_RY, 0, 4096, 0, 0);
		/* fall through */

	case INTUOS3S:
		input_set_abs_params(input_dev, ABS_RX, 0, 4096, 0, 0);
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		/* fall through */

	case INTUOS:
		wacom_setup_intuos(wacom_wac);
		break;

	case INTUOSP2:
		if (features->device_type == BTN_TOOL_TRIPLETAP) {
			for (i = 0; i < 10; i++)
				wacom_wac->slots[i] = -1;

			__set_bit(BTN_TOOL_TRIPLETAP, input_dev->keybit);
			input_set_capability(input_dev, EV_MSC, MSC_SERIAL);
			input_set_abs_params(input_dev, ABS_RX, 0, features->x_phy, 0, 0);
			input_set_abs_params(input_dev, ABS_RY, 0, features->y_phy, 0, 0);
			__set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
		} else {
			wacom_wac->previous_ring = WACOM_INTUOSP2_RING_UNTOUCHED;
		}
	case INTUOS5:
	case INTUOS5L:
	case INTUOSPM:
	case INTUOSPL:
		if (features->device_type == BTN_TOOL_PEN) {
			__set_bit(BTN_7, input_dev->keybit);
			__set_bit(BTN_8, input_dev->keybit);
		}
		/* fall through */

	case INTUOS5S:
	case INTUOSPS:
		if (features->device_type == BTN_TOOL_PEN) {
			for (i = 0; i < 7; i++)
				__set_bit(BTN_0 + i, input_dev->keybit);

			input_set_abs_params(input_dev, ABS_DISTANCE, 0,
					      features->distance_max,
					      features->distance_fuzz, 0);

			input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);

			wacom_setup_intuos(wacom_wac);
		} else if (features->device_type == BTN_TOOL_DOUBLETAP) {
			__clear_bit(ABS_MISC, input_dev->absbit);

			__set_bit(BTN_TOOL_FINGER, input_dev->keybit);
			__set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
			__set_bit(BTN_TOOL_TRIPLETAP, input_dev->keybit);
			__set_bit(BTN_TOOL_QUADTAP, input_dev->keybit);

			input_set_abs_params(input_dev, ABS_TOOL_WIDTH, 0, 255, 0, 0);
		}
		break;

	case INTUOS4:
	case INTUOS4L:
	case INTUOS4S:
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		wacom_setup_intuos(wacom_wac);
		break;

	case MTTPC:
	case MTTPC_B:
	case MTTPC_C:
	case DTH1152T:
	case WACOM_MSPROT:
		if (features->device_type == BTN_TOOL_TRIPLETAP) {
			for (i = 0; i < 10; i++)
				wacom_wac->slots[i] = -1;
		}
		/* fall through */

	case TABLETPC2FG:
		if (features->device_type == BTN_TOOL_TRIPLETAP) {
			__set_bit(BTN_TOOL_TRIPLETAP, input_dev->keybit);
			input_set_capability(input_dev, EV_MSC, MSC_SERIAL);
		}
		/* fall through */

	case TABLETPC:
		if (features->device_type == BTN_TOOL_DOUBLETAP ||
		    features->device_type == BTN_TOOL_TRIPLETAP) {
			input_set_abs_params(input_dev, ABS_RX, 0, features->x_phy, 0, 0);
			input_set_abs_params(input_dev, ABS_RY, 0, features->y_phy, 0, 0);
			__set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
		}

		if (features->device_type != BTN_TOOL_PEN)
			break;  /* no need to process stylus stuff */

		/* fall through */

	case DTUS:
	case DTUSX:
	case PL:
	case PTU:
	case DTU:
	case DTH1152:
		if (features->type == DTUS) {
			input_set_capability(input_dev, EV_MSC, MSC_SERIAL);
		}
		__set_bit(BTN_TOOL_PEN, input_dev->keybit);
		__set_bit(BTN_STYLUS, input_dev->keybit);
		__set_bit(BTN_STYLUS2, input_dev->keybit);
		/* fall through */

	case PENPARTNER:
		__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
		break;
	}

	/*
	 * Because all devices with numbered buttons have the pen and pad on
	 * the same interface we can rely on this check to avoid creating
	 * extra pads. Future devices may require creating a more involved
	 * check.
	 */
	if (features->device_type == BTN_TOOL_PEN)
		wacom_setup_numbered_buttons(input_dev,
					     features->numbered_buttons);
}

static const struct wacom_features wacom_features_0x00 =
	{ "Wacom Penpartner",     WACOM_PKGLEN_PENPRTN,    5040,  3780,  255,  0, PENPARTNER };
static const struct wacom_features wacom_features_0x10 =
	{ "Wacom Graphire",       WACOM_PKGLEN_GRAPHIRE,  10206,  7422,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x11 =
	{ "Wacom Graphire2 4x5",  WACOM_PKGLEN_GRAPHIRE,  10206,  7422,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x12 =
	{ "Wacom Graphire2 5x7",  WACOM_PKGLEN_GRAPHIRE,  13918, 10206,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x13 =
	{ "Wacom Graphire3",      WACOM_PKGLEN_GRAPHIRE,  10208,  7424,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x14 =
	{ "Wacom Graphire3 6x8",  WACOM_PKGLEN_GRAPHIRE,  16704, 12064,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x15 =
	{ "Wacom Graphire4 4x5",  WACOM_PKGLEN_GRAPHIRE,  10208,  7424,  511, 63, WACOM_G4 };
static const struct wacom_features wacom_features_0x16 =
	{ "Wacom Graphire4 6x8",  WACOM_PKGLEN_GRAPHIRE,  16704, 12064,  511, 63, WACOM_G4 };
static const struct wacom_features wacom_features_0x17 =
	{ "Wacom BambooFun 4x5",  WACOM_PKGLEN_BBFUN,     14760,  9225,  511, 63, WACOM_MO };
static const struct wacom_features wacom_features_0x18 =
	{ "Wacom BambooFun 6x8",  WACOM_PKGLEN_BBFUN,     21648, 13530,  511, 63, WACOM_MO };
static const struct wacom_features wacom_features_0x19 =
	{ "Wacom Bamboo1 Medium", WACOM_PKGLEN_GRAPHIRE,  16704, 12064,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x60 =
	{ "Wacom Volito",         WACOM_PKGLEN_GRAPHIRE,   5104,  3712,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x61 =
	{ "Wacom PenStation2",    WACOM_PKGLEN_GRAPHIRE,   3250,  2320,  255, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x62 =
	{ "Wacom Volito2 4x5",    WACOM_PKGLEN_GRAPHIRE,   5104,  3712,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x63 =
	{ "Wacom Volito2 2x3",    WACOM_PKGLEN_GRAPHIRE,   3248,  2320,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x64 =
	{ "Wacom PenPartner2",    WACOM_PKGLEN_GRAPHIRE,   3250,  2320,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x65 =
	{ "Wacom Bamboo",         WACOM_PKGLEN_BBFUN,     14760,  9225,  511, 63, WACOM_MO };
static const struct wacom_features wacom_features_0x69 =
	{ "Wacom Bamboo1",        WACOM_PKGLEN_GRAPHIRE,   5104,  3712,  511, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x6A =
	{ "Wacom Bamboo1 4x6",    WACOM_PKGLEN_GRAPHIRE,  14760,  9225, 1023, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0x6B =
	{ "Wacom Bamboo1 5x8",    WACOM_PKGLEN_GRAPHIRE,  21648, 13530, 1023, 63, GRAPHIRE };
static const struct wacom_features wacom_features_0xD1 =
	{ "Wacom BambooFun 2FG 4x5", WACOM_PKGLEN_BBFUN,  14720,  9200, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD4 =
	{ "Wacom Bamboo Pen 4x5", WACOM_PKGLEN_BBFUN,     14720,  9200, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD2 =
	{ "Wacom Bamboo Craft",   WACOM_PKGLEN_BBFUN,     14720,  9200, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD3 =
	{ "Wacom BambooFun 2FG 6x8", WACOM_PKGLEN_BBFUN,  21648, 13700, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD0 =
	{ "Wacom Bamboo 2FG",     WACOM_PKGLEN_BBFUN,     14720,  9200, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD5 =
	{ "Wacom Bamboo Pen 6x8", WACOM_PKGLEN_BBFUN,     21648, 13700, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD6 =
	{ "Wacom BambooPT 2FG 4x5", WACOM_PKGLEN_BBFUN,   14720,  9200, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD7 =
	{ "Wacom BambooPT 2FG Small", WACOM_PKGLEN_BBFUN, 14720,  9200, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xD8 =
	{ "Wacom Bamboo Comic 2FG", WACOM_PKGLEN_BBFUN,   21648, 13700, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xDA =
	{ "Wacom Bamboo 2FG 4x5 SE", WACOM_PKGLEN_BBFUN,  14720,  9200, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xDB =
	{ "Wacom Bamboo 2FG 6x8 SE", WACOM_PKGLEN_BBFUN,  21648, 13700, 1023, 63, BAMBOO_PT };
static const struct wacom_features wacom_features_0xDD =
        { "Wacom Bamboo Connect", WACOM_PKGLEN_BBPEN,     14720,  9200, 1023,
          31, BAMBOO_PT };
static const struct wacom_features wacom_features_0xDE =
        { "Wacom Bamboo 16FG 4x5", WACOM_PKGLEN_BBPEN,    14720,  9200, 1023,
          31, BAMBOO_PT };
static const struct wacom_features wacom_features_0xDF =
        { "Wacom Bamboo 16FG 6x8", WACOM_PKGLEN_BBPEN,    21648, 13700, 1023,
          31, BAMBOO_PT } ;
static const struct wacom_features wacom_features_0x300 =
	{ "Wacom Bamboo One S",    WACOM_PKGLEN_BBPEN,    14720,  9225, 1023, 31, BAMBOO_PT };
static const struct wacom_features wacom_features_0x301 =
	{ "Wacom Bamboo One M",    WACOM_PKGLEN_BBPEN,    21648, 13530, 1023, 31, BAMBOO_PT };
static const struct wacom_features wacom_features_0x302 =
	{ "Wacom Intuos PT S",     WACOM_PKGLEN_BBPEN,    15200,  9500, 1023, 31, INTUOSHT };
static const struct wacom_features wacom_features_0x303 =
	{ "Wacom Intuos PT M",     WACOM_PKGLEN_BBPEN,    21600, 13500, 1023, 31, INTUOSHT };
static const struct wacom_features wacom_features_0x30E =
	{ "Wacom Intuos S",        WACOM_PKGLEN_BBPEN,    15200,  9500, 1023, 31, INTUOSHT };
static const struct wacom_features wacom_features_0x323 =
	{ "Wacom Intuos P M",      WACOM_PKGLEN_BBPEN,    21600, 13500, 1023, 31, INTUOSHT };
static const struct wacom_features wacom_features_0x33B =
	{ "Wacom Intuos S 2",      WACOM_PKGLEN_INTUOS,   15200,  9500, 2047, 63, INTUOSHT2 };
static const struct wacom_features wacom_features_0x33C =
	{ "Wacom Intuos PT S 2",   WACOM_PKGLEN_INTUOS,   15200,  9500, 2047, 63, INTUOSHT2 };
static const struct wacom_features wacom_features_0x33D =
	{ "Wacom Intuos P M 2",    WACOM_PKGLEN_INTUOS,   21600, 13500, 2047, 63, INTUOSHT2 };
static const struct wacom_features wacom_features_0x33E =
	{ "Wacom Intuos PT M 2",   WACOM_PKGLEN_INTUOS,   21600, 13500, 2047, 63, INTUOSHT2 };
static const struct wacom_features wacom_features_0x20 =
	{ "Wacom Intuos 4x5",     WACOM_PKGLEN_INTUOS,    12700, 10600, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x21 =
	{ "Wacom Intuos 6x8",     WACOM_PKGLEN_INTUOS,    20320, 16240, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x22 =
	{ "Wacom Intuos 9x12",    WACOM_PKGLEN_INTUOS,    30480, 24060, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x23 =
	{ "Wacom Intuos 12x12",   WACOM_PKGLEN_INTUOS,    30480, 31680, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x24 =
	{ "Wacom Intuos 12x18",   WACOM_PKGLEN_INTUOS,    45720, 31680, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x30 =
	{ "Wacom PL400",          WACOM_PKGLEN_GRAPHIRE,   5408,  4056,  255,  0, PL };
static const struct wacom_features wacom_features_0x31 =
	{ "Wacom PL500",          WACOM_PKGLEN_GRAPHIRE,   6144,  4608,  255,  0, PL };
static const struct wacom_features wacom_features_0x32 =
	{ "Wacom PL600",          WACOM_PKGLEN_GRAPHIRE,   6126,  4604,  255,  0, PL };
static const struct wacom_features wacom_features_0x33 =
	{ "Wacom PL600SX",        WACOM_PKGLEN_GRAPHIRE,   6260,  5016,  255,  0, PL };
static const struct wacom_features wacom_features_0x34 =
	{ "Wacom PL550",          WACOM_PKGLEN_GRAPHIRE,   6144,  4608,  511,  0, PL };
static const struct wacom_features wacom_features_0x35 =
	{ "Wacom PL800",          WACOM_PKGLEN_GRAPHIRE,   7220,  5780,  511,  0, PL };
static const struct wacom_features wacom_features_0x37 =
	{ "Wacom PL700",          WACOM_PKGLEN_GRAPHIRE,   6758,  5406,  511,  0, PL };
static const struct wacom_features wacom_features_0x38 =
	{ "Wacom PL510",          WACOM_PKGLEN_GRAPHIRE,   6282,  4762,  511,  0, PL };
static const struct wacom_features wacom_features_0x39 =
	{ "Wacom DTU710",         WACOM_PKGLEN_GRAPHIRE,  34080, 27660,  511,  0, PL };
static const struct wacom_features wacom_features_0xC4 =
	{ "Wacom DTF521",         WACOM_PKGLEN_GRAPHIRE,   6282,  4762,  511,  0, PL };
static const struct wacom_features wacom_features_0xC0 =
	{ "Wacom DTF720",         WACOM_PKGLEN_GRAPHIRE,   6858,  5506,  511,  0, PL };
static const struct wacom_features wacom_features_0xC2 =
	{ "Wacom DTF720a",        WACOM_PKGLEN_GRAPHIRE,   6858,  5506,  511,  0, PL };
static const struct wacom_features wacom_features_0x03 =
	{ "Wacom Cintiq Partner", WACOM_PKGLEN_GRAPHIRE,  20480, 15360,  511,  0, PTU };
static const struct wacom_features wacom_features_0x41 =
	{ "Wacom Intuos2 4x5",    WACOM_PKGLEN_INTUOS,    12700, 10600, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x42 =
	{ "Wacom Intuos2 6x8",    WACOM_PKGLEN_INTUOS,    20320, 16240, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x43 =
	{ "Wacom Intuos2 9x12",   WACOM_PKGLEN_INTUOS,    30480, 24060, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x44 =
	{ "Wacom Intuos2 12x12",  WACOM_PKGLEN_INTUOS,    30480, 31680, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x45 =
	{ "Wacom Intuos2 12x18",  WACOM_PKGLEN_INTUOS,    45720, 31680, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0xB0 =
	{ "Wacom Intuos3 4x5",    WACOM_PKGLEN_INTUOS,    25400, 20320, 1023, 63, INTUOS3S, 4 };
static const struct wacom_features wacom_features_0xB1 =
	{ "Wacom Intuos3 6x8",    WACOM_PKGLEN_INTUOS,    40640, 30480, 1023, 63, INTUOS3, 8 };
static const struct wacom_features wacom_features_0xB2 =
	{ "Wacom Intuos3 9x12",   WACOM_PKGLEN_INTUOS,    60960, 45720, 1023, 63, INTUOS3, 8 };
static const struct wacom_features wacom_features_0xB3 =
	{ "Wacom Intuos3 12x12",  WACOM_PKGLEN_INTUOS,    60960, 60960, 1023, 63, INTUOS3L, 8 };
static const struct wacom_features wacom_features_0xB4 =
	{ "Wacom Intuos3 12x19",  WACOM_PKGLEN_INTUOS,    97536, 60960, 1023, 63, INTUOS3L, 8 };
static const struct wacom_features wacom_features_0xB5 =
	{ "Wacom Intuos3 6x11",   WACOM_PKGLEN_INTUOS,    54204, 31750, 1023, 63, INTUOS3, 8 };
static const struct wacom_features wacom_features_0xB7 =
	{ "Wacom Intuos3 4x6",    WACOM_PKGLEN_INTUOS,    31496, 19685, 1023, 63, INTUOS3S, 4 };
static const struct wacom_features wacom_features_0xB8 =
	{ "Wacom Intuos4 4x6",    WACOM_PKGLEN_INTUOS,    31496, 19685, 2047, 63, INTUOS4S, 7 };
static const struct wacom_features wacom_features_0xB9 =
	{ "Wacom Intuos4 6x9",    WACOM_PKGLEN_INTUOS,    44704, 27940, 2047, 63, INTUOS4, 9 };
static const struct wacom_features wacom_features_0xBA =
	{ "Wacom Intuos4 8x13",   WACOM_PKGLEN_INTUOS,    65024, 40640, 2047, 63, INTUOS4L, 9 };
static const struct wacom_features wacom_features_0xBB =
	{ "Wacom Intuos4 12x19",  WACOM_PKGLEN_INTUOS,    97536, 60960, 2047, 63, INTUOS4L, 9 };
static const struct wacom_features wacom_features_0xBC =
	{ "Wacom Intuos4 WL",     WACOM_PKGLEN_INTUOS,    40640, 25400, 2047, 63, INTUOS4, 9 };
static const struct wacom_features wacom_features_0x26 =
        { "Wacom Intuos5 touch S", WACOM_PKGLEN_INTUOS,  31496, 19685, 2047, 63, INTUOS5S, 7 };
static const struct wacom_features wacom_features_0x27 =
        { "Wacom Intuos5 touch M", WACOM_PKGLEN_INTUOS,  44704, 27940, 2047, 63, INTUOS5, 9 };
static const struct wacom_features wacom_features_0x28 =
        { "Wacom Intuos5 touch L", WACOM_PKGLEN_INTUOS, 65024, 40640, 2047, 63, INTUOS5L, 9 };
static const struct wacom_features wacom_features_0x29 =
        { "Wacom Intuos5 S", WACOM_PKGLEN_INTUOS,  31496, 19685, 2047, 63, INTUOS5S, 7 };
static const struct wacom_features wacom_features_0x2A =
        { "Wacom Intuos5 M", WACOM_PKGLEN_INTUOS,  44704, 27940, 2047, 63, INTUOS5, 9 };
static const struct wacom_features wacom_features_0x314 =
	{ "Wacom Intuos Pro S", WACOM_PKGLEN_INTUOS,  31496, 19685, 2047, 63, INTUOSPS, 7 };
static const struct wacom_features wacom_features_0x315 =
	{ "Wacom Intuos Pro M", WACOM_PKGLEN_INTUOS,  44704, 27940, 2047, 63, INTUOSPM, 9 };
static const struct wacom_features wacom_features_0x317 =
	{ "Wacom Intuos Pro L", WACOM_PKGLEN_INTUOS,  65024, 40640, 2047, 63, INTUOSPL, 9 };
static const struct wacom_features wacom_features_0xF4 =
	{ "Wacom Cintiq 24HD",       WACOM_PKGLEN_INTUOS, 104080, 65200, 2047, 63,
	  WACOM_24HD, 16, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0xF8 =
	{ "Wacom Cintiq 24HD touch", WACOM_PKGLEN_INTUOS, 104080, 65200, 2047, 63,
	  WACOM_24HD, 16, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x32A =
	{ "Wacom Cintiq 27QHD", WACOM_PKGLEN_INTUOS, 119740, 67520, 2047, 63,
	  WACOM_27QHD, 0, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x32B =
	{ "Wacom Cintiq 27QHD touch", WACOM_PKGLEN_INTUOS, 119740, 67520, 2047, 63,
	  WACOM_27QHD, 0, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x3F =
	{ "Wacom Cintiq 21UX",    WACOM_PKGLEN_INTUOS,    87200, 65600, 1023, 63, CINTIQ, 8 };
static const struct wacom_features wacom_features_0xC5 =
	{ "Wacom Cintiq 20WSX",   WACOM_PKGLEN_INTUOS,    86680, 54180, 1023, 63, WACOM_BEE, 10 };
static const struct wacom_features wacom_features_0xC6 =
	{ "Wacom Cintiq 12WX",    WACOM_PKGLEN_INTUOS,    53020, 33440, 1023, 63, WACOM_BEE, 10 };
static const struct wacom_features wacom_features_0x304 =
	{ "Wacom Cintiq 13HD",    WACOM_PKGLEN_INTUOS,    59152, 33448, 1023, 63,
	  WACOM_13HD, 9, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x333 =
	{ "Wacom Cintiq 13HD touch", WACOM_PKGLEN_INTUOS, 59152, 33448, 2047, 63,
	  WACOM_13HD, 9, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0xC7 =
	{ "Wacom DTU1931",        WACOM_PKGLEN_GRAPHIRE,  37832, 30305,  511,  0, PL };
static const struct wacom_features wacom_features_0xCE =
	{ "Wacom DTU2231",        WACOM_PKGLEN_GRAPHIRE,  47864, 27011,  511,  0, DTU };
static const struct wacom_features wacom_features_0xF0 =
	{ "Wacom DTU1631",        WACOM_PKGLEN_GRAPHIRE,  34623, 19553,  511,  0, DTU };
static const struct wacom_features wacom_features_0xFB =
	{ "Wacom DTU1031",        WACOM_PKGLEN_DTUS,      21896, 13760,  511, 0, DTUS, 4, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x32F =
	{ "Wacom DTU1031X",       WACOM_PKGLEN_DTUS,      22472, 12728, 511,  0, DTUSX, 0, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x336 =
	{ "Wacom DTU1141",         WACOM_PKGLEN_DTUS,      23472, 13203, 1023, 0, DTUS, 6, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x343 =
	{ "Wacom DTK1651",         WACOM_PKGLEN_DTUS,     34616, 19559, 1023, 0, DTUS, 4, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x57 =
	{ "Wacom DTK2241",        WACOM_PKGLEN_INTUOS,    95640, 54060, 2047, 63, DTK, 6, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x59 =
	{ "Wacom DTH2242",        WACOM_PKGLEN_INTUOS,    95640, 54060, 2047, 63, DTK, 6, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0xCC =
	{ "Wacom Cintiq 21UX2",   WACOM_PKGLEN_INTUOS,    86800, 65200, 2047, 63, WACOM_21UX2, 18, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0xFA =
	{ "Wacom Cintiq 22HD",    WACOM_PKGLEN_INTUOS,    95440, 53860, 2047, 63, WACOM_22HD, 18, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x5B =
	{ "Wacom Cintiq 22HDT", WACOM_PKGLEN_INTUOS,      95440, 53860, 2047, 63, WACOM_22HD, 18, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x90 =
	{ "Wacom ISDv4 90",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325,  255,  0, TABLETPC };
static const struct wacom_features wacom_features_0x93 =
	{ "Wacom ISDv4 93",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325,  255,  0, TABLETPC };
static const struct wacom_features wacom_features_0x97 =
	{ "Wacom ISDv4 97",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325,  511,  0, TABLETPC };
static const struct wacom_features wacom_features_0x9A =
	{ "Wacom ISDv4 9A",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325,  255,  0, TABLETPC };
static const struct wacom_features wacom_features_0x9F =
	{ "Wacom ISDv4 9F",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325,  255,  0, TABLETPC };
static const struct wacom_features wacom_features_0xE2 =
	{ "Wacom ISDv4 E2",       WACOM_PKGLEN_TPC2FG,    26202, 16325,  255,  0, TABLETPC2FG };
static const struct wacom_features wacom_features_0xE3 =
	{ "Wacom ISDv4 E3",       WACOM_PKGLEN_TPC2FG,    26202, 16325,  255,  0, TABLETPC2FG };
static const struct wacom_features wacom_features_0xE6 =
	{ "Wacom ISDv4 E6",       WACOM_PKGLEN_TPC2FG,    27760, 15694,  255,  0, TABLETPC2FG };
static const struct wacom_features wacom_features_0x100 =
	{ "Wacom ISDv4 100",       WACOM_PKGLEN_MTTPC,    26202, 16325,  255,  0, MTTPC };
static const struct wacom_features wacom_features_0x101 =
	{ "Wacom ISDv4 101",       WACOM_PKGLEN_MTTPC,    26202, 16325,  255,  0, MTTPC };
static const struct wacom_features wacom_features_0x10D =
	{ "Wacom ISDv4 10D",      WACOM_PKGLEN_MTTPC,     26202, 16325,  255,  0, MTTPC };
static const struct wacom_features wacom_features_0x10F =
       { "Wacom ISDv4 10F",       WACOM_PKGLEN_MTTPC,     27760, 15694,  255,  0, MTTPC };
static const struct wacom_features wacom_features_0x116 =
       { "Wacom ISDv4 116",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325,  255,  0, TABLETPC };
static const struct wacom_features wacom_features_0x12C =
       { "Wacom ISDv4 12C",       WACOM_PKGLEN_GRAPHIRE,  27848, 15752, 2047,  0, TABLETPC };
static const struct wacom_features wacom_features_0x4001 =
	{ "Wacom ISDv4 4001",       WACOM_PKGLEN_MTTPC,   26202, 16325,  255,  0, MTTPC };
static const struct wacom_features wacom_features_0x4004 =
	{ "Wacom ISDv4 4004",       WACOM_PKGLEN_MTTPC,   11060, 6220,  255,  0, MTTPC_B };
static const struct wacom_features wacom_features_0x5000 =
	{ "Wacom ISDv4 5000",       WACOM_PKGLEN_MTTPC,   27848, 15752,  255,  0, MTTPC_B };
static const struct wacom_features wacom_features_0x5002 =
	{ "Wacom ISDv4 5002",       WACOM_PKGLEN_MTTPC,   29576, 16724,  1023,  0, MTTPC_B };
static const struct wacom_features wacom_features_0x5010 =
	{ "Wacom ISDv4 5010",       WACOM_PKGLEN_MTTPC,   13756, 7736,  1023,  0, MTTPC_B };
static const struct wacom_features wacom_features_0x5013 =
	{ "Wacom ISDv4 5013",      WACOM_PKGLEN_MTTPC,    11752, 6612,  1023,  0, MTTPC_B };
static const struct wacom_features wacom_features_0x5044 =
	{ "Wacom ISDv4 5044",      WACOM_PKGLEN_MTTPC,     27648, 15552, 2047, 0, MTTPC_C };
static const struct wacom_features wacom_features_0x5048 =
	{ "Wacom ISDv4 5048",      WACOM_PKGLEN_MTTPC,     27648, 15552, 2047, 0, MTTPC_C };
static const struct wacom_features wacom_features_0x5090 =
	{ "Wacom ISDv4 5090",      WACOM_PKGLEN_MTTPC,     27648, 15552, 2047, 0, MTTPC_C };
static const struct wacom_features wacom_features_0x47 =
	{ "Wacom Intuos2 6x8",    WACOM_PKGLEN_INTUOS,    20320, 16240, 1023, 31, INTUOS };
static const struct wacom_features wacom_features_0x6004 =
	{ "ISD-V4",               WACOM_PKGLEN_GRAPHIRE,  12800, 8000, 255, 0, TABLETPC };
static const struct wacom_features wacom_features_0x34A =
	{ "Wacom MobileStudio Pro 13 Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34d }; /* Touch */
static const struct wacom_features wacom_features_0x34B =
	{ "Wacom MobileStudio Pro 16 Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34e }; /* Touch */
static const struct wacom_features wacom_features_0x34D =
	{ "Wacom MobileStudio Pro 13", WACOM_PKGLEN_MSPRO, 59552, 33848, 8191, 63,
	  WACOM_MSPRO, 11, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34a };
static const struct wacom_features wacom_features_0x34E =
	{ "Wacom MobileStudio Pro 16", WACOM_PKGLEN_MSPRO, 69920, 39680, 8191, 63,
	  WACOM_MSPRO, 13, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34b };
static const struct wacom_features wacom_features_0x34F =
	{ "Wacom Cintiq Pro 13FHD", WACOM_PKGLEN_MSPRO, 59552, 33848, 8191, 63,
	  WACOM_MSPRO, 0, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x353 };
static const struct wacom_features wacom_features_0x350 =
	{ "Wacom Cintiq Pro 16UHD", WACOM_PKGLEN_MSPRO, 69920, 39680, 8191, 63,
	  WACOM_MSPRO, 0, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x354 };
static const struct wacom_features wacom_features_0x353 =
	{ "Wacom Cintiq Pro 13FHD Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34f }; /* Touch */
static const struct wacom_features wacom_features_0x354 =
	{ "Wacom Cintiq Pro 16UHD Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x350 }; /* Touch */
static const struct wacom_features wacom_features_0x357 =
	{ "Wacom Intuos Pro M", WACOM_PKGLEN_INTUOSP2, 44800, 29600, 8191, 63, INTUOSP2, 9 };
static const struct wacom_features wacom_features_0x358 =
	{ "Wacom Wacom Intuos Pro L", WACOM_PKGLEN_INTUOSP2, 62200, 43200, 8191, 63, INTUOSP2, 9 };
static const struct wacom_features wacom_features_0x35A =
	{ "Wacom DTH-1152", WACOM_PKGLEN_DTH1152, 22320, 12555, 1024,
	  0, DTH1152, .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x368 };
static const struct wacom_features wacom_features_0x368 =
	{ "Wacom DTH-1152 Touch", WACOM_PKGLEN_27QHDT, .type = DTH1152T,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x35a }; /* Touch */

#define USB_DEVICE_WACOM(prod)					\
	USB_DEVICE(USB_VENDOR_ID_WACOM, prod),			\
	.driver_info = (kernel_ulong_t)&wacom_features_##prod

#define USB_DEVICE_DETAILED(prod, class, sub, proto)			\
	USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_WACOM, prod, class,	\
				      sub, proto),			\
	.driver_info = (kernel_ulong_t)&wacom_features_##prod

#define USB_DEVICE_LENOVO(prod)					\
	USB_DEVICE(USB_VENDOR_ID_LENOVO, prod),			\
	.driver_info = (kernel_ulong_t)&wacom_features_##prod

const struct usb_device_id wacom_ids[] = {
	{ USB_DEVICE_WACOM(0x00) },
	{ USB_DEVICE_WACOM(0x10) },
	{ USB_DEVICE_WACOM(0x11) },
	{ USB_DEVICE_WACOM(0x12) },
	{ USB_DEVICE_WACOM(0x13) },
	{ USB_DEVICE_WACOM(0x14) },
	{ USB_DEVICE_WACOM(0x15) },
	{ USB_DEVICE_WACOM(0x16) },
	{ USB_DEVICE_WACOM(0x17) },
	{ USB_DEVICE_WACOM(0x18) },
	{ USB_DEVICE_WACOM(0x19) },
	{ USB_DEVICE_WACOM(0x60) },
	{ USB_DEVICE_WACOM(0x61) },
	{ USB_DEVICE_WACOM(0x62) },
	{ USB_DEVICE_WACOM(0x63) },
	{ USB_DEVICE_WACOM(0x64) },
	{ USB_DEVICE_WACOM(0x65) },
	{ USB_DEVICE_WACOM(0x69) },
	{ USB_DEVICE_WACOM(0x6A) },
	{ USB_DEVICE_WACOM(0x6B) },
	{ USB_DEVICE_WACOM(0x20) },
	{ USB_DEVICE_WACOM(0x21) },
	{ USB_DEVICE_WACOM(0x22) },
	{ USB_DEVICE_WACOM(0x23) },
	{ USB_DEVICE_WACOM(0x24) },
	{ USB_DEVICE_WACOM(0x30) },
	{ USB_DEVICE_WACOM(0x31) },
	{ USB_DEVICE_WACOM(0x32) },
	{ USB_DEVICE_WACOM(0x33) },
	{ USB_DEVICE_WACOM(0x34) },
	{ USB_DEVICE_WACOM(0x35) },
	{ USB_DEVICE_WACOM(0x37) },
	{ USB_DEVICE_WACOM(0x38) },
	{ USB_DEVICE_WACOM(0x39) },
	{ USB_DEVICE_WACOM(0xC4) },
	{ USB_DEVICE_WACOM(0xC0) },
	{ USB_DEVICE_WACOM(0xC2) },
	{ USB_DEVICE_WACOM(0x03) },
	{ USB_DEVICE_WACOM(0xD1) },
	{ USB_DEVICE_WACOM(0xD4) },
	{ USB_DEVICE_WACOM(0xD2) },
	{ USB_DEVICE_WACOM(0xD3) },
	{ USB_DEVICE_WACOM(0xD0) },
	{ USB_DEVICE_WACOM(0xD5) },
	{ USB_DEVICE_WACOM(0xD6) },
	{ USB_DEVICE_WACOM(0xD7) },
	{ USB_DEVICE_WACOM(0xD8) },
	{ USB_DEVICE_WACOM(0xDA) },
	{ USB_DEVICE_WACOM(0xDB) },
	{ USB_DEVICE_WACOM(0xDD) },
	{ USB_DEVICE_WACOM(0xDE) },
	{ USB_DEVICE_WACOM(0xDF) },
	{ USB_DEVICE_WACOM(0x41) },
	{ USB_DEVICE_WACOM(0x42) },
	{ USB_DEVICE_WACOM(0x43) },
	{ USB_DEVICE_WACOM(0x44) },
	{ USB_DEVICE_WACOM(0x45) },
	{ USB_DEVICE_WACOM(0x57) },
	{ USB_DEVICE_WACOM(0x59) },
	{ USB_DEVICE_WACOM(0x5B) },
	{ USB_DEVICE_WACOM(0xB0) },
	{ USB_DEVICE_WACOM(0xB1) },
	{ USB_DEVICE_WACOM(0xB2) },
	{ USB_DEVICE_WACOM(0xB3) },
	{ USB_DEVICE_WACOM(0xB4) },
	{ USB_DEVICE_WACOM(0xB5) },
	{ USB_DEVICE_WACOM(0xB7) },
	{ USB_DEVICE_WACOM(0xB8) },
	{ USB_DEVICE_WACOM(0xB9) },
	{ USB_DEVICE_WACOM(0xBA) },
	{ USB_DEVICE_WACOM(0xBB) },
	{ USB_DEVICE_WACOM(0xBC) },
	{ USB_DEVICE_WACOM(0x26) },
	{ USB_DEVICE_WACOM(0x27) },
	{ USB_DEVICE_WACOM(0x28) },
	{ USB_DEVICE_WACOM(0x29) },
	{ USB_DEVICE_WACOM(0x2A) },
	{ USB_DEVICE_WACOM(0x3F) },
	{ USB_DEVICE_WACOM(0xC5) },
	{ USB_DEVICE_WACOM(0xC6) },
	{ USB_DEVICE_WACOM(0xC7) },
	/*
	 * DTU-2231 has two interfaces on the same configuration,
	 * only one is used.
	 */
	{ USB_DEVICE_DETAILED(0xCE, USB_CLASS_HID,
			      USB_INTERFACE_SUBCLASS_BOOT,
			      USB_INTERFACE_PROTOCOL_MOUSE) },
	{ USB_DEVICE_WACOM(0xF0) },
	{ USB_DEVICE_WACOM(0xCC) },
	{ USB_DEVICE_WACOM(0x90) },
	{ USB_DEVICE_WACOM(0x93) },
	{ USB_DEVICE_WACOM(0x97) },
	{ USB_DEVICE_WACOM(0x9A) },
	{ USB_DEVICE_WACOM(0x9F) },
	{ USB_DEVICE_WACOM(0xE2) },
	{ USB_DEVICE_WACOM(0xE3) },
	{ USB_DEVICE_WACOM(0xE6) },
	{ USB_DEVICE_WACOM(0x100) },
	{ USB_DEVICE_WACOM(0x101) },
	{ USB_DEVICE_WACOM(0x10D) },
	{ USB_DEVICE_WACOM(0x10F) },
	{ USB_DEVICE_WACOM(0x116) },
	{ USB_DEVICE_WACOM(0x12C) },
	{ USB_DEVICE_WACOM(0x4001) },
	{ USB_DEVICE_WACOM(0x4004) },
	{ USB_DEVICE_WACOM(0x5000) },
	{ USB_DEVICE_WACOM(0x5002) },
	{ USB_DEVICE_WACOM(0x5010) },
	{ USB_DEVICE_WACOM(0x5013) },
	{ USB_DEVICE_WACOM(0x5044) },
	{ USB_DEVICE_WACOM(0x5048) },
	{ USB_DEVICE_WACOM(0x5090) },
	{ USB_DEVICE_WACOM(0x300) },
	{ USB_DEVICE_WACOM(0x301) },
	{ USB_DEVICE_DETAILED(0x302, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x303, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x30E, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x304) },
	{ USB_DEVICE_DETAILED(0x323, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x314, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x315, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x317, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x333) },
	{ USB_DEVICE_WACOM(0x336) },
	{ USB_DEVICE_DETAILED(0x33B, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x33C, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x33D, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x33E, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x47) },
	{ USB_DEVICE_WACOM(0xF4) },
	{ USB_DEVICE_WACOM(0xF8) },
	{ USB_DEVICE_WACOM(0xFA) },
	{ USB_DEVICE_WACOM(0xFB) },
	{ USB_DEVICE_WACOM(0x32A) },
	{ USB_DEVICE_WACOM(0x32B) },
	{ USB_DEVICE_WACOM(0x32F) },
	{ USB_DEVICE_WACOM(0x343) },
	{ USB_DEVICE_WACOM(0x34A) },
	{ USB_DEVICE_WACOM(0x34B) },
	{ USB_DEVICE_WACOM(0x34D) },
	{ USB_DEVICE_WACOM(0x34E) },
	{ USB_DEVICE_WACOM(0x34F) },
	{ USB_DEVICE_WACOM(0x350) },
	{ USB_DEVICE_WACOM(0x353) },
	{ USB_DEVICE_WACOM(0x354) },
	{ USB_DEVICE_DETAILED(0x357, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x358, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x35A) },
	{ USB_DEVICE_WACOM(0x368) },
	{ USB_DEVICE_LENOVO(0x6004) },
	{ }
};
MODULE_DEVICE_TABLE(usb, wacom_ids);
