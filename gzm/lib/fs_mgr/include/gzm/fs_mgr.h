/*
 * Copyright (c) 2026 GZM Embarcados
 */

/**
 * @file fs_mgr.h
 * @brief Mutex-protected helpers around Zephyr fs_* with power-cut safe save/recover.
 */

#ifndef GZM_FS_MGR_H
#define GZM_FS_MGR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#define FS_MGR_MAX_FILE_NAME MAX_FILE_NAME

enum fs_mgr_mem_info {
	FS_MGR_MEM_SIZE = 0,
	FS_MGR_MEM_FREE_SPACE,
	FS_MGR_MEM_USED_SPACE,
	FS_MGR_MEM_MAX
};

/**
 * @brief Open mode for positional read / write helpers.
 *
 * Maps to Zephyr fs_open() flag combinations. Picked names match
 * stdio fopen() conventions: r / r+ / w / w+ / a / a+. The
 * variants without TRUNC ("open existing without erasing") are
 * needed by ringfs / recordfs when they re-write at offsets inside
 * an already-populated file.
 */
enum fs_mgr_open_mode {
	FS_MGR_MODE_R           = FS_O_READ,                                   /**< "r": open existing for read */
	FS_MGR_MODE_RW          = FS_O_RDWR,                                   /**< "r+": open existing for read+write */
	FS_MGR_MODE_W_OPEN      = FS_O_CREATE | FS_O_WRITE,                    /**< open-or-create for write, do not truncate */
	FS_MGR_MODE_W_CREATE    = FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC,       /**< "w": create or truncate, write only */
	FS_MGR_MODE_W_APPEND    = FS_O_CREATE | FS_O_WRITE | FS_O_APPEND,      /**< "a": create or open, position at EOF */
	FS_MGR_MODE_RW_OPEN     = FS_O_CREATE | FS_O_RDWR,                     /**< open-or-create for read+write, do not truncate */
	FS_MGR_MODE_RW_CREATE   = FS_O_CREATE | FS_O_RDWR  | FS_O_TRUNC,       /**< "w+": create or truncate, read+write */
	FS_MGR_MODE_RW_APPEND   = FS_O_CREATE | FS_O_RDWR  | FS_O_APPEND,      /**< "a+": create or open, read+write, EOF */
};

/**
 * @brief Special offset value for fs_mgr_pwrite: seek to current
 *        end-of-file before writing (equivalent to APPEND, but
 *        explicit per call).
 */
#define FS_MGR_OFFSET_END UINT32_MAX

/**
 * @brief File entry returned by fs_mgr_list_files.
 */
struct fs_mgr_file_entry {
	char name[32];
	int size;
};

/**
 * @brief Initialize the filesystem.
 *
 * @param fsmout Pointer to the filesystem mount structure.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_init(struct fs_mount_t *fsmout);

/**
 * @brief Deinitialize the filesystem.
 *
 * @param fsmout Pointer to the filesystem mount structure.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_deinit(struct fs_mount_t *fsmout);

/**
 * @brief Format the filesystem.
 *
 * @param fsmout Pointer to the filesystem mount structure.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_format(struct fs_mount_t *fsmout);

/**
 * @brief Search for a file in a directory.
 *
 * @param path Directory path to search.
 * @param fname Buffer to store the found file name.
 * @param fname_len Length of the fname buffer.
 * @param file_size Pointer to store the file size.
 * @param num_files_found Pointer to store the number of files found.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_search_file_in_dir(const char *path, char *fname, int fname_len, int *file_size,
			      int *num_files_found);

/**
 * @brief Get the number of files in a directory.
 *
 * @param path Directory path.
 * @param num_files Pointer to store the number of files.
 * @param total_size Pointer to store the total size of all files.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_get_num_files_in_dir(const char *path, int *num_files, int *total_size);

/**
 * @brief Save data to a file, safe against a power cut.
 *
 * Writes the payload to "<file_name>.new", flushes and closes it, then
 * renames over the final file. A power loss at any point leaves either
 * the previous good content on disk or a complete .new that
 * fs_mgr_load_file / fs_mgr_recover promotes on next boot.
 *
 * No CRC is added: pair with your own header if you need corruption
 * detection beyond the FS layer.
 *
 * @param path Directory path (or NULL for root).
 * @param file_name File name (without the .new suffix).
 * @param buf Pointer to data buffer.
 * @param buflen Number of bytes to write.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_save_file(const char *path, const char *file_name, const void *buf, int buflen);

/**
 * @brief Read data from a file.
 *
 * @param path Directory path (or NULL for root).
 * @param file_name File name.
 * @param buf Pointer to destination buffer.
 * @param buflen Maximum number of bytes to read.
 * @return Number of bytes read on success, negative errno on failure.
 */
