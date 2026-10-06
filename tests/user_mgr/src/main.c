/*
 * Copyright (c) 2026 GZM Embarcados
 *
 * Unit tests for gzm/user_mgr.  Covers CRUD, authentication,
 * persistence across reinits, the in-RAM sorted index (including
 * non-sequential ids) and validation.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#include <gzm/user_mgr.h>

#include "test_storage.h"

#define USERS_DIR TEST_MNT "/users"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(test_storage);

/* Same partition as the app: chosen app,users-partition (SPI NOR on the
 * board, flash simulator on native_sim). */
static struct fs_mount_t lfs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &test_storage,
	.storage_dev = (void *)DT_FIXED_PARTITION_ID(DT_CHOSEN(app_users_partition)),
	.mnt_point = TEST_MNT,
};

int test_storage_mount(void)
{
	int ret = fs_mount(&lfs_mnt);

	if (ret == -EBUSY) {
		return 0;
	}
	if (ret != 0) {
		ret = fs_mkfs(FS_LITTLEFS, (uintptr_t)lfs_mnt.storage_dev, NULL, 0);
		if (ret == 0) {
			ret = fs_mount(&lfs_mnt);
		}
	}
	return ret;
}

int test_storage_remount(void)
{
	int ret = fs_unmount(&lfs_mnt);

	return ret ? ret : fs_mount(&lfs_mnt);
}

/* Fixture -------------------------------------------------------------- */

void test_storage_wipe_dir(const char *dir)
{
	struct fs_dir_t d;
	struct fs_dirent ent;
	char path[64];
	int n;

	fs_dir_t_init(&d);
	if (fs_opendir(&d, dir) != 0) {
		return;
	}
	while (fs_readdir(&d, &ent) == 0 && ent.name[0] != '\0') {
		if (ent.type != FS_DIR_ENTRY_FILE) {
			continue;
		}
		n = snprintf(path, sizeof(path), "%s/%s", dir, ent.name);
		if (n > 0 && (size_t)n < sizeof(path)) {
			(void)fs_unlink(path);
		}
	}
	(void)fs_closedir(&d);
}

static void wipe_users_dir(void)
{
	test_storage_wipe_dir(USERS_DIR);
}

static void *suite_setup(void)
{
	int ret = test_storage_mount();

	zassert_ok(ret, "LittleFS mount on %s failed: %d", TEST_MNT, ret);
	return NULL;
}

static void suite_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	wipe_users_dir();
	/* The mount stays in place for the user_mgr_limit suite. */
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	wipe_users_dir();
	zassert_ok(user_mgr_init(USERS_DIR), "user_mgr_init failed");
}

ZTEST_SUITE(user_mgr, NULL, suite_setup, before_each, NULL, suite_teardown);

/* Lifecycle ------------------------------------------------------------ */

ZTEST(user_mgr, test_init_empty_store)
{
	zassert_equal(user_mgr_count(), 0);
	zassert_equal(user_mgr_count_admins_enabled(), 0);
}

ZTEST(user_mgr, test_init_bad_args)
{
	zassert_equal(user_mgr_init(NULL), -EINVAL);
}

/* Default admin -------------------------------------------------------- */

ZTEST(user_mgr, test_default_admin_seeds_when_empty)
{
	struct user_public view;

	zassert_ok(user_mgr_create_default_admin(7777, "hunter2", ACC_LEVEL_ADMIN));
	zassert_equal(user_mgr_count(), 1);
	zassert_equal(user_mgr_count_admins_enabled(), 1);

	zassert_ok(user_mgr_get_by_id(7777, &view));
	zassert_equal(view.user_id, 7777);
	zassert_equal(view.level, ACC_LEVEL_ADMIN);
	zassert_equal(view.status, USER_STATUS_ENABLED);
	zassert_str_equal(view.first_name, "admin");
	zassert_str_equal(view.last_name, "");
}

