/*
 * Copyright (c) 2026 GZM Embarcados
 *
 * Unit tests for gzm/fs_mgr on LittleFS: atomic save + load, recovery of
 * an interrupted save, directories, listings, partial reads/writes and
 * disk space.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <gzm/fs_mgr.h>

#define MNT "/lfs"
#define TEST_DIR MNT "/t"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(test_lfs);

static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &test_lfs,
	.storage_dev = (void *)DT_FIXED_PARTITION_ID(DT_CHOSEN(app_users_partition)),
	.mnt_point = MNT,
};

/* Size of a file, or a negative errno when it does not exist */
static int size_of(const char *name)
{
	int size = 0;
	int err = fs_mgr_get_file_size(TEST_DIR, name, &size);

	return err ? err : size;
}

static void *suite_setup(void)
{
	zassert_ok(fs_mgr_init(&mnt), "fs_mgr_init on %s", MNT);
	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_ok(fs_mgr_mkdir_if_not_exists(TEST_DIR));
	(void)fs_mgr_delete_files_of_path(TEST_DIR);
}

ZTEST_SUITE(fs_mgr, NULL, suite_setup, before_each, NULL, NULL);

/* Save + load ---------------------------------------------------------- */

ZTEST(fs_mgr, test_save_and_load_roundtrip)
{
	/* Larger than the 512 B write chunk: exercises the chunked write */
	static uint8_t out[1500];
	static uint8_t in[1500];

	for (size_t i = 0; i < sizeof(out); i++) {
		out[i] = (uint8_t)(i * 7);
	}
	zassert_ok(fs_mgr_save_file(TEST_DIR, "a.bin", out, sizeof(out)));
	zassert_equal(fs_mgr_load_file(TEST_DIR, "a.bin", in, sizeof(in)), sizeof(in));
	zassert_mem_equal(in, out, sizeof(out));
	zassert_equal(size_of("a.bin.new"), -ENOENT, "temporary file left behind");
}

ZTEST(fs_mgr, test_save_replaces_the_content)
{
	char in[16] = {0};

	zassert_ok(fs_mgr_save_file(TEST_DIR, "a.txt", "first", 5));
	zassert_ok(fs_mgr_save_file(TEST_DIR, "a.txt", "2nd", 3));
	zassert_equal(fs_mgr_load_file(TEST_DIR, "a.txt", in, sizeof(in)), 3);
	zassert_str_equal(in, "2nd");
}

ZTEST(fs_mgr, test_save_with_full_path)
{
	char in[8] = {0};

	zassert_ok(fs_mgr_save_file(NULL, TEST_DIR "/b.txt", "abc", 3));
	zassert_equal(fs_mgr_read_file(TEST_DIR, "b.txt", in, sizeof(in)), 3);
	zassert_str_equal(in, "abc");
}

ZTEST(fs_mgr, test_save_bad_args)
{
	zassert_equal(fs_mgr_save_file(TEST_DIR, NULL, "x", 1), -EINVAL);
	zassert_equal(fs_mgr_save_file(TEST_DIR, "x", NULL, 1), -EINVAL);
	zassert_equal(fs_mgr_save_file(TEST_DIR, "x", "x", -1), -EINVAL);
}

ZTEST(fs_mgr, test_load_missing_file)
{
	char in[4];

	zassert_equal(fs_mgr_load_file(TEST_DIR, "nope.bin", in, sizeof(in)), -ENOENT);
}

/* Interrupted save ----------------------------------------------------- */

ZTEST(fs_mgr, test_recover_only_the_new_file)
{
	/* Power cut after the old file was removed, before the rename */
	char in[8] = {0};

	zassert_equal(fs_mgr_pwrite(TEST_DIR "/c.txt.new", "new", 3, 0, FS_MGR_MODE_W_CREATE), 3);
	zassert_equal(fs_mgr_load_file(TEST_DIR, "c.txt", in, sizeof(in)), 3);
	zassert_str_equal(in, "new");
	zassert_equal(size_of("c.txt.new"), -ENOENT);
}

ZTEST(fs_mgr, test_recover_new_file_wins_over_old)
{
	/* Power cut after the new file was written, before the old was removed */
	char in[8] = {0};

	zassert_ok(fs_mgr_save_file(TEST_DIR, "c.txt", "old", 3));
	zassert_equal(fs_mgr_pwrite(TEST_DIR "/c.txt.new", "new", 3, 0, FS_MGR_MODE_W_CREATE), 3);
	zassert_equal(fs_mgr_load_file(TEST_DIR, "c.txt", in, sizeof(in)), 3);
	zassert_str_equal(in, "new");
	zassert_equal(size_of("c.txt.new"), -ENOENT);
}

