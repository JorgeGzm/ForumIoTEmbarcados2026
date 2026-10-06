/*
 * Copyright (c) 2026 GZM Embarcados
 */

/**
 * @file    user_mgr_shell.c
 * @brief   Shell commands for the user store.
 */

#include <gzm/user_mgr.h>

#include <zephyr/kernel.h>

#if defined(CONFIG_SHELL)

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

static const char *user_mgr_shell_level_str(enum access_level lvl);
static const char *user_mgr_shell_status_str(enum user_status st);
static int user_mgr_shell_parse_level(const char *s, enum access_level *out);
static int user_mgr_shell_parse_u32(const char *s, uint32_t *out);

static int user_mgr_shell_count_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_show_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_show_all_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_add_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_del_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_set_pwd_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_set_status_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_rename_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_login_cmd(const struct shell *sh, size_t argc, char **argv);
static int user_mgr_shell_levels_cmd(const struct shell *sh, size_t argc, char **argv);
static void user_mgr_shell_print_levels(const struct shell *sh);

static const char *user_mgr_shell_level_str(enum access_level lvl)
{
	switch (lvl) {
	case ACC_LEVEL_NONE:
		return "none";
	case ACC_LEVEL_USER:
		return "user";
	case ACC_LEVEL_SUBMANAGER:
		return "submanager";
	case ACC_LEVEL_MANAGER:
		return "manager";
	case ACC_LEVEL_ADMIN:
		return "admin";
	case ACC_LEVEL_ENGINEER:
		return "engineer";
	case ACC_LEVEL_FACTORY:
		return "factory";
	default:
		return "?";
	}
}

static const char *user_mgr_shell_status_str(enum user_status st)
{
	switch (st) {
	case USER_STATUS_ENABLED:
		return "enabled";
	case USER_STATUS_BLOCKED:
		return "blocked";
	default:
		return "?";
	}
}

static int user_mgr_shell_parse_level(const char *s, enum access_level *out)
{
	char *endp = NULL;
	unsigned long v = 0;

	v = strtoul(s, &endp, 10);
	if (endp != s && *endp == '\0') {
		if (v < ACC_LEVEL_MAX) {
			*out = (enum access_level)v;
			return 0;
		}
		return -EINVAL;
	}
	if (strcmp(s, "none") == 0) {
		*out = ACC_LEVEL_NONE;
		return 0;
	}
	if (strcmp(s, "user") == 0) {
		*out = ACC_LEVEL_USER;
		return 0;
	}
	if (strcmp(s, "submanager") == 0) {
		*out = ACC_LEVEL_SUBMANAGER;
		return 0;
	}
	if (strcmp(s, "manager") == 0) {
		*out = ACC_LEVEL_MANAGER;
		return 0;
	}
	if (strcmp(s, "admin") == 0) {
		*out = ACC_LEVEL_ADMIN;
		return 0;
	}
	if (strcmp(s, "engineer") == 0) {
		*out = ACC_LEVEL_ENGINEER;
		return 0;
	}
	if (strcmp(s, "factory") == 0) {
		*out = ACC_LEVEL_FACTORY;
		return 0;
	}
	return -EINVAL;
}

static int user_mgr_shell_parse_u32(const char *s, uint32_t *out)
{
	char *endp = NULL;
	unsigned long v = 0;

	v = strtoul(s, &endp, 0);
	if (endp == s || (endp && *endp != '\0')) {
		return -EINVAL;
	}
	if (v > UINT32_MAX) {
		return -ERANGE;
	}
	*out = (uint32_t)v;
	return 0;
}

static int user_mgr_shell_count_cmd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "%zu users (%zu admins enabled)", user_mgr_count(),
		    user_mgr_count_admins_enabled());
	return 0;
}

