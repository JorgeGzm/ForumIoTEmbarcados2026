/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 *
 * The product rule: at most CONFIG_GZM_USER_MGR_MAX users (50), the default
 * admin included. Same naming pattern as the Robot Framework suite
 * (tests/robot): id 1000+n, "Usuario", "Teste<nn>", password "Senha<nn>".
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/ztest.h>

#include <gzm/user_mgr.h>

#include "test_storage.h"

#define LIMIT_DIR   TEST_MNT "/users_limit"
#define USERS_MAX   CONFIG_GZM_USER_MGR_MAX
#define ADMIN_ID    1
#define FIRST_ID    1000

/* User n of the pattern: id 1000+n, Usuario Teste<nn>, Senha<nn>. */
static int add_user(unsigned int n)
{
	char last[16];
	char pwd[16];

	snprintf(last, sizeof(last), "Teste%02u", n);
	snprintf(pwd, sizeof(pwd), "Senha%02u", n);
	return user_mgr_add(FIRST_ID + n, "Usuario", last, pwd, ACC_LEVEL_USER);
}

/* Admin + 49 pattern users = USERS_MAX. */
static void fill_store(void)
{
	for (unsigned int n = 1; n < USERS_MAX; n++) {
		zassert_ok(add_user(n), "user %u", n);
	}
	zassert_equal(user_mgr_count(), USERS_MAX);
}

static void *limit_setup(void)
{
	zassert_ok(test_storage_mount());
	return NULL;
}

static void limit_before(void *fixture)
{
	ARG_UNUSED(fixture);
	test_storage_wipe_dir(LIMIT_DIR);
	zassert_ok(user_mgr_init(LIMIT_DIR));
	zassert_ok(user_mgr_create_default_admin(ADMIN_ID, "1234", ACC_LEVEL_ADMIN));
}

static void limit_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	test_storage_wipe_dir(LIMIT_DIR);
}

ZTEST_SUITE(user_mgr_limit, NULL, limit_setup, limit_before, NULL, limit_teardown);

ZTEST(user_mgr_limit, test_fills_up_to_the_limit)
{
	fill_store();
	zassert_ok(user_mgr_login(FIRST_ID + 49, "Senha49", NULL));
}

ZTEST(user_mgr_limit, test_user_51_is_refused)
{
	fill_store();
	zassert_equal(add_user(50), -ENOSPC);
	zassert_equal(user_mgr_count(), USERS_MAX);
	zassert_equal(user_mgr_login(FIRST_ID + 50, "Senha50", NULL), -ENOENT);
}

ZTEST(user_mgr_limit, test_limit_survives_remount)
{
	fill_store();
	zassert_ok(test_storage_remount());
	zassert_ok(user_mgr_init(LIMIT_DIR));
	zassert_equal(user_mgr_count(), USERS_MAX);
	zassert_equal(add_user(50), -ENOSPC);
}

ZTEST(user_mgr_limit, test_delete_frees_a_slot)
{
	fill_store();
	zassert_ok(user_mgr_delete(FIRST_ID + 25));
	zassert_ok(add_user(50));
	zassert_equal(user_mgr_count(), USERS_MAX);
}

ZTEST(user_mgr_limit, test_blocked_users_still_count)
{
	fill_store();
	for (unsigned int n = 1; n <= 5; n++) {
		zassert_ok(user_mgr_set_status(FIRST_ID + n, USER_STATUS_BLOCKED));
	}
	zassert_equal(user_mgr_count_blocked(), 5);
	zassert_equal(user_mgr_login(FIRST_ID + 1, "Senha01", NULL), -EACCES);
	zassert_equal(add_user(50), -ENOSPC);
}
