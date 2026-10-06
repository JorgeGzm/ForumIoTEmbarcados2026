/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_STORAGE_H_
#define TEST_STORAGE_H_

#define TEST_MNT "/lfs1"

/** Mount LittleFS on chosen app,users-partition (format if needed). */
int test_storage_mount(void);

/** Unmount and mount again: what a reboot does to the filesystem. */
int test_storage_remount(void);

/** Delete every file in @p dir. */
void test_storage_wipe_dir(const char *dir);

#endif /* TEST_STORAGE_H_ */