static int user_mgr_shell_show_cmd(const struct shell *sh, size_t argc, char **argv)
{
	struct user_public u;
	uint32_t id = 0;
	int err = 0;

	ARG_UNUSED(argc);

	err = user_mgr_shell_parse_u32(argv[1], &id);
	if (err) {
		shell_error(sh, "bad id '%s'", argv[1]);
		return err;
	}

	err = user_mgr_get_by_id(id, &u);
	if (err) {
		shell_error(sh, "id=%u not found (%d)", id, err);
		return err;
	}

	shell_print(sh, "id         : %u", u.user_id);
	shell_print(sh, "first_name : %s", u.first_name);
	shell_print(sh, "last_name  : %s", u.last_name);
	shell_print(sh, "level      : %s (%d)", user_mgr_shell_level_str(u.level), u.level);
	shell_print(sh, "status     : %s", user_mgr_shell_status_str(u.status));
	return 0;
}

static int user_mgr_shell_show_all_cmd(const struct shell *sh, size_t argc, char **argv)
{
	struct user_public u;
	size_t n = user_mgr_count();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Users (%zu):", n);
	shell_print(sh,
		    "ID         | Level       | Status   | First name               | Last name");
	shell_print(sh,
		    "-----------|-------------|----------|--------------------------|---------");

	for (size_t i = 0; i < n; i++) {
		if (user_mgr_get_by_index(i, &u) == 0) {
			shell_print(sh, "%-10u | %-11s | %-8s | %-24s | %s", u.user_id,
				    user_mgr_shell_level_str(u.level),
				    user_mgr_shell_status_str(u.status), u.first_name, u.last_name);
		}
	}
	return 0;
}

static int user_mgr_shell_add_cmd(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id = 0;
	int err = 0;
	enum access_level lvl = ACC_LEVEL_NONE;

	ARG_UNUSED(argc);

	err = user_mgr_shell_parse_u32(argv[1], &id);
	if (err) {
		shell_error(sh, "bad id");
		return err;
	}
	err = user_mgr_shell_parse_level(argv[2], &lvl);
	if (err) {
		user_mgr_shell_print_levels(sh);
		shell_error(sh, "bad level");
		return err;
	}

	err = user_mgr_add(id, argv[3], argv[4], argv[5], lvl);
	if (err) {
		shell_error(sh, "add failed (%d)", err);
		return err;
	}
	shell_print(sh, "user %u added", id);
	return 0;
}

static int user_mgr_shell_del_cmd(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id = 0;
	int err = 0;

	ARG_UNUSED(argc);

	err = user_mgr_shell_parse_u32(argv[1], &id);
	if (err) {
		shell_error(sh, "bad id");
		return err;
	}

	err = user_mgr_delete(id);
	if (err) {
		shell_error(sh, "delete failed (%d)", err);
		return err;
	}
	shell_print(sh, "user %u deleted", id);
	return 0;
}

static int user_mgr_shell_set_pwd_cmd(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id = 0;
	int err = 0;

	ARG_UNUSED(argc);

	err = user_mgr_shell_parse_u32(argv[1], &id);
	if (err) {
		shell_error(sh, "bad id");
		return err;
	}

	err = user_mgr_set_password(id, argv[2]);
	if (err) {
		shell_error(sh, "set_pwd failed (%d)", err);
		return err;
	}
	shell_print(sh, "password updated");
	return 0;
}

static int user_mgr_shell_set_status_cmd(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id = 0;
	uint32_t val = 0;
	int err = 0;

	ARG_UNUSED(argc);

	err = user_mgr_shell_parse_u32(argv[1], &id);
	if (!err) {
		err = user_mgr_shell_parse_u32(argv[2], &val);
	}
	if (err) {
		shell_error(sh, "bad id or status");
		return err;
	}

	err = user_mgr_set_status(id, val ? USER_STATUS_ENABLED : USER_STATUS_BLOCKED);
	if (err) {
		shell_error(sh, "set_status failed (%d)", err);
		return err;
	}
	shell_print(sh, "user %u %s", id, val ? "enabled" : "blocked");
	return 0;
}

