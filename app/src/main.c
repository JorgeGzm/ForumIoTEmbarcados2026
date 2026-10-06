/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include "setup.h"

/* MCUboot starts the IWDG (20 s) and it cannot be stopped: the main loop
 * must keep feeding it. */
#if defined(CONFIG_WATCHDOG) && DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
#define WDT_FEED() wdt_feed(wdt, 0)
#else
#define WDT_FEED()
#endif

int main(void)
{
	setup_init();

	while (1) {
		uint32_t sleep_ms = lv_timer_handler();

		WDT_FEED();
		k_msleep(MIN(sleep_ms, 50U));
	}

	return 0;
}
