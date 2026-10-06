/*
 * Copyright (c) 2026 GZM Embarcados
 */

/**
 * @file fs_mgr.c
 * @brief Mutex-protected helpers around Zephyr fs_* with power-cut safe save/recover.
 */

#include <gzm/fs_mgr.h>

#include <string.h>
#include <stdio.h>

#include <zephyr/device.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(fs_mgr, CONFIG_LOG_DEFAULT_LEVEL);

#define XFS_WRITE_CHUNK_SIZE  512
#define XFS_MUTEX_TIMEOUT_MS  5000
#define XFS_FILE_SIZE         (FS_MGR_MAX_FILE_NAME + 10)

#define FS_RET_OK 0

static int fs_mgr_lock(void);
static void fs_mgr_unlock(void);
static int fs_mgr_mount_storage(struct fs_mount_t *fsmout);
static int fs_mgr_unlink_silent(const char *path);

static bool g_xfs_initialized;
/* Statically initialized so ringfs / recordfs / any caller can serialize
 * against fs_mgr operations without depending on fs_mgr_init() running
 * first (e.g. unit tests that only need the locking primitive).
 */
static K_SEM_DEFINE(g_xfs_mutex, 1, 1);

static int fs_mgr_lock(void)
{
	return k_sem_take(&g_xfs_mutex, K_MSEC(XFS_MUTEX_TIMEOUT_MS));
}

static void fs_mgr_unlock(void)
{
	k_sem_give(&g_xfs_mutex);
}

/* fs_unlink wrapper that probes existence first to silence the
 * unconditional LOG_ERR fs.c emits on -ENOENT. Callers that treat
 * "missing" as a no-op (atomic save tmp/final cleanup, optional
 * file deletion) should use this instead of bare fs_unlink.
 */
static int fs_mgr_unlink_silent(const char *path)
{
	struct fs_dirent ent;

	if (fs_stat(path, &ent) != 0) {
		return -ENOENT;
	}
	return fs_unlink(path);
}

static int fs_mgr_mount_storage(struct fs_mount_t *fsmout)
{
	int res;

	res = fs_mount(fsmout);

	if (res == FS_RET_OK) {
		LOG_INF("Disk mounted at %s", fsmout->mnt_point);
		/* Unmount and remount to ensure clean state */
		res = fs_unmount(fsmout);
		if (res != FS_RET_OK) {
			LOG_ERR("Error unmounting disk");
			return res;
		}
		res = fs_mount(fsmout);
		if (res != FS_RET_OK) {
			LOG_ERR("Error remounting disk");
			return res;
		}
	} else {
		LOG_ERR("Error mounting disk (%d)", res);
	}

	return res;
}

int fs_mgr_init(struct fs_mount_t *fsmout)
{
	int ret;

	LOG_INF("Initializing filesystem...");

	ret = fs_mgr_mount_storage(fsmout);
	if (ret != 0) {
		LOG_WRN("Mount failed (%d), attempting format...", ret);
		ret = fs_mgr_format(fsmout);
		if (ret != 0) {
			LOG_ERR("Format + mount failed (%d)", ret);
			return ret;
		}
	}

	g_xfs_initialized = true;
	LOG_INF("Filesystem initialized successfully");
	return 0;
}

int fs_mgr_deinit(struct fs_mount_t *fsmout)
{
	int ret;

	ret = fs_mgr_lock();
	if (ret) {
		return ret;
	}

	ret = fs_unmount(fsmout);
	if (ret != 0) {
		LOG_ERR("Failed to unmount filesystem (%d)", ret);
		fs_mgr_unlock();
		return ret;
	}

	g_xfs_initialized = false;
	fs_mgr_unlock();
	LOG_INF("Filesystem unmounted successfully");
	return 0;
}