ZTEST(user_mgr, test_default_admin_rejects_non_empty_store)
{
	zassert_ok(user_mgr_add(1, "Joao", "Silva", "pwd", ACC_LEVEL_USER));
	zassert_equal(user_mgr_create_default_admin(2, "pwd", ACC_LEVEL_ADMIN), -EEXIST);
}

/* Add / duplicate id --------------------------------------------------- */

ZTEST(user_mgr, test_add_and_get)
{
	struct user_public view;

	zassert_ok(user_mgr_add(42, "Alice", "Lima", "s3cret", ACC_LEVEL_MANAGER));
	zassert_equal(user_mgr_count(), 1);

	zassert_ok(user_mgr_get_by_id(42, &view));
	zassert_equal(view.user_id, 42);
	zassert_str_equal(view.first_name, "Alice");
	zassert_str_equal(view.last_name, "Lima");
	zassert_equal(view.level, ACC_LEVEL_MANAGER);
	zassert_equal(view.status, USER_STATUS_ENABLED);
}

ZTEST(user_mgr, test_add_duplicate_id_rejected)
{
	zassert_ok(user_mgr_add(10, "Bob", "", "p", ACC_LEVEL_USER));
	zassert_equal(user_mgr_add(10, "Other", "", "p", ACC_LEVEL_USER), -EEXIST);
	zassert_equal(user_mgr_count(), 1);
}

ZTEST(user_mgr, test_add_non_sequential_ids_stay_sorted)
{
	struct user_public view;
	uint32_t ids[] = {500, 5, 1000, 1, 750, 42};
	size_t i;
	uint32_t last;

	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		zassert_ok(user_mgr_add(ids[i], "u", "", "p", ACC_LEVEL_USER));
	}
	zassert_equal(user_mgr_count(), ARRAY_SIZE(ids));

	last = 0;
	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		zassert_ok(user_mgr_get_by_index(i, &view));
		zassert_true(view.user_id > last, "index not sorted at %zu: %u <= %u", i,
			     view.user_id, last);
		last = view.user_id;
	}
}

/* Input validation ----------------------------------------------------- */

