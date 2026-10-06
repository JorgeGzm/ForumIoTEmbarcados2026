/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>

#include <app_version.h>

#include "setup.h"
#include "ui_app.h"
#include "users_app.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(setup, CONFIG_LOG_DEFAULT_LEVEL);

#ifdef CONFIG_USB_DEVICE_STACK_NEXT
USBD_DEVICE_DEFINE(usbd_device,
		   DEVICE_DT_GET(DT_NODELABEL(usbotg_fs)),
		   0x1209, 0x0001);

USBD_DESC_LANG_DEFINE(usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "GZM Embedded Systems");
USBD_DESC_PRODUCT_DEFINE(usb_product, "Do Codigo ao Campo Demo");
USBD_DESC_CONFIG_DEFINE(usb_fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(usb_fs_config, USB_SCD_SELF_POWERED, 100, &usb_fs_cfg_desc);
#endif /* CONFIG_USB_DEVICE_STACK_NEXT */

static void setup_sanity_init(void)
{
	printk("Do Codigo ao Campo demo v%s\r\n", APP_VERSION_STRING);
}

static int setup_usb_init(void)
{
#ifndef CONFIG_USB_DEVICE_STACK_NEXT
	return 0;
#else
	int ret;

	ret = usbd_add_descriptor(&usbd_device, &usb_lang);
	if (ret) {
		LOG_ERR("USB lang descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(&usbd_device, &usb_mfr);
	if (ret) {
		LOG_ERR("USB mfr descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(&usbd_device, &usb_product);
	if (ret) {
		LOG_ERR("USB product descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_configuration(&usbd_device, USBD_SPEED_FS, &usb_fs_config);
	if (ret) {
		LOG_ERR("USB FS config: %d", ret);
		return ret;
	}

	ret = usbd_register_all_classes(&usbd_device, USBD_SPEED_FS, 1, NULL);
	if (ret) {
		LOG_ERR("USB register classes: %d", ret);
		return ret;
	}

	usbd_device_set_code_triple(&usbd_device, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	ret = usbd_init(&usbd_device);
	if (ret) {
		LOG_ERR("usbd_init: %d", ret);
		return ret;
	}

	ret = usbd_enable(&usbd_device);
	if (ret) {
		LOG_ERR("usbd_enable: %d", ret);
		return ret;
	}

	return 0;
#endif /* CONFIG_USB_DEVICE_STACK_NEXT */
}

void setup_init(void)
{
	int ret;

	setup_sanity_init();

	ret = setup_usb_init();
	if (ret) {
		LOG_ERR("USB console init failed: %d", ret);
	}

	ret = users_app_init();
	if (ret) {
		LOG_ERR("users_app init failed: %d", ret);
	}

	ret = ui_app_init(APP_VERSION_MAJOR);
	if (ret) {
		LOG_ERR("ui_app init failed: %d", ret);
	}

	LOG_INF("setup done");
}
