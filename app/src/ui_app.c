/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#include <lvgl.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <zephyr/shell/shell.h>

#include <app_version.h>

#include "boot.h"
#include "ui_app.h"
#include "users_app.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ui_app, CONFIG_LOG_DEFAULT_LEVEL);

#define UI_REFRESH_MS 200
#define UI_SPLASH_MS  3000

LV_IMAGE_DECLARE(gzm_logo_160);

static lv_obj_t *label_users;
static lv_obj_t *label_blocked;
static lv_obj_t *label_version;
static lv_obj_t *splash_bar;
static uint32_t splash_start;

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), backlight_gpios)
static const struct gpio_dt_spec backlight =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), backlight_gpios);
#endif

/* Shell requests are applied from the LVGL timer context: LVGL is not
 * thread-safe. */
enum ui_request { UI_REQ_NONE, UI_REQ_PATTERN, UI_REQ_LOGO, UI_REQ_MAIN };
static volatile enum ui_request ui_request;

static void ui_main_create(void);

/* The firmware major version picks the colors (see ui_app_init), so an
 * update between an odd and an even version is visible at a glance. On
 * yellow the texts switch to dark tones to stay readable. */
struct ui_theme {
	uint32_t splash_bar;
	uint32_t main_bg;
	uint32_t title;
	uint32_t caption;
	uint32_t value;
	uint32_t value_full;
	uint32_t blocked;
	uint32_t confirmed;
	uint32_t testing;
};

static const struct ui_theme theme_odd = {
	.splash_bar = 0xffffff, .main_bg = 0x101418, .title = 0x80d8ff,
	.caption = 0x9e9e9e, .value = 0xffffff, .value_full = 0xff8a80,
	.blocked = 0xffab00, .confirmed = 0x00c853, .testing = 0xffab00,
};

static const struct ui_theme theme_even = {
	.splash_bar = 0xffd600, .main_bg = 0xffd600, .title = 0x0d47a1,
	.caption = 0x424242, .value = 0x101418, .value_full = 0xb71c1c,
	.blocked = 0xbf360c, .confirmed = 0x1b5e20, .testing = 0xbf360c,
};

static const struct ui_theme *theme = &theme_odd;

static void ui_refresh_cb(lv_timer_t *timer)
{
	ARG_UNUSED(timer);
	char text[32];

	if (label_users == NULL || label_blocked == NULL || label_version == NULL) {
		return;
	}

	if (users_app_ready()) {
		size_t n = users_app_count();
		size_t blocked = users_app_blocked();

		snprintf(text, sizeof(text), "%zu/%d", n, CONFIG_GZM_USER_MGR_MAX);
		lv_label_set_text(label_users, text);
		/* Full store: the next "user add" is refused (-ENOSPC). */
		lv_obj_set_style_text_color(label_users,
					    lv_color_hex(n >= CONFIG_GZM_USER_MGR_MAX
								 ? theme->value_full
								 : theme->value),
					    LV_PART_MAIN);
		snprintf(text, sizeof(text), "%zu", blocked);
		lv_label_set_text(label_blocked, text);
	} else {
		lv_label_set_text(label_users, "--");
		lv_label_set_text(label_blocked, "--");
	}

#ifdef CONFIG_MCUBOOT_IMG_MANAGER
	/* Reading the state goes to flash: once confirmed it cannot revert until
	 * the next boot, so stop polling; while in test, poll once per second. */
	static bool confirmed;
	static uint32_t polls;

	if (!confirmed && (polls++ % (1000 / UI_REFRESH_MS)) == 0) {
		confirmed = boot_is_image_confirmed();
	}
	const char *state = confirmed ? "OK" : "TEST";
	lv_color_t color = lv_color_hex(confirmed ? theme->confirmed : theme->testing);
#else
	const char *state = "SIM";
	lv_color_t color = lv_color_hex(theme->title);
#endif

	snprintf(text, sizeof(text), "v%s %s", APP_VERSION_STRING, state);
	lv_label_set_text(label_version, text);
	lv_obj_set_style_text_color(label_version, color, LV_PART_MAIN);
}

