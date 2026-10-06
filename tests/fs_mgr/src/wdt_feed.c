/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 *
 * On the board MCUboot starts the IWDG (20 s) and it cannot be stopped:
 * the test image must keep feeding it, like the app does in main.c,
 * or the board resets in the middle of the longer tests.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_WATCHDOG) && DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static void wdt_feed_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(wdt_feed_work, wdt_feed_fn);

static void wdt_feed_fn(struct k_work *work)
{
	wdt_feed(wdt, 0);
	k_work_schedule(&wdt_feed_work, K_SECONDS(1));
}

static int wdt_feed_start(void)
{
	k_work_schedule(&wdt_feed_work, K_NO_WAIT);
	return 0;
}

SYS_INIT(wdt_feed_start, APPLICATION, 0);
#endif
