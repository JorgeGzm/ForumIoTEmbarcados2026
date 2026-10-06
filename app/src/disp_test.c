/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fills the panel through the display API, bypassing LVGL: separates panel
 * or byte-order problems from LVGL rendering problems.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include <stdlib.h>
#include <string.h>

#define DISP_W 160
#define DISP_H 80
#define STRIP_H 8

static uint16_t strip[DISP_W * STRIP_H];

struct color_map {
	const char *name;
	uint16_t rgb565; /* natural value, R in the top bits */
};

static const struct color_map colors[] = {
	{ "red",   0xF800 }, { "green", 0x07E0 }, { "blue",  0x001F },
	{ "white", 0xFFFF }, { "black", 0x0000 }, { "gray",  0x39C7 },
};

static int fill(const struct shell *shell, const char *name, bool swap)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct color_map *c = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(colors); i++) {
		if (strcmp(name, colors[i].name) == 0) {
			c = &colors[i];
			break;
		}
	}
	if (c == NULL) {
		shell_error(shell, "colors: red green blue white black gray");
		return -EINVAL;
	}
	if (!device_is_ready(disp)) {
		shell_error(shell, "display not ready");
		return -ENODEV;
	}

	/* Panel wire order is big-endian; "fillswap" sends CPU-native LE. */
	uint16_t v = swap ? c->rgb565 : sys_cpu_to_be16(c->rgb565);

	for (size_t i = 0; i < ARRAY_SIZE(strip); i++) {
		strip[i] = v;
	}

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(strip),
		.width = DISP_W,
		.height = STRIP_H,
		.pitch = DISP_W,
	};

	for (int y = 0; y < DISP_H; y += STRIP_H) {
		int ret = display_write(disp, 0, y, &desc, strip);

		if (ret) {
			shell_error(shell, "display_write: %d", ret);
			return ret;
		}
	}

	shell_print(shell, "filled %s%s (0x%04x)", name,
		    swap ? " (swapped)" : "", v);
	return 0;
}

static int cmd_fill(const struct shell *shell, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(shell, "Usage: disp fill <color>");
		return -EINVAL;
	}
	return fill(shell, argv[1], false);
}

static int cmd_fillswap(const struct shell *shell, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(shell, "Usage: disp fillswap <color>");
		return -EINVAL;
	}
	return fill(shell, argv[1], true);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_disp,
	SHELL_CMD(fill, NULL, "Fill screen via display API (no LVGL)", cmd_fill),
	SHELL_CMD(fillswap, NULL, "Fill with byte-swapped color", cmd_fillswap),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(disp, &sub_disp, "Raw display tests", NULL);