static void ui_main_create(void)
{
	lv_obj_t *screen = lv_screen_active();

	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

	lv_obj_t *title = lv_label_create(screen);

	lv_label_set_text(title, "DO CODIGO AO CAMPO");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(title, lv_color_hex(theme->title), LV_PART_MAIN);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

	lv_obj_t *cap_users = lv_label_create(screen);

	lv_label_set_text(cap_users, "usuarios");
	lv_obj_set_style_text_font(cap_users, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(cap_users, lv_color_hex(theme->caption), LV_PART_MAIN);
	lv_obj_align(cap_users, LV_ALIGN_LEFT_MID, 8, -16);

	label_users = lv_label_create(screen);
	lv_label_set_text(label_users, "--");
	lv_obj_set_style_text_font(label_users, &lv_font_montserrat_24, LV_PART_MAIN);
	lv_obj_set_style_text_color(label_users, lv_color_hex(theme->value), LV_PART_MAIN);
	lv_obj_align(label_users, LV_ALIGN_LEFT_MID, 8, 6);

	lv_obj_t *cap_blocked = lv_label_create(screen);

	lv_label_set_text(cap_blocked, "bloqueados");
	lv_obj_set_style_text_font(cap_blocked, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(cap_blocked, lv_color_hex(theme->caption), LV_PART_MAIN);
	lv_obj_align(cap_blocked, LV_ALIGN_RIGHT_MID, -8, -16);

	label_blocked = lv_label_create(screen);
	lv_label_set_text(label_blocked, "--");
	lv_obj_set_style_text_font(label_blocked, &lv_font_montserrat_24, LV_PART_MAIN);
	lv_obj_set_style_text_color(label_blocked, lv_color_hex(theme->blocked), LV_PART_MAIN);
	lv_obj_align(label_blocked, LV_ALIGN_RIGHT_MID, -8, 6);

	label_version = lv_label_create(screen);
	lv_label_set_text(label_version, "v" APP_VERSION_STRING);
	lv_obj_align(label_version, LV_ALIGN_BOTTOM_MID, 0, -2);

	lv_timer_create(ui_refresh_cb, UI_REFRESH_MS, NULL);
}

/* R/G/B/W bars: any panel color transform (byte swap, BGR, inversion)
 * shows up unambiguously. */
static void ui_pattern_create(void)
{
	static const struct { uint32_t color; const char *tag; } bars[] = {
		{ 0xff0000, "R" }, { 0x00ff00, "G" },
		{ 0x0000ff, "B" }, { 0xffffff, "W" },
	};
	lv_obj_t *screen = lv_screen_active();

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

	for (int i = 0; i < 4; i++) {
		lv_obj_t *rect = lv_obj_create(screen);

		lv_obj_set_size(rect, 40, 50);
		lv_obj_set_pos(rect, i * 40, 0);
		lv_obj_set_style_bg_color(rect, lv_color_hex(bars[i].color),
					  LV_PART_MAIN);
		lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_radius(rect, 0, LV_PART_MAIN);
		lv_obj_set_style_border_width(rect, 0, LV_PART_MAIN);

		lv_obj_t *tag = lv_label_create(rect);

		lv_label_set_text(tag, bars[i].tag);
		lv_obj_set_style_text_color(tag, lv_color_hex(0x000000), LV_PART_MAIN);
		lv_obj_center(tag);
	}

	lv_obj_t *strip = lv_obj_create(screen);

	lv_obj_set_size(strip, 160, 16);
	lv_obj_set_pos(strip, 0, 52);
	lv_obj_set_style_bg_color(strip, lv_color_hex(0x383838), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_radius(strip, 0, LV_PART_MAIN);
	lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);

	lv_obj_t *note = lv_label_create(screen);

	lv_label_set_text(note, "R G B W + cinza");
	lv_obj_set_style_text_color(note, lv_color_hex(0xffffff), LV_PART_MAIN);
	lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void ui_logo_create(void)
{
	lv_obj_t *screen = lv_screen_active();

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x383838), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

	lv_obj_t *logo = lv_image_create(screen);

	lv_image_set_src(logo, &gzm_logo_160);
	lv_obj_center(logo);
}

static void ui_request_cb(lv_timer_t *timer)
{
	ARG_UNUSED(timer);
	enum ui_request req = ui_request;

	if (req == UI_REQ_NONE) {
		return;
	}
	ui_request = UI_REQ_NONE;

	label_users = NULL;
	label_blocked = NULL;
	label_version = NULL;
	lv_obj_clean(lv_screen_active());

	switch (req) {
	case UI_REQ_PATTERN:
		ui_pattern_create();
		break;
	case UI_REQ_LOGO:
		ui_logo_create();
		break;
	default:
		lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(theme->main_bg),
					  LV_PART_MAIN);
		lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, LV_PART_MAIN);
		ui_main_create();
		break;
	}
}

