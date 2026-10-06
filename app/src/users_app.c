/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 *
 * User store of the demo: the gzm user_mgr library on LittleFS. The
 * partition comes from the devicetree (chosen app,users-partition): the
 * SPI NOR on the board, the flash simulator on native_sim.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#include <gzm/fs_mgr.h>
#include <gzm/user_mgr.h>

#include "users_app.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(users_app, CONFIG_LOG_DEFAULT_LEVEL);

#define USERS_PARTITION DT_CHOSEN(app_users_partition)
#define USERS_MNT       "/lfs"
#define USERS_DIR       USERS_MNT "/users"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(users_lfs);

static struct fs_mount_t users_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &users_lfs,
	.storage_dev = (void *)DT_FIXED_PARTITION_ID(USERS_PARTITION),
	.mnt_point = USERS_MNT,
};

int users_app_init(void)
{
	int ret;

	ret = fs_mgr_init(&users_mnt);
	if (ret) {
		LOG_ERR("LittleFS on %s: %d", USERS_MNT, ret);
		return ret;
	}

	ret = user_mgr_init(USERS_DIR);
	if (ret) {
		LOG_ERR("user_mgr_init: %d", ret);
		return ret;
	}

	/* Only on an empty store: the first boot, or after a format. */
	ret = user_mgr_create_default_admin(CONFIG_APP_ADMIN_ID, CONFIG_APP_ADMIN_PASSWORD,
					    ACC_LEVEL_ADMIN);
	if (ret == 0) {
		LOG_INF("default admin %d created", CONFIG_APP_ADMIN_ID);
	} else if (ret != -EEXIST) {
		LOG_ERR("default admin: %d", ret);
	}

	LOG_INF("%zu users (%zu blocked), max %d", user_mgr_count(), user_mgr_count_blocked(),
		CONFIG_GZM_USER_MGR_MAX);
	return 0;
}

bool users_app_ready(void)
{
	return user_mgr_is_ready();
}

size_t users_app_count(void)
{
	return user_mgr_count();
}

size_t users_app_blocked(void)
{
	return user_mgr_count_blocked();
}