int fs_mgr_format(struct fs_mount_t *fsmout)
{
	int ret;

	ret = fs_mgr_lock();
	if (ret) {
		return ret;
	}

	LOG_INF("Formatting %s (type %d)...", fsmout->mnt_point, fsmout->type);

	/* Unmount if currently mounted */
	fs_unmount(fsmout);

#if defined(CONFIG_FILE_SYSTEM_MKFS)
	/* fs_mkfs's dev_id is filesystem-specific:
	 *   - FAT: drive path without leading '/' (e.g. "SD:")
	 *   - LittleFS: the flash_area partition id kept in storage_dev
	 * Passing the wrong one returns -ENODEV (-19) silently.
	 */
	uintptr_t dev_id;

	if (fsmout->type == FS_LITTLEFS) {
		dev_id = (uintptr_t)fsmout->storage_dev;
	} else {
		const char *drv = fsmout->mnt_point;

		if (drv[0] == '/') {
			drv++;
		}
		dev_id = (uintptr_t)drv;
	}
	ret = fs_mkfs(fsmout->type, dev_id, NULL, 0);
	if (ret != 0) {
		LOG_ERR("Format failed (%d)", ret);
		fs_mgr_unlock();
		return ret;
	}
	LOG_INF("Filesystem formatted successfully");
#else
	LOG_WRN("fs_mkfs not available (CONFIG_FILE_SYSTEM_MKFS=n)");
	ret = -ENOTSUP;
	fs_mgr_unlock();
	return ret;
#endif

	/* Remount the filesystem */
	ret = fs_mount(fsmout);
	if (ret != 0) {
		LOG_ERR("Failed to mount after format (%d)", ret);
		fs_mgr_unlock();
		return ret;
	}

	fs_mgr_unlock();
	LOG_INF("Filesystem mounted after format");
	return 0;
}

int fs_mgr_search_file_in_dir(const char *path, char *fname, int fname_len, int *file_size,
			      int *num_files_found)
{
	int result;
	struct fs_dir_t dir;
	struct fs_dirent entry;

	result = fs_mgr_lock();
	if (result) {
		return result;
	}

	*file_size = 0;
	*num_files_found = 0;

	fs_dir_t_init(&dir);
	result = fs_opendir(&dir, path);
	if (result) {
		LOG_ERR("Unable to open %s (err %d)", path, result);
		fs_mgr_unlock();
		return -ENOEXEC;
	}

	while (1) {
		result = fs_readdir(&dir, &entry);
		if (result) {
			LOG_ERR("Unable to read directory");
			break;
		} else if (entry.name[0] == '\0') {
			/* End of directory listing */
			break;
		} else if (entry.type == FS_DIR_ENTRY_FILE) {
			if ((*file_size) == 0) {
				*file_size = entry.size;
				snprintf(fname, fname_len, "%s", entry.name);
			}
			(*num_files_found)++;
		}
	}

	fs_closedir(&dir);

	if (*num_files_found == 0) {
		result = -ENOENT;
	}

	fs_mgr_unlock();
	return result;
}

int fs_mgr_get_num_files_in_dir(const char *path, int *num_files, int *total_size)
{
	int result;
	struct fs_dir_t dir;
	struct fs_dirent entry;

	*num_files = 0;
	*total_size = 0;

	result = fs_mgr_lock();
	if (result) {
		return result;
	}

	fs_dir_t_init(&dir);
	result = fs_opendir(&dir, path);
	if (result) {
		LOG_ERR("Unable to open %s (err %d)", path, result);
		fs_mgr_unlock();
		return -ENOEXEC;
	}

	while (1) {
		result = fs_readdir(&dir, &entry);
		if (result) {
			LOG_ERR("Unable to read directory");
			break;
		} else if (entry.name[0] == '\0') {
			/* End of directory listing */
			break;
		} else if (entry.type == FS_DIR_ENTRY_FILE) {
			(*num_files)++;
			(*total_size) += entry.size;
		}
	}

	fs_closedir(&dir);
	fs_mgr_unlock();

	return result;
}

int fs_mgr_read_file(const char *path, const char *file_name, void *buf, int buflen)
{
	char fname[XFS_FILE_SIZE];
	int result;
	struct fs_file_t file;

	fs_file_t_init(&file);

	result = fs_mgr_lock();
	if (result < 0) {
		return result;
	}

	memset(fname, 0, sizeof(fname));

	if (path) {
		snprintf(fname, sizeof(fname), "%s/%s", path, file_name);
	} else {
		snprintf(fname, sizeof(fname), "%s", file_name);
	}

	result = fs_open(&file, fname, FS_O_READ);
	if (result < 0) {
		LOG_DBG("Open %s file error: %d", fname, result);
		fs_mgr_unlock();
		return result;
	}

	result = fs_read(&file, buf, buflen);
	fs_close(&file);

	fs_mgr_unlock();

	/* Return bytes read */
	return result;
}

