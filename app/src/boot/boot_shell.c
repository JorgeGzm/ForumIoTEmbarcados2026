/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include <app_version.h>

#include "boot.h"

static void print_hash(const struct shell *shell, const uint8_t *hash)
{
	shell_fprintf(shell, SHELL_NORMAL, "    hash: ");
	for (int i = 0; i < 32; i++) {
		shell_fprintf(shell, SHELL_NORMAL, "%02x", hash[i]);
	}
	shell_fprintf(shell, SHELL_NORMAL, "\n");
}

static int cmd_boot_confirm(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ARG_UNUSED(shell);

	boot_check_and_confirm_image();
	return 0;
}

static int cmd_boot_reboot(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "Rebooting system in 1 second...");
	k_msleep(1000);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

static int cmd_boot_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int slot_count = boot_get_slot_count();
	struct boot_slot_info info;
	int ret;

	shell_print(shell, "=== MCUboot Image Status ===");
	shell_print(shell, "");

	for (int i = 0; i < slot_count; i++) {
		ret = boot_get_slot_info(i, &info);
		if (ret != 0) {
			shell_error(shell, " slot=%d: failed to read info (%d)", i, ret);
			continue;
		}

		shell_print(shell, " slot=%d [%s]", i, info.label);

		if (info.valid_header) {
			shell_print(shell, "    version: %d.%d.%d+%d",
				    info.version_major, info.version_minor,
				    info.version_revision, info.version_build);
			shell_print(shell, "    image size: %u bytes", info.image_size);
		} else {
			shell_print(shell, "    version: <no valid header>");
		}

		shell_print(shell, "    partition: offset=0x%08x size=%u KB",
			    info.offset, info.size / 1024);
		shell_print(shell, "    bootable: %s", info.valid_header ? "true" : "false");

		if (i == 0) {
			shell_print(shell, "    active: %s", info.active ? "true" : "false");
			shell_print(shell, "    confirmed: %s",
				    info.confirmed ? "true" : "false");
		} else {
			shell_print(shell, "    pending: %s", info.pending ? "true" : "false");
		}

		if (info.has_hash) {
			print_hash(shell, info.hash);
		}
	}

	return 0;
}

static int cmd_boot_erase_slot1(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "Erasing slot1 partition (this may take a while)...");

	int ret = boot_erase_slot1();

	if (ret < 0) {
		shell_error(shell, "Failed to erase slot1: %d", ret);
		return ret;
	}

	shell_print(shell, "Slot1 erased successfully!");
	shell_print(shell, "Next firmware update will be faster (write only, no erase).");

	return 0;
}

static int cmd_boot_version(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "app version: v%s (%s)", APP_VERSION_STRING,
		    boot_is_image_confirmed() ? "confirmed" : "test");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_boot,
	SHELL_CMD(confirm, NULL, "Confirm current firmware image", cmd_boot_confirm),
	SHELL_CMD(status, NULL, "Show boot/image status", cmd_boot_status),
	SHELL_CMD(version, NULL, "Show application version", cmd_boot_version),
	SHELL_CMD(reboot, NULL, "Reboot the system", cmd_boot_reboot),
	SHELL_CMD(erase_slot1, NULL, "Erase slot1 partition", cmd_boot_erase_slot1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(boot, &sub_boot, "Boot and firmware update commands", NULL);
