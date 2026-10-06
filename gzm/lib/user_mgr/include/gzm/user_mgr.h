/*
 * Copyright (c) 2026 GZM Embarcados
 */

/**
 * @file    user_mgr.h
 * @brief   User account manager (CRUD + auth) backed by a JSON store.
 */

#ifndef GZM_USER_MGR_H
#define GZM_USER_MGR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gzm/access_level.h>

#define USER_MGR_FIRST_NAME_LEN CONFIG_GZM_USER_MGR_FIRST_NAME_LEN
#define USER_MGR_LAST_NAME_LEN  CONFIG_GZM_USER_MGR_LAST_NAME_LEN

enum user_status {
	USER_STATUS_BLOCKED = 0,
	USER_STATUS_ENABLED = 1,
};

/**
 * Public view of a user, without secrets (salt and key stay hidden).
 */
struct user_public {
	uint32_t user_id;
	char first_name[USER_MGR_FIRST_NAME_LEN + 1];
	char last_name[USER_MGR_LAST_NAME_LEN + 1];
	enum access_level level;
	enum user_status status;
};

/**
 * Initialise the user store. Scans @p storage_dir and loads existing
 * users into the in-RAM sorted index. Creates the directory if missing.
 * Calling it again drops the index and loads @p storage_dir from scratch.
 */
int user_mgr_init(const char *storage_dir);

bool user_mgr_is_ready(void);

/**
 * Create the default admin if (and only if) the repository is empty.
 * The caller picks the ID, password and access level: these vary per
 * product.  Returns 0 on success, -EEXIST if the id is taken or the
 * base is not empty, -EINVAL on bad args.
 */
int user_mgr_create_default_admin(uint32_t id, const char *password, enum access_level level);

int user_mgr_add(uint32_t id, const char *first_name, const char *last_name,
		 const char *password, enum access_level level);

int user_mgr_update(uint32_t id, const char *first_name, const char *last_name,
		    enum access_level level);

int user_mgr_set_password(uint32_t id, const char *new_password);

int user_mgr_delete(uint32_t id);

int user_mgr_set_status(uint32_t id, enum user_status status);

int user_mgr_get_by_id(uint32_t id, struct user_public *out);

int user_mgr_get_by_index(size_t idx, struct user_public *out);

size_t user_mgr_count(void);
size_t user_mgr_count_admins_enabled(void);
size_t user_mgr_count_blocked(void);

/**
 * Verify credentials. Uses constant-time comparison on the derived key.
 * On success, fills @p out (if non-NULL) with the public view.
 */
int user_mgr_login(uint32_t id, const char *password, struct user_public *out);


#ifdef __cplusplus
}
#endif

#endif /* GZM_USER_MGR_H */