static int user_mgr_shell_rename_cmd(const struct shell *sh, size_t argc, char **argv)
{
	struct user_public u;
	uint32_t id = 0;
	int err = 0;

	ARG_UNUSED(argc);

	err = user_mgr_shell_parse_u32(argv[1], &id);
	if (err) {
		shell_error(sh, "bad id");
		return err;
	}

	err = user_mgr_get_by_id(id, &u);
	if (err) {
		shell_error(sh, "id=%u not found (%d)", id, err);
		return err;
	}
	err = user_mgr_update(id, argv[2], argv[3], u.level);
	if (err) {
		shell_error(sh, "rename failed (%d)", err);
		return err;
	}
	shell_print(sh, "user %u renamed", id);
	return 0;
}

static int user_mgr_shell_login_cmd(const struct shell *sh, size_t argc, char **argv)
{
	struct user_public u;
	uint32_t id = 0;
	int err = 0;

	ARG_UNUSED(argc);

	err = user_mgr_shell_parse_u32(argv[1], &id);
	if (err) {
		shell_error(sh, "bad id");
		return err;
	}

	err = user_mgr_login(id, argv[2], &u);
	if (err) {
		shell_warn(sh, "login failed (%d)", err);
		return err;
	}
	shell_print(sh, "ok: %s %s (%s)", u.first_name, u.last_name,
		    user_mgr_shell_level_str(u.level));
	return 0;
}

static void user_mgr_shell_print_levels(const struct shell *sh)
{
	shell_print(sh, "Access levels:");
	shell_print(sh, "  [0]  none");
	shell_print(sh, "  [1]  user");
	shell_print(sh, "  [2]  submanager");
	shell_print(sh, "  [3]  manager");
	shell_print(sh, "  [4]  admin");
	shell_print(sh, "  [5]  engineer");
	shell_print(sh, "  [6]  factory");
}

static int user_mgr_shell_levels_cmd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	user_mgr_shell_print_levels(sh);
	return 0;
}

/* SHELL_CMD_ARG: the shell checks the argument count and prints the help */
SHELL_STATIC_SUBCMD_SET_CREATE(
	user_mgr_cmds,
	SHELL_CMD_ARG(count, NULL, "Count users", user_mgr_shell_count_cmd, 1, 0),
	SHELL_CMD_ARG(show, NULL, "Show user: show <id>", user_mgr_shell_show_cmd, 2, 0),
	SHELL_CMD_ARG(show_all, NULL, "List users", user_mgr_shell_show_all_cmd, 1, 0),
	SHELL_CMD_ARG(add, NULL,
		      "Add: add <id> <level> <first_name> <last_name> <password> "
		      "(see 'user levels')",
		      user_mgr_shell_add_cmd, 6, 0),
	SHELL_CMD_ARG(levels, NULL, "List available access levels", user_mgr_shell_levels_cmd, 1,
		      0),
	SHELL_CMD_ARG(del, NULL, "Delete: del <id>", user_mgr_shell_del_cmd, 2, 0),
	SHELL_CMD_ARG(set_pwd, NULL, "Change password: set_pwd <id> <password>",
		      user_mgr_shell_set_pwd_cmd, 3, 0),
	SHELL_CMD_ARG(set_status, NULL, "Enable/block: set_status <id> <0|1>",
		      user_mgr_shell_set_status_cmd, 3, 0),
	SHELL_CMD_ARG(rename, NULL, "Rename: rename <id> <first_name> <last_name>",
		      user_mgr_shell_rename_cmd, 4, 0),
	SHELL_CMD_ARG(login, NULL, "Verify password: login <id> <password>",
		      user_mgr_shell_login_cmd, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(user, &user_mgr_cmds, "User manager commands", NULL);

#endif /* CONFIG_SHELL */