int fs_mgr_read_file(const char *path, const char *file_name, void *buf, int buflen);

/**
 * @brief Read a file written by fs_mgr_save_file, recovering any
 *        interrupted previous save before the read.
 *
 * @return Number of bytes read on success, negative errno on failure.
 */
int fs_mgr_load_file(const char *path, const char *file_name, void *buf, int buflen);

/**
 * @brief Reconcile a leftover ".new" from a previous interrupted atomic
 *        save. Idempotent: safe to call when no .new exists.
 *
 *        Resolution rules:
 *          - .new alone           -> rename .new -> final
 *          - .new and final exist -> drop final, rename .new -> final
 *          - no .new              -> no-op
 *
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_recover(const char *path, const char *file_name);

/**
 * @brief Delete a file.
 *
 * @param path Directory path (or NULL for root).
 * @param file_name File name.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_delete_file(const char *path, const char *file_name);

/**
 * @brief Delete all files in a directory.
 *
 * @param path Directory path.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_delete_files_of_path(const char *path);

/**
 * @brief Get the size of a file.
 *
 * @param path Directory path (or NULL for root).
 * @param file_name File name.
 * @param file_size Pointer to store the file size.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_get_file_size(const char *path, const char *file_name, int *file_size);

/**
 * @brief Release space in a directory by deleting oldest files.
 *
 * @param path Directory path.
 * @param size Minimum number of bytes to release.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_release_space_in_path(const char *path, int size);

/**
 * @brief Create a directory if it does not exist.
 *
 * @param path Directory path.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_mkdir_if_not_exists(const char *path);

/**
 * @brief Get filesystem memory information.
 *
 * @param fsmout Pointer to the filesystem mount structure.
 * @param ch Memory info channel to query.
 * @param val Pointer to store the result in bytes.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_mem_info(struct fs_mount_t *fsmout, enum fs_mgr_mem_info ch, float *val);

/**
 * @brief List files in a directory (single mutex lock).
 *
 * @param path        Directory path.
 * @param entries     Array to fill with file info.
 * @param max_entries Maximum entries to return.
 * @return Number of entries filled, or negative error.
 */
int fs_mgr_list_files(const char *path, struct fs_mgr_file_entry *entries, int max_entries);

/**
 * @brief Positional read: open, seek, read, close in one call.
 *
 * @param path   Absolute file path.
 * @param buf    Destination buffer.
 * @param buflen Maximum bytes to read.
 * @param offset Byte offset to seek to before reading. Must be within
 *               the current file size (Zephyr fs_seek rejects past-EOF
 *               with -EINVAL).
 * @param mode   Open mode. Must include FS_O_READ.
 * @return Number of bytes read on success, negative errno on failure.
 */
int fs_mgr_pread(const char *path, void *buf, uint32_t buflen, uint32_t offset,
		 enum fs_mgr_open_mode mode);

/**
 * @brief Positional write: open, seek, write (chunked), close in one call.
 *
 * Writes are chunked to avoid timeouts on slow SD cards (matches the
 * fs_mgr_save_file chunking).
 *
 * @param path   Absolute file path.
 * @param buf    Source buffer.
 * @param buflen Bytes to write.
 * @param offset Byte offset to seek to before writing. Pass
 *               FS_MGR_OFFSET_END to seek to current end-of-file.
 *               Must be <= current file size unless mode has TRUNC,
 *               or pre-allocated via fs_mgr_preallocate.
 * @param mode   Open mode. Must include FS_O_WRITE.
 * @return Number of bytes written on success, negative errno on failure.
 */
int fs_mgr_pwrite(const char *path, const void *buf, uint32_t buflen, uint32_t offset,
		  enum fs_mgr_open_mode mode);

/**
 * @brief Pre-allocate a file to a fixed size (extends with zeros, or
 *        truncates if already larger). Required before
 *        fs_mgr_pwrite / fs_mgr_pread can use offsets > 0 on a
 *        freshly created file under FATFS, whose Zephyr shim rejects
 *        seek past EOF with -EINVAL.
 *
 * @param path Absolute file path. Created if missing.
 * @param size Final size in bytes.
 * @return 0 on success, negative errno on failure.
 */
int fs_mgr_preallocate(const char *path, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* GZM_FS_MGR_H */