static int cmd_ui(const struct shell *shell, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(shell, "Usage: ui <pattern|logo|main>");
		return -EINVAL;
	}
	if (strcmp(argv[1], "pattern") == 0) {
		ui_request = UI_REQ_PATTERN;
	} else if (strcmp(argv[1], "logo") == 0) {
		ui_request = UI_REQ_LOGO;
	} else {
		ui_request = UI_REQ_MAIN;
	}
	shell_print(shell, "ui: switching to %s", argv[1]);
	return 0;
}

SHELL_CMD_REGISTER(ui, NULL, "Switch UI screen (pattern|logo|main)", cmd_ui);

static void splash_tick_cb(lv_timer_t *timer)
{
	uint32_t elapsed = lv_tick_elaps(splash_start);
	int32_t pct = (int32_t)((elapsed * 100U) / UI_SPLASH_MS);

	if (pct >= 100) {
		lv_bar_set_value(splash_bar, 100, LV_ANIM_OFF);
		lv_timer_delete(timer);
		lv_obj_clean(lv_screen_active());
		lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(theme->main_bg),
					  LV_PART_MAIN);
		lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, LV_PART_MAIN);
		ui_main_create();
		return;
	}

	lv_bar_set_value(splash_bar, pct, LV_ANIM_OFF);
}

int ui_app_init(uint8_t fw_major)
{
	lv_obj_t *screen = lv_screen_active();

	theme = (fw_major % 2) ? &theme_odd : &theme_even;

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x383838), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

	lv_obj_t *logo = lv_image_create(screen);

	lv_image_set_src(logo, &gzm_logo_160);
	lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 2);

	splash_bar = lv_bar_create(screen);
	lv_obj_set_size(splash_bar, 140, 8);
	lv_obj_align(splash_bar, LV_ALIGN_BOTTOM_MID, 0, -6);
	lv_bar_set_range(splash_bar, 0, 100);
	lv_bar_set_value(splash_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_bg_opa(splash_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(splash_bar, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(splash_bar, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
	lv_obj_set_style_radius(splash_bar, 4, LV_PART_MAIN);
	lv_obj_set_style_bg_color(splash_bar, lv_color_hex(theme->splash_bar),
				  LV_PART_INDICATOR);
	lv_obj_set_style_radius(splash_bar, 4, LV_PART_INDICATOR);

	splash_start = lv_tick_get();
	lv_timer_create(splash_tick_cb, 30, NULL);
	lv_timer_create(ui_request_cb, 100, NULL);

	/* Draw now so the backlight never shows a partial frame. The SDL
	 * display on native_sim stays blank until blanking is turned off. */
	lv_refr_now(NULL);
	display_blanking_off(DEVICE_DT_GET(DT_CHOSEN(zephyr_display)));

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), backlight_gpios)
	if (gpio_is_ready_dt(&backlight)) {
		gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
	}
#endif

	return 0;
}