int fs_mgr_save_file(const char *path, const char *file_name, const void *buf, int buflen)
{
	char fname_final[XFS_FILE_SIZE];
	char fname_tmp[XFS_FILE_SIZE + sizeof(".new")];
	int chunk;
	int remaining;
	int result;
	int written_total = 0;
	const uint8_t *data = (const uint8_t *)buf;
	struct fs_file_t file;

	if (!file_name || !buf || buflen < 0) {
		return -EINVAL;
	}
	remaining = buflen;

	fs_file_t_init(&file);

	result = fs_mgr_lock();
	if (result < 0) {
		return result;
	}

	if (path) {
		snprintf(fname_final, sizeof(fname_final), "%s/%s", path, file_name);
		snprintf(fname_tmp, sizeof(fname_tmp), "%s/%s.new", path, file_name);
	} else {
		snprintf(fname_final, sizeof(fname_final), "%s", file_name);
		snprintf(fname_tmp, sizeof(fname_tmp), "%s.new", file_name);
	}

	/* 1. Drop any leftover .new from a previously interrupted save.
	 *    -ENOENT is the normal first-time case; not an error.
	 */
	result = fs_mgr_unlink_silent(fname_tmp);
	if (result < 0 && result != -ENOENT) {
		LOG_WRN("Drop stale tmp %s: %d", fname_tmp, result);
		/* Non-fatal: open with TRUNC will recover unless FS is read-only,
		 * in which case the open below will surface the real error. */
	}

	/* 2. Write the new content into .new and flush it to media. */
	result = fs_open(&file, fname_tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (result < 0) {
		LOG_ERR("Open %s error: %d", fname_tmp, result);
		fs_mgr_unlock();
		return result;
	}

	while (remaining > 0) {
		chunk = (remaining > XFS_WRITE_CHUNK_SIZE) ? XFS_WRITE_CHUNK_SIZE : remaining;

		result = fs_write(&file, data + written_total, chunk);
		if (result < 0) {
			LOG_ERR("Write %s at %d: %d", fname_tmp, written_total, result);
			(void)fs_close(&file);
			(void)fs_unlink(fname_tmp);
			fs_mgr_unlock();
			return -EIO;
		}
		if (result != chunk) {
			LOG_ERR("Partial write %s at %d: %d/%d", fname_tmp, written_total, result,
				chunk);
			(void)fs_close(&file);
			(void)fs_unlink(fname_tmp);
			fs_mgr_unlock();
			return -EIO;
		}
		written_total += chunk;
		remaining -= chunk;
	}

	result = fs_sync(&file);
	if (result < 0) {
		LOG_ERR("Sync %s: %d", fname_tmp, result);
		(void)fs_close(&file);
		(void)fs_unlink(fname_tmp);
		fs_mgr_unlock();
		return result;
	}

	result = fs_close(&file);
	if (result < 0) {
		LOG_ERR("Close %s: %d", fname_tmp, result);
		(void)fs_unlink(fname_tmp);
		fs_mgr_unlock();
		return result;
	}

	/* 3. Drop the previous final. fs_rename on FATFS will not overwrite,
	 *    so an explicit unlink is required for portability across FATFS
	 *    and LittleFS. -ENOENT on first-ever save is normal.
	 *
	 *    Power-cut between this unlink and the rename below leaves only
	 *    .new on disk; fs_mgr_recover detects that and promotes
	 *    it on the next boot.
	 */
	result = fs_mgr_unlink_silent(fname_final);
	if (result < 0 && result != -ENOENT) {
		LOG_ERR("Unlink %s before rename: %d", fname_final, result);
		fs_mgr_unlock();
		return result;
	}

	/* 4. Atomic rename. After this, fname_final holds the new bytes. */
	result = fs_rename(fname_tmp, fname_final);
	if (result < 0) {
		LOG_ERR("Rename %s -> %s: %d", fname_tmp, fname_final, result);
		/* Leave .new on disk; fs_mgr_recover will retry on next boot. */
		fs_mgr_unlock();
		return result;
	}

	fs_mgr_unlock();
	return 0;
}

int fs_mgr_recover(const char *path, const char *file_name)
{
	bool final_exists;
	bool tmp_exists;
	char fname_final[XFS_FILE_SIZE];
	char fname_tmp[XFS_FILE_SIZE + sizeof(".new")];
	int result;
	struct fs_dirent ent;

	if (!file_name) {
		return -EINVAL;
	}

	result = fs_mgr_lock();
	if (result < 0) {
		return result;
	}

	if (path) {
		snprintf(fname_final, sizeof(fname_final), "%s/%s", path, file_name);
		snprintf(fname_tmp, sizeof(fname_tmp), "%s/%s.new", path, file_name);
	} else {
		snprintf(fname_final, sizeof(fname_final), "%s", file_name);
		snprintf(fname_tmp, sizeof(fname_tmp), "%s.new", file_name);
	}

	tmp_exists = (fs_stat(fname_tmp, &ent) == 0);
	final_exists = (fs_stat(fname_final, &ent) == 0);

	if (!tmp_exists) {
		fs_mgr_unlock();
		return 0;
	}

	/* Both branches resolve to "final = .new contents":
	 *   - power cut between fs_unlink(final) and fs_rename:
	 *       only .new on disk -> rename it to final
	 *   - power cut between fs_close(.new) and fs_unlink(final):
	 *       both on disk; .new is the newer write the user requested
	 *       -> drop final, then rename .new -> final
	 */
	if (final_exists) {
		result = fs_unlink(fname_final);
		if (result < 0) {
			LOG_ERR("Recover %s: unlink: %d", fname_final, result);
			fs_mgr_unlock();
			return result;
		}
	}

	result = fs_rename(fname_tmp, fname_final);
	if (result < 0) {
		LOG_ERR("Recover %s: rename: %d", fname_final, result);
		fs_mgr_unlock();
		return result;
	}

	LOG_INF("Recovered %s from interrupted save", fname_final);
	fs_mgr_unlock();
	return 0;
}

int fs_mgr_load_file(const char *path, const char *file_name, void *buf, int buflen)
{
	char fname[XFS_FILE_SIZE];
	struct fs_dirent ent;
	int result;

	result = fs_mgr_recover(path, file_name);
	if (result < 0) {
		return result;
	}

	/* Probe with fs_stat first: fs.c is the only fs subsystem wrapper
	 * that swallows -ENOENT silently. Without this, the unconditional
	 * LOG_ERR inside fs_open fires for missing-on-first-boot, which is
	 * the expected case for callers using fs_mgr_load_file to read optional
	 * persistent state.
	 */
	if (path) {
		snprintf(fname, sizeof(fname), "%s/%s", path, file_name);
	} else {
		snprintf(fname, sizeof(fname), "%s", file_name);
	}
	if (fs_stat(fname, &ent) != 0) {
		return -ENOENT;
	}

	return fs_mgr_read_file(path, file_name, buf, buflen);
}

int fs_mgr_delete_file(const char *path, const char *file_name)
{
	char fname[XFS_FILE_SIZE];
	int result;

	result = fs_mgr_lock();
	if (result < 0) {
		return result;
	}

	if (path) {
		snprintf(fname, sizeof(fname), "%s/%s", path, file_name);
	} else {
		snprintf(fname, sizeof(fname), "%s", file_name);
	}

	result = fs_unlink(fname);

	if (result) {
		LOG_ERR("Erasing %s file error: %d", fname, result);
	} else {
		LOG_DBG("File deleted: %s", fname);
	}

	fs_mgr_unlock();
	return result;
}

int fs_mgr_get_file_size(const char *path, const char *file_name, int *file_size)
{
	char fname[XFS_FILE_SIZE];
	int result;
	struct fs_dirent dirent;

	result = fs_mgr_lock();
	if (result < 0) {
		return result;
	}

	if (path) {
		snprintf(fname, sizeof(fname), "%s/%s", path, file_name);
	} else {
		snprintf(fname, sizeof(fname), "%s", file_name);
	}

	result = fs_stat(fname, &dirent);
	if (!result) {
		*file_size = dirent.size;
	}

	fs_mgr_unlock();
	return result;
}

int fs_mgr_release_space_in_path(const char *path, int size)
{
	char fname[XFS_FILE_SIZE];
	int result;
	int tot_files_deleted = 0;
	int tot_size = 0;
	struct fs_dir_t dir;
	struct fs_dirent entry;

	if (size <= 0) {
		return -EINVAL;
	}

	result = fs_mgr_lock();
	if (result) {
		return result;
	}

	fs_dir_t_init(&dir);
	result = fs_opendir(&dir, path);
	if (result) {
		LOG_ERR("Unable to open %s (err %d)", path, result);
		fs_mgr_unlock();
		return -ENOEXEC;
	}

	while (1) {
		result = fs_readdir(&dir, &entry);
		if (result) {
			LOG_ERR("Unable to read directory");
			break;
		} else if (entry.name[0] == '\0') {
			/* End of directory listing */
			break;
		} else if (entry.type == FS_DIR_ENTRY_FILE) {
			tot_files_deleted++;
			tot_size += entry.size;
			snprintf(fname, sizeof(fname), "%s/%s", path, entry.name);
			result = fs_unlink(fname);
			LOG_INF("Delete file %s - size: %d - err: %d", fname, entry.size, result);
			if (tot_size >= size) {
				break;
			}
		}
	}

	LOG_INF("Total files deleted: %d - released size: %d bytes", tot_files_deleted, tot_size);
	fs_closedir(&dir);

	fs_mgr_unlock();
	return result;
}

int fs_mgr_delete_files_of_path(const char *path)
{
	char fname[XFS_FILE_SIZE];
	int result;
	struct fs_dir_t dir;
	struct fs_dirent entry;

	result = fs_mgr_lock();
	if (result) {
		return result;
	}

	fs_dir_t_init(&dir);
	result = fs_opendir(&dir, path);
	if (result) {
		LOG_ERR("Unable to open %s (err %d)", path, result);
		fs_mgr_unlock();
		return -ENOEXEC;
	}

	while (1) {
		result = fs_readdir(&dir, &entry);
		if (result) {
			LOG_ERR("Unable to read directory");
			break;
		} else if (entry.name[0] == '\0') {
			/* End of directory listing */
			break;
		} else if (entry.type == FS_DIR_ENTRY_FILE) {
			snprintf(fname, sizeof(fname), "%s/%s", path, entry.name);
			result = fs_unlink(fname);
			LOG_DBG("Deleted: %s (err %d)", fname, result);
		}
	}

	fs_closedir(&dir);
	fs_mgr_unlock();

	return result;
}

/* mkdir each '/'-delimited prefix that does not yet exist, then mkdir
 * the final component. Idempotent: already-existing dirs short-circuit
 * via fs_stat.
 *
 * Mount-root skip: the first '/' separator in an absolute path closes
 * the mount-point segment (e.g. "/SD:" or "/lfs"). Mount roots are
 * guaranteed to exist by the time fs_mgr is in use (fs_mgr_init has
 * mounted them), so we don't try to mkdir that prefix. Avoiding the
 * call sidesteps both the FATFS quirk (f_stat on the mount root
 * returns -ENOENT then fs_mkdir returns -EEXIST) and the noisy
 * unconditional LOG_ERR inside fs.c:fs_mkdir on -EEXIST.
 *
 * EEXIST tolerance on intermediate components: still absorbed silently
 * for the rare case of a TOCTOU race or backend that returns EEXIST
 * even after a successful stat-probe.
 *
 * Behaviour change vs. the previous non-recursive implementation:
 * a path whose parent doesn't exist used to return -ENOENT; it now
 * succeeds, creating every missing component. All in-tree callers
 * simply propagated the -ENOENT, none depended on getting it.
 */
int fs_mgr_mkdir_if_not_exists(const char *path)
{
	char prefix[XFS_FILE_SIZE];
	struct fs_dirent entry;
	bool past_mnt_root = false;
	size_t len;
	int err;
	int result = 0;

	if (path == NULL) {
		return -EINVAL;
	}
	len = strlen(path);
	if (len == 0 || len >= sizeof(prefix)) {
		return -EINVAL;
	}

	/* Take the global fs_mgr lock around the whole prefix walk so that
	 * a follow-up fs_mgr_pwrite / fs_mgr_preallocate / fs_mgr_save_*
	 * by the same caller cannot interleave with another thread's mkdir
	 * or stat-probe that races on the just-created directory entry.
	 *
	 * Pair with CONFIG_FS_FATFS_REENTRANT (fatfs adds its own internal
	 * locks around the volume); fs_mgr's lock covers fs_mgr-level
	 * sequences, FATFS's lock covers raw fs_* coming from elsewhere
	 * in the system (Zephyr LOG backend writes, etc.).
	 */
	err = fs_mgr_lock();
	if (err < 0) {
		return err;
	}

	for (size_t i = 0; i <= len; i++) {
		prefix[i] = path[i];

		/* mkdir at every '/' separator (skipping the leading one
		 * if the path is absolute) and at end-of-string.
		 */
		if ((i > 0 && path[i] == '/') || path[i] == '\0') {
			char saved;

			/* Skip the first non-leading '/': that is the
			 * mount-point boundary, and the mount root must
			 * already exist for any of the rest to succeed.
			 */
			if (!past_mnt_root && path[i] == '/') {
				past_mnt_root = true;
				continue;
			}

			saved = prefix[i];
			prefix[i] = '\0';

			err = fs_stat(prefix, &entry);
			if (err == -ENOENT) {
				err = fs_mkdir(prefix);
				if (err < 0 && err != -EEXIST) {
					result = err;
					break;
				}
			} else if (err < 0) {
				result = err;
				break;
			} else if (entry.type != FS_DIR_ENTRY_DIR) {
				result = -EEXIST;
				break;
			}

			prefix[i] = saved;
		}
	}

	fs_mgr_unlock();
	return result;
}

int fs_mgr_mem_info(struct fs_mount_t *fsmout, enum fs_mgr_mem_info ch, float *val)
{
	int result;
	long long space = 0;
	struct fs_statvfs stat;

	if (!val) {
		return -EINVAL;
	}

	result = fs_statvfs(fsmout->mnt_point, &stat);
	if (result) {
		return result;
	}

	switch (ch) {
	case FS_MGR_MEM_SIZE:
		/* block size x total num blocks */
		space = (long long)stat.f_blocks * stat.f_frsize;
		break;

	case FS_MGR_MEM_FREE_SPACE:
		/* block size x num free blocks */
		space = (long long)stat.f_bfree * stat.f_frsize;
		break;

	case FS_MGR_MEM_USED_SPACE:
		/* block size x total num blocks */
		space = (long long)stat.f_blocks * stat.f_frsize;
		/* block size x num free blocks */
		space -= ((long long)stat.f_bfree * stat.f_frsize);
		break;

	default:
		return -ENOTSUP;
	}

	*val = (float)(space);
	return 0;
}

int fs_mgr_list_files(const char *path, struct fs_mgr_file_entry *entries, int max_entries)
{
	int count = 0;
	int result;
	struct fs_dir_t dir;
	struct fs_dirent dirent;

	if (!entries || max_entries <= 0) {
		return -EINVAL;
	}

	result = fs_mgr_lock();
	if (result) {
		return result;
	}

	fs_dir_t_init(&dir);
	result = fs_opendir(&dir, path);
	if (result) {
		LOG_ERR("Unable to open %s (err %d)", path, result);
		fs_mgr_unlock();
		return result;
	}

	while (count < max_entries) {
		result = fs_readdir(&dir, &dirent);
		if (result || dirent.name[0] == '\0') {
			break;
		}

		if (dirent.type == FS_DIR_ENTRY_FILE) {
			strncpy(entries[count].name, dirent.name, sizeof(entries[count].name) - 1);
			entries[count].name[sizeof(entries[count].name) - 1] = '\0';
			entries[count].size = dirent.size;
			count++;
		}
	}

	fs_closedir(&dir);
	fs_mgr_unlock();

	return count;
}

int fs_mgr_pread(const char *path, void *buf, uint32_t buflen, uint32_t offset,
		 enum fs_mgr_open_mode mode)
{
	struct fs_file_t file;
	int rc;

	if (!path || !buf || buflen == 0) {
		return -EINVAL;
	}
	if ((mode & FS_O_READ) == 0) {
		return -EINVAL;
	}

	fs_file_t_init(&file);

	rc = fs_mgr_lock();
	if (rc < 0) {
		return rc;
	}

	/* Pure read with no CREATE flag: probe with fs_stat first.
	 * Missing-on-cold-boot is the expected outcome for ringfs
	 * resume reads (data.conf), event_log audit_hash load, and
	 * any other "optional persistent state" use case. fs_open in
	 * Zephyr fs.c logs ERR unconditionally on -ENOENT, which the
	 * caller cannot suppress; fs_stat is the only fs.c wrapper
	 * that swallows -ENOENT silently (see fs.c:627-628).
	 */
	if ((mode & FS_O_CREATE) == 0) {
		struct fs_dirent ent;

		if (fs_stat(path, &ent) != 0) {
			fs_mgr_unlock();
			return -ENOENT;
		}
	}

	rc = fs_open(&file, path, (fs_mode_t)mode);
	if (rc < 0) {
		LOG_DBG("pread: open %s: %d", path, rc);
		fs_mgr_unlock();
		return rc;
	}

	if (offset != 0) {
		rc = fs_seek(&file, (off_t)offset, FS_SEEK_SET);
		if (rc < 0) {
			LOG_DBG("pread: seek %s @%u: %d", path, offset, rc);
			(void)fs_close(&file);
			fs_mgr_unlock();
			return rc;
		}
	}

	rc = fs_read(&file, buf, buflen);
	(void)fs_close(&file);
	fs_mgr_unlock();
	return rc;
}

int fs_mgr_pwrite(const char *path, const void *buf, uint32_t buflen, uint32_t offset,
		  enum fs_mgr_open_mode mode)
{
	struct fs_file_t file;
	const uint8_t *src = buf;
	uint32_t remaining;
	uint32_t written_total = 0;
	int rc;

	if (!path || !buf || buflen == 0) {
		return -EINVAL;
	}
	if ((mode & FS_O_WRITE) == 0) {
		return -EINVAL;
	}
	remaining = buflen;

	fs_file_t_init(&file);

	rc = fs_mgr_lock();
	if (rc < 0) {
		return rc;
	}

	rc = fs_open(&file, path, (fs_mode_t)mode);
	if (rc < 0) {
		LOG_DBG("pwrite: open %s: %d", path, rc);
		fs_mgr_unlock();
		return rc;
	}

	if (offset == FS_MGR_OFFSET_END) {
		rc = fs_seek(&file, 0, FS_SEEK_END);
	} else if (offset != 0) {
		rc = fs_seek(&file, (off_t)offset, FS_SEEK_SET);
	} else {
		rc = 0;
	}
	if (rc < 0) {
		LOG_DBG("pwrite: seek %s @%u: %d", path, offset, rc);
		(void)fs_close(&file);
		fs_mgr_unlock();
		return rc;
	}

	while (remaining > 0) {
		uint32_t chunk = (remaining > XFS_WRITE_CHUNK_SIZE) ? XFS_WRITE_CHUNK_SIZE : remaining;
		ssize_t n = fs_write(&file, src + written_total, chunk);

		if (n < 0) {
			LOG_ERR("pwrite: write %s @%u +%u: %zd", path, offset, written_total, n);
			(void)fs_close(&file);
			fs_mgr_unlock();
			return (int)n;
		}
		if ((uint32_t)n != chunk) {
			LOG_ERR("pwrite: short %s @%u +%u: %zd/%u", path, offset, written_total, n,
				chunk);
			(void)fs_close(&file);
			fs_mgr_unlock();
			return -EIO;
		}
		written_total += chunk;
		remaining -= chunk;
	}

	(void)fs_close(&file);
	fs_mgr_unlock();
	return (int)written_total;
}

int fs_mgr_preallocate(const char *path, uint32_t size)
{
	struct fs_file_t file;
	int rc;

	if (!path) {
		return -EINVAL;
	}

	fs_file_t_init(&file);

	rc = fs_mgr_lock();
	if (rc < 0) {
		return rc;
	}

	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		LOG_DBG("preallocate: open %s: %d", path, rc);
		fs_mgr_unlock();
		return rc;
	}

	rc = fs_truncate(&file, (off_t)size);
	if (rc < 0) {
		LOG_ERR("preallocate: truncate %s to %u: %d", path, size, rc);
		(void)fs_close(&file);
		fs_mgr_unlock();
		return rc;
	}

	(void)fs_sync(&file);
	(void)fs_close(&file);
	fs_mgr_unlock();
	return 0;
}
