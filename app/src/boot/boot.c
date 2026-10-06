/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#include "boot.h"

#define SLOT0_PARTITION_ID PARTITION_ID(slot0_partition)
#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

struct slot_entry {
	uint8_t partition_id;
	const char *label;
};

static const struct slot_entry slot_table[] = {
	{ SLOT0_PARTITION_ID, "image-0" },
	{ SLOT1_PARTITION_ID, "image-1" },
};

#define SLOT_COUNT ARRAY_SIZE(slot_table)

void boot_check_and_confirm_image(void)
{
	if (boot_is_img_confirmed()) {
		printk("Current firmware image is already confirmed.\n");
		return;
	}

	printk("Current firmware image is not confirmed. Confirming now...\n");

	int ret = boot_write_img_confirmed();

	if (ret == 0) {
		printk("Firmware image confirmed successfully.\n");
	} else {
		printk("Failed to confirm the firmware image. Error: %d\n", ret);
	}
}

bool boot_is_image_confirmed(void)
{
	return boot_is_img_confirmed();
}

int boot_erase_slot1(void)
{
	uint32_t start_time = k_uptime_get_32();
	int result;

	printk("Erasing slot1 partition...\n");

	result = boot_erase_img_bank(SLOT1_PARTITION_ID);
	if (result) {
		printk("Failed to erase slot1: %d\n", result);
	} else {
		printk("Slot1 erased successfully in %d ms\n",
		       (k_uptime_get_32() - start_time));
	}

	return result;
}

int boot_get_slot_count(void)
{
	return (int)SLOT_COUNT;
}

int boot_get_slot_info(int slot, struct boot_slot_info *info)
{
	if (info == NULL || slot < 0 || slot >= (int)SLOT_COUNT) {
		return -EINVAL;
	}

	memset(info, 0, sizeof(*info));
	info->slot = slot;

	uint8_t partition_id = slot_table[slot].partition_id;

	strncpy(info->label, slot_table[slot].label, sizeof(info->label) - 1);

	const struct flash_area *fa;
	int ret = flash_area_open(partition_id, &fa);

	if (ret) {
		return ret;
	}

	info->size = fa->fa_size;
	info->offset = fa->fa_off;

	struct mcuboot_img_header header;

	ret = boot_read_bank_header(partition_id, &header, sizeof(header));
	if (ret == 0 && header.mcuboot_version == 1) {
		info->valid_header = true;
		info->version_major = header.h.v1.sem_ver.major;
		info->version_minor = header.h.v1.sem_ver.minor;
		info->version_revision = header.h.v1.sem_ver.revision;
		info->version_build = header.h.v1.sem_ver.build_num;
		info->image_size = header.h.v1.image_size;
	}

	if (slot == 0) {
		info->confirmed = boot_is_img_confirmed();
		info->active = true;
	} else {
		info->pending = info->valid_header;
	}

	/* Assumes the imgtool default 32-byte header; a protected-TLV block
	 * (0x6907) may precede the one holding the SHA256. */
	if (info->valid_header && info->image_size > 0) {
		uint32_t tlv_offset = header.h.v1.image_size + 32;
		uint8_t tlv_buf[40];

		ret = flash_area_read(fa, tlv_offset, tlv_buf, sizeof(tlv_buf));
		if (ret == 0) {
			uint16_t tlv_type = tlv_buf[0] | (tlv_buf[1] << 8);
			uint16_t tlv_len = tlv_buf[2] | (tlv_buf[3] << 8);

			if (tlv_type == 0x6907) {
				ret = flash_area_read(fa, tlv_offset + 4, tlv_buf,
						      sizeof(tlv_buf));
				if (ret == 0) {
					tlv_type = tlv_buf[0] | (tlv_buf[1] << 8);
					tlv_len = tlv_buf[2] | (tlv_buf[3] << 8);
				}
			}

			if (tlv_type == 0x10 && tlv_len == 32) {
				memcpy(info->hash, &tlv_buf[4], 32);
				info->has_hash = true;
			}
		}
	}

	flash_area_close(fa);
	return 0;
}