ZTEST(fs_mgr, test_recover_without_new_file_keeps_the_old)
{
	char in[8] = {0};

	zassert_ok(fs_mgr_save_file(TEST_DIR, "c.txt", "old", 3));
	zassert_ok(fs_mgr_recover(TEST_DIR, "c.txt"));
	zassert_equal(fs_mgr_load_file(TEST_DIR, "c.txt", in, sizeof(in)), 3);
	zassert_str_equal(in, "old");
	zassert_equal(fs_mgr_recover(TEST_DIR, NULL), -EINVAL);
}

/* Directories ---------------------------------------------------------- */

ZTEST(fs_mgr, test_mkdir_creates_every_level_and_is_idempotent)
{
	struct fs_dirent ent;

	zassert_ok(fs_mgr_mkdir_if_not_exists(TEST_DIR "/x/y/z"));
	zassert_ok(fs_mgr_mkdir_if_not_exists(TEST_DIR "/x/y/z"));
	zassert_ok(fs_stat(TEST_DIR "/x/y/z", &ent));
	zassert_equal(ent.type, FS_DIR_ENTRY_DIR);
}

ZTEST(fs_mgr, test_mkdir_errors)
{
	zassert_ok(fs_mgr_save_file(TEST_DIR, "file", "x", 1));
	zassert_equal(fs_mgr_mkdir_if_not_exists(TEST_DIR "/file"), -EEXIST, "a file is not a dir");
	zassert_equal(fs_mgr_mkdir_if_not_exists(NULL), -EINVAL);
	zassert_equal(fs_mgr_mkdir_if_not_exists(""), -EINVAL);
}

ZTEST(fs_mgr, test_count_list_and_search)
{
	struct fs_mgr_file_entry list[8];
	char name[32];
	int n = 0;
	int total = 0;
	int size = 0;

	zassert_ok(fs_mgr_save_file(TEST_DIR, "f1", "0123456789", 10));
	zassert_ok(fs_mgr_save_file(TEST_DIR, "f2", "01234567890123456789", 20));
	zassert_ok(fs_mgr_save_file(TEST_DIR, "f3", "012345678901234567890123456789", 30));
	zassert_ok(fs_mgr_mkdir_if_not_exists(TEST_DIR "/sub"));

	zassert_ok(fs_mgr_get_num_files_in_dir(TEST_DIR, &n, &total));
	zassert_equal(n, 3, "directories do not count");
	zassert_equal(total, 60);

	zassert_equal(fs_mgr_list_files(TEST_DIR, list, ARRAY_SIZE(list)), 3);
	zassert_equal(fs_mgr_list_files(TEST_DIR, list, 2), 2, "stops at max_entries");
	zassert_equal(fs_mgr_list_files(TEST_DIR, NULL, 2), -EINVAL);

	zassert_ok(fs_mgr_search_file_in_dir(TEST_DIR, name, sizeof(name), &size, &n));
	zassert_equal(n, 3);
	zassert_true(size > 0);
}

ZTEST(fs_mgr, test_empty_and_missing_dirs)
{
	char name[32];
	int size = 0;
	int n = 0;

	zassert_equal(fs_mgr_search_file_in_dir(TEST_DIR, name, sizeof(name), &size, &n), -ENOENT);
	zassert_equal(n, 0);
	zassert_equal(fs_mgr_get_num_files_in_dir(MNT "/missing", &n, &size), -ENOEXEC);
}

/* Delete + release space ----------------------------------------------- */

ZTEST(fs_mgr, test_delete)
{
	int n = 0;
	int total = 0;

	zassert_ok(fs_mgr_save_file(TEST_DIR, "d1", "x", 1));
	zassert_ok(fs_mgr_save_file(TEST_DIR, "d2", "y", 1));

	zassert_ok(fs_mgr_delete_file(TEST_DIR, "d1"));
	zassert_equal(size_of("d1"), -ENOENT);
	zassert_not_equal(fs_mgr_delete_file(TEST_DIR, "d1"), 0, "already deleted");

	zassert_ok(fs_mgr_delete_files_of_path(TEST_DIR));
	zassert_ok(fs_mgr_get_num_files_in_dir(TEST_DIR, &n, &total));
	zassert_equal(n, 0);
}

