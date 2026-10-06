/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_BOOT_H_
#define APP_BOOT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

struct boot_slot_info {
	int slot;
	uint32_t offset;
	uint32_t size;
	uint32_t image_size;
	uint8_t version_major;
	uint8_t version_minor;
	uint16_t version_revision;
	uint32_t version_build;
	uint8_t hash[32];          /* SHA256 from the image TLV */
	char label[24];
	bool valid_header;
	bool has_hash;
	bool confirmed;            /* slot0: image confirmed */
	bool active;               /* slot0: running image */
	bool pending;              /* slot1: upgrade pending */
};

int boot_get_slot_count(void);

/** @brief Confirm the running image if it is not confirmed yet. */
void boot_check_and_confirm_image(void);

bool boot_is_image_confirmed(void);

int boot_erase_slot1(void);

/** @brief Fill @p info with header/flags/hash of the given slot. */
int boot_get_slot_info(int slot, struct boot_slot_info *info);

#ifdef __cplusplus
}
#endif

#endif /* APP_BOOT_H_ */