ZTEST(user_mgr, test_add_validation)
{
	char long_name[USER_MGR_FIRST_NAME_LEN + 5];

	memset(long_name, 'a', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';

	/* empty first name */
	zassert_equal(user_mgr_add(1, "", "", "p", ACC_LEVEL_USER), -EINVAL);

	/* first name too long */
	zassert_equal(user_mgr_add(1, long_name, "", "p", ACC_LEVEL_USER), -EINVAL);

	/* level out of range */
	zassert_equal(user_mgr_add(1, "ok", "", "p", ACC_LEVEL_MAX), -EINVAL);

	/* empty password */
	zassert_equal(user_mgr_add(1, "ok", "", "", ACC_LEVEL_USER), -EINVAL);

	/* last name may be NULL (treated as empty) */
	zassert_ok(user_mgr_add(1, "ok", NULL, "p", ACC_LEVEL_USER));
}

/* Update / rename ------------------------------------------------------ */

ZTEST(user_mgr, test_password_rules)
{
	/* no password and empty password: refused */
	zassert_equal(user_mgr_add(1, "Joao", "", NULL, ACC_LEVEL_USER), -EINVAL);
	zassert_equal(user_mgr_add(1, "Joao", "", "", ACC_LEVEL_USER), -EINVAL);

	/* a valid password: accepted */
	zassert_ok(user_mgr_add(1, "Joao", "", "Senha01", ACC_LEVEL_USER));
}

ZTEST(user_mgr, test_update_changes_names_and_level)
{
	struct user_public view;

	zassert_ok(user_mgr_add(1, "Joao", "Silva", "p", ACC_LEVEL_USER));
	zassert_ok(user_mgr_update(1, "Maria", "Santos", ACC_LEVEL_MANAGER));

	zassert_ok(user_mgr_get_by_id(1, &view));
	zassert_str_equal(view.first_name, "Maria");
	zassert_str_equal(view.last_name, "Santos");
	zassert_equal(view.level, ACC_LEVEL_MANAGER);

	/* update preserves password */
	zassert_ok(user_mgr_login(1, "p", NULL));
}

ZTEST(user_mgr, test_update_unknown_id)
{
	zassert_equal(user_mgr_update(999, "x", "", ACC_LEVEL_USER), -ENOENT);
}

/* Password management + login ----------------------------------------- */

ZTEST(user_mgr, test_login_success_and_failure)
{
	struct user_public view;

	zassert_ok(user_mgr_add(1, "Joao", "", "correct", ACC_LEVEL_USER));

	zassert_ok(user_mgr_login(1, "correct", &view));
	zassert_equal(view.user_id, 1);
	zassert_str_equal(view.first_name, "Joao");

	zassert_equal(user_mgr_login(1, "wrong", NULL), -EACCES);
	zassert_equal(user_mgr_login(99, "correct", NULL), -ENOENT);
}

ZTEST(user_mgr, test_set_password_invalidates_old_and_accepts_new)
{
	zassert_ok(user_mgr_add(1, "u", "", "old", ACC_LEVEL_USER));
	zassert_ok(user_mgr_set_password(1, "new"));

	zassert_equal(user_mgr_login(1, "old", NULL), -EACCES);
	zassert_ok(user_mgr_login(1, "new", NULL));
}

ZTEST(user_mgr, test_set_status_blocks_login)
{
	zassert_ok(user_mgr_add(1, "u", "", "pwd", ACC_LEVEL_USER));
	zassert_ok(user_mgr_login(1, "pwd", NULL));

	zassert_ok(user_mgr_set_status(1, USER_STATUS_BLOCKED));
	zassert_equal(user_mgr_login(1, "pwd", NULL), -EACCES);

	zassert_ok(user_mgr_set_status(1, USER_STATUS_ENABLED));
	zassert_ok(user_mgr_login(1, "pwd", NULL));
}

/* Delete --------------------------------------------------------------- */

ZTEST(user_mgr, test_delete)
{
	zassert_ok(user_mgr_add(1, "u", "", "p", ACC_LEVEL_USER));
	zassert_ok(user_mgr_add(2, "v", "", "p", ACC_LEVEL_USER));
	zassert_equal(user_mgr_count(), 2);

	zassert_ok(user_mgr_delete(1));
	zassert_equal(user_mgr_count(), 1);
	zassert_equal(user_mgr_get_by_id(1, NULL), -EINVAL);

	zassert_equal(user_mgr_delete(999), -ENOENT);
}

/* Persistence ---------------------------------------------------------- */

ZTEST(user_mgr, test_reinit_reloads_users)
{
	struct user_public view;

	zassert_ok(user_mgr_add(100, "Alice", "Lima", "pwd1", ACC_LEVEL_USER));
	zassert_ok(user_mgr_add(200, "Bob", "Silva", "pwd2", ACC_LEVEL_ADMIN));

	/* Re-init without wiping the disk: it reloads USERS_DIR */
	zassert_ok(user_mgr_init(USERS_DIR));
	zassert_equal(user_mgr_count(), 2);
	zassert_equal(user_mgr_count_admins_enabled(), 1);

	zassert_ok(user_mgr_get_by_id(100, &view));
	zassert_str_equal(view.first_name, "Alice");

	/* Passwords must survive the reload */
	zassert_ok(user_mgr_login(100, "pwd1", NULL));
	zassert_ok(user_mgr_login(200, "pwd2", NULL));
}

/* Counts ------------------------------------------------- */

ZTEST(user_mgr, test_admin_count_tracks_status_and_level)
{
	zassert_ok(user_mgr_add(1, "u1", "", "p", ACC_LEVEL_USER));
	zassert_ok(user_mgr_add(2, "a1", "", "p", ACC_LEVEL_ADMIN));
	zassert_ok(user_mgr_add(3, "f1", "", "p", ACC_LEVEL_FACTORY));
	zassert_equal(user_mgr_count_admins_enabled(), 2);

	zassert_ok(user_mgr_set_status(2, USER_STATUS_BLOCKED));
	zassert_equal(user_mgr_count_admins_enabled(), 1);
}