ZTEST(fs_mgr, test_release_space_deletes_until_enough)
{
	static uint8_t buf[100];
	int n = 0;
	int total = 0;

	zassert_ok(fs_mgr_save_file(TEST_DIR, "r1", buf, sizeof(buf)));
	zassert_ok(fs_mgr_save_file(TEST_DIR, "r2", buf, sizeof(buf)));
	zassert_ok(fs_mgr_save_file(TEST_DIR, "r3", buf, sizeof(buf)));

	/* 150 bytes: two files of 100 go, one stays */
	zassert_ok(fs_mgr_release_space_in_path(TEST_DIR, 150));
	zassert_ok(fs_mgr_get_num_files_in_dir(TEST_DIR, &n, &total));
	zassert_equal(n, 1);
	zassert_equal(fs_mgr_release_space_in_path(TEST_DIR, 0), -EINVAL);
}

/* Partial read/write --------------------------------------------------- */

ZTEST(fs_mgr, test_pwrite_pread_at_offset)
{
	char in[16] = {0};

	zassert_equal(fs_mgr_pwrite(TEST_DIR "/p", "0123456789", 10, 0, FS_MGR_MODE_W_CREATE), 10);
	zassert_equal(fs_mgr_pwrite(TEST_DIR "/p", "AB", 2, 3, FS_MGR_MODE_RW), 2);
	zassert_equal(fs_mgr_pread(TEST_DIR "/p", in, 4, 2, FS_MGR_MODE_R), 4);
	zassert_mem_equal(in, "2AB5", 4);

	zassert_equal(fs_mgr_pwrite(TEST_DIR "/p", "XY", 2, FS_MGR_OFFSET_END, FS_MGR_MODE_W_OPEN), 2);
	zassert_equal(size_of("p"), 12);
	memset(in, 0, sizeof(in));
	zassert_equal(fs_mgr_pread(TEST_DIR "/p", in, sizeof(in), 0, FS_MGR_MODE_R), 12);
	zassert_str_equal(in, "012AB56789XY");
}

ZTEST(fs_mgr, test_pread_pwrite_errors)
{
	char in[4];

	zassert_equal(fs_mgr_pread(TEST_DIR "/missing", in, sizeof(in), 0, FS_MGR_MODE_R), -ENOENT);
	zassert_equal(fs_mgr_pread(TEST_DIR "/p", in, sizeof(in), 0, FS_MGR_MODE_W_CREATE), -EINVAL,
		      "read with a write-only mode");
	zassert_equal(fs_mgr_pwrite(TEST_DIR "/p", "x", 1, 0, FS_MGR_MODE_R), -EINVAL,
		      "write with a read-only mode");
	zassert_equal(fs_mgr_pwrite(TEST_DIR "/p", "x", 0, 0, FS_MGR_MODE_W_CREATE), -EINVAL);
	zassert_equal(fs_mgr_pread(NULL, in, sizeof(in), 0, FS_MGR_MODE_R), -EINVAL);
}

ZTEST(fs_mgr, test_preallocate)
{
	zassert_ok(fs_mgr_preallocate(TEST_DIR "/big", 4096));
	zassert_equal(size_of("big"), 4096);
	zassert_equal(fs_mgr_preallocate(NULL, 1), -EINVAL);
}

/* Disk space ----------------------------------------------------------- */

ZTEST(fs_mgr, test_mem_info_adds_up)
{
	float size = 0;
	float free_space = 0;
	float used = 0;

	zassert_ok(fs_mgr_mem_info(&mnt, FS_MGR_MEM_SIZE, &size));
	zassert_ok(fs_mgr_mem_info(&mnt, FS_MGR_MEM_FREE_SPACE, &free_space));
	zassert_ok(fs_mgr_mem_info(&mnt, FS_MGR_MEM_USED_SPACE, &used));
	zassert_true(size > 0 && free_space > 0 && used > 0);
	zassert_within(used + free_space, size, 1.0f);

	zassert_equal(fs_mgr_mem_info(&mnt, FS_MGR_MEM_MAX, &size), -ENOTSUP);
	zassert_equal(fs_mgr_mem_info(&mnt, FS_MGR_MEM_SIZE, NULL), -EINVAL);
}

/* Mount ---------------------------------------------------------------- */

ZTEST(fs_mgr, test_files_survive_unmount_and_mount)
{
	char in[8] = {0};

	zassert_ok(fs_mgr_save_file(TEST_DIR, "keep", "kept", 4));
	zassert_ok(fs_mgr_deinit(&mnt));
	zassert_ok(fs_mgr_init(&mnt));
	zassert_equal(fs_mgr_load_file(TEST_DIR, "keep", in, sizeof(in)), 4);
	zassert_str_equal(in, "kept");
}

ZTEST(fs_mgr, test_format_wipes_everything)
{
	char in[4];

	zassert_ok(fs_mgr_save_file(TEST_DIR, "gone", "x", 1));
	zassert_ok(fs_mgr_format(&mnt));
	zassert_equal(fs_mgr_load_file(TEST_DIR, "gone", in, sizeof(in)), -ENOENT);
}
