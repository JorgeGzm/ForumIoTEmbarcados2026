/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mock example: the real user_mgr.c runs against fake storage. FFF fakes
 * replace fs_mgr and the Zephyr fs calls, so the tests can force what the
 * real NOR rarely does: a full disk, a failed write, a corrupted record.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include <gzm/user_mgr.h>

DEFINE_FFF_GLOBALS;

/* fs_mgr: what user_mgr uses to store one file per user */
FAKE_VALUE_FUNC(int, fs_mgr_mkdir_if_not_exists, const char *);
FAKE_VALUE_FUNC(int, fs_mgr_save_file, const char *, const char *, const void *, int);
FAKE_VALUE_FUNC(int, fs_mgr_load_file, const char *, const char *, void *, int);
/* Zephyr fs: directory scan on init, unlink on delete */
FAKE_VALUE_FUNC(int, fs_opendir, struct fs_dir_t *, const char *);
FAKE_VALUE_FUNC(int, fs_readdir, struct fs_dir_t *, struct fs_dirent *);
FAKE_VALUE_FUNC(int, fs_closedir, struct fs_dir_t *);
FAKE_VALUE_FUNC(int, fs_unlink, const char *);

/* Last buffer user_mgr asked to write */
static uint8_t written[512];
static int written_len;

static bool contains(const uint8_t *buf, int len, const char *str)
{
	size_t n = strlen(str);

	for (int i = 0; i + (int)n <= len; i++) {
		if (memcmp(&buf[i], str, n) == 0) {
			return true;
		}
	}
	return false;
}

static int save_capture(const char *path, const char *name, const void *buf, int len)
{
	written_len = MIN(len, (int)sizeof(written));
	memcpy(written, buf, written_len);
	return 0;
}

/* One directory entry, then the end of the listing */
static int readdir_one(struct fs_dir_t *d, struct fs_dirent *ent)
{
	if (fs_readdir_fake.call_count == 1) {
		ent->type = FS_DIR_ENTRY_FILE;
		strcpy(ent->name, "1001.bin");
		ent->size = 128;
	} else {
		ent->name[0] = '\0';
	}
	return 0;
}

/* The stored record of user 1001 comes back with one byte flipped */
static int load_corrupted(const char *path, const char *name, void *buf, int len)
{
	memcpy(buf, written, MIN(len, written_len));
	((uint8_t *)buf)[8] ^= 0x01;
	return written_len;
}

static void reset(void *fixture)
{
	ARG_UNUSED(fixture);
	RESET_FAKE(fs_mgr_mkdir_if_not_exists);
	RESET_FAKE(fs_mgr_save_file);
	RESET_FAKE(fs_mgr_load_file);
	RESET_FAKE(fs_opendir);
	RESET_FAKE(fs_readdir);
	RESET_FAKE(fs_closedir);
	RESET_FAKE(fs_unlink);
	FFF_RESET_HISTORY();
	fs_opendir_fake.return_val = -ENOENT; /* empty store */
	fs_mgr_save_file_fake.custom_fake = save_capture;

	zassert_ok(user_mgr_init("/lfs/users"));
}

ZTEST_SUITE(user_mgr_mock, NULL, NULL, reset, NULL, NULL);

ZTEST(user_mgr_mock, test_disk_full_is_reported_and_nothing_changes)
{
	fs_mgr_save_file_fake.custom_fake = NULL;
	fs_mgr_save_file_fake.return_val = -ENOSPC;

	zassert_equal(user_mgr_add(1001, "Usuario", "Teste01", "Senha01", ACC_LEVEL_USER),
		      -ENOSPC);
	zassert_equal(user_mgr_count(), 0, "user must not exist only in RAM");
}

ZTEST(user_mgr_mock, test_failed_unlink_keeps_the_user)
{
	zassert_ok(user_mgr_add(1001, "Usuario", "Teste01", "Senha01", ACC_LEVEL_USER));
	fs_unlink_fake.return_val = -EIO;

	zassert_equal(user_mgr_delete(1001), -EIO);
	zassert_equal(user_mgr_count(), 1);
}

ZTEST(user_mgr_mock, test_corrupted_record_is_skipped_on_boot)
{
	zassert_ok(user_mgr_add(1001, "Usuario", "Teste01", "Senha01", ACC_LEVEL_USER));

	/* "Reboot": the scan finds 1001.bin, but its CRC no longer matches */
	fs_opendir_fake.return_val = 0;
	fs_readdir_fake.custom_fake = readdir_one;
	fs_mgr_load_file_fake.custom_fake = load_corrupted;

	zassert_ok(user_mgr_init("/lfs/users"));
	zassert_equal(fs_mgr_load_file_fake.call_count, 1);
	zassert_equal(user_mgr_count(), 0, "corrupted user must not load");
}

ZTEST(user_mgr_mock, test_password_is_never_written_in_clear)
{
	zassert_ok(user_mgr_add(1001, "Usuario", "Teste01", "Senha01", ACC_LEVEL_USER));

	zassert_true(written_len > 0);
	zassert_false(contains(written, written_len, "Senha01"),
		      "password found in the stored record");
	zassert_true(contains(written, written_len, "Teste01"));
}
