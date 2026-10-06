/*
 * Copyright (c) 2026 GZM Embarcados
 */

/**
 * @file    user_mgr.c
 * @brief   User account manager (CRUD + auth) backed by a JSON store.
 */

#include <gzm/user_mgr.h>

#include <gzm/fs_mgr.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <psa/crypto.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(user_mgr, LOG_LEVEL_INF);

#define LOCK_TIMEOUT_MS    2000
#define USER_FILE_EXT      ".bin"
#define USER_PATH_MAX      192
#define USER_ID_STRMAX     12
#define USER_STORE_VERSION 3U

#define USER_MGR_PASSWORD_MAX_LEN CONFIG_GZM_USER_MGR_PASSWORD_MAX_LEN
#define USER_MGR_SALT_LEN         CONFIG_GZM_USER_MGR_SALT_LEN
#define USER_MGR_KEY_LEN          32 /* SHA-256 output */

struct user_record {
	uint16_t version;
	uint32_t user_id;
	char first_name[USER_MGR_FIRST_NAME_LEN + 1];
	char last_name[USER_MGR_LAST_NAME_LEN + 1];
	uint8_t level;
	uint8_t status;
	uint8_t salt[USER_MGR_SALT_LEN];
	uint32_t iterations;
	uint8_t key[USER_MGR_KEY_LEN];
	uint32_t crc32;
} __packed;

/* In-RAM index entry: what the counters and the id lookups need, so
 * they never touch the flash. Kept sorted by user_id. */
struct user_index_entry {
	uint32_t user_id;
	uint8_t level;  /* enum access_level */
	uint8_t status; /* enum user_status */
};

static struct user_index_entry user_index[CONFIG_GZM_USER_MGR_MAX];
static size_t index_count;
static const char *storage_dir;
static bool initialized;
K_SEM_DEFINE(user_mgr_sem, 1, 1);

static int user_mgr_lock(void)
{
	return k_sem_take(&user_mgr_sem, K_MSEC(LOCK_TIMEOUT_MS));
}

static void user_mgr_unlock(void)
{
	k_sem_give(&user_mgr_sem);
}

static int user_mgr_derive(const char *password, size_t password_len, const uint8_t *salt,
			   size_t salt_len, uint32_t iterations, uint8_t *key, size_t key_len)
{
	/* PBKDF2-HMAC-SHA256 through the PSA Crypto API (Mbed TLS 4 / TF-PSA-Crypto). */
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_status_t st = psa_crypto_init();

	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST,
						      iterations);
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt,
						    salt_len);
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
						    (const uint8_t *)password, password_len);
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_output_bytes(&op, key, key_len);
	}
	(void)psa_key_derivation_abort(&op);
	if (st != PSA_SUCCESS) {
		LOG_ERR("pbkdf2 failed (psa %d)", (int)st);
		return -EIO;
	}
	return 0;
}

static bool user_mgr_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t diff = 0;
	size_t i = 0;

	for (i = 0; i < len; i++) {
		diff |= a[i] ^ b[i];
	}
	return diff == 0;
}

static uint32_t user_mgr_record_crc(const struct user_record *rec)
{
	return crc32_ieee((const uint8_t *)rec, offsetof(struct user_record, crc32));
}

static int user_mgr_build_path(const char *dir, uint32_t user_id, char *out, size_t outlen)
{
	int n = 0;

	n = snprintf(out, outlen, "%s/%u%s", dir, user_id, USER_FILE_EXT);
	if (n < 0 || (size_t)n >= outlen) {
		return -ENAMETOOLONG;
	}
	return 0;
}

static bool user_mgr_parse_filename(const char *name, uint32_t *id_out)
{
	char idbuf[USER_ID_STRMAX];
	char *endp = NULL;
	size_t nlen = 0;
	size_t idlen = 0;
	unsigned long id = 0;

	nlen = strlen(name);
	if (nlen <= strlen(USER_FILE_EXT)) {
		return false;
	}
	idlen = nlen - strlen(USER_FILE_EXT);
	if (idlen >= sizeof(idbuf)) {
		return false;
	}
	if (strcmp(&name[idlen], USER_FILE_EXT) != 0) {
		return false;
	}

	memcpy(idbuf, name, idlen);
	idbuf[idlen] = '\0';

	id = strtoul(idbuf, &endp, 10);
	if (endp == idbuf || !endp || *endp != '\0') {
		return false;
	}
	if (id > UINT32_MAX) {
		return false;
	}
	*id_out = (uint32_t)id;
	return true;
}

static int user_mgr_store_write(const char *dir, const struct user_record *rec)
{
	char path[USER_PATH_MAX];
	struct user_record tmp;
	int err = 0;

	err = user_mgr_build_path(dir, rec->user_id, path, sizeof(path));
	if (err) {
		return err;
	}

	tmp = *rec;
	tmp.version = USER_STORE_VERSION;
	tmp.crc32 = user_mgr_record_crc(&tmp);

	return fs_mgr_save_file(NULL, path, &tmp, sizeof(tmp));
}

static int user_mgr_store_read(const char *dir, uint32_t user_id, struct user_record *rec)
{
	char path[USER_PATH_MAX];
	int err = 0;
	int got = 0;

	err = user_mgr_build_path(dir, user_id, path, sizeof(path));
	if (err) {
		return err;
	}

	got = fs_mgr_load_file(NULL, path, rec, sizeof(*rec));
	if (got < 0) {
		return got;
	}
	if ((size_t)got != sizeof(*rec)) {
		LOG_WRN("%s: truncated (%d/%zu)", path, got, sizeof(*rec));
		return -EBADMSG;
	}
	if (rec->version != USER_STORE_VERSION) {
		LOG_WRN("%s: unknown version %u", path, rec->version);
		return -EBADMSG;
	}
	if (user_mgr_record_crc(rec) != rec->crc32) {
		LOG_WRN("%s: crc mismatch", path);
		return -EBADMSG;
	}
	return 0;
}

static int user_mgr_store_delete(const char *dir, uint32_t user_id)
{
	char path[USER_PATH_MAX];
	int err = 0;

	err = user_mgr_build_path(dir, user_id, path, sizeof(path));
	if (err) {
		return err;
	}

	err = fs_unlink(path);
	if (err == -ENOENT) {
		return 0;
	}
	return err;
}

static void user_mgr_index_locate(uint32_t user_id, size_t *pos, bool *found)
{
	size_t lo = 0;
	size_t hi = index_count;
	size_t mid = 0;
	uint32_t v = 0;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		v = user_index[mid].user_id;
		if (v == user_id) {
			*pos = mid;
			*found = true;
			return;
		}
		if (v < user_id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	*pos = lo;
	*found = false;
}

static int user_mgr_index_insert(const struct user_index_entry *e)
{
	bool found = false;
	size_t pos = 0;

	if (index_count >= ARRAY_SIZE(user_index)) {
		return -ENOSPC;
	}

	user_mgr_index_locate(e->user_id, &pos, &found);
	if (found) {
		return -EEXIST;
	}

	if (pos < index_count) {
		memmove(&user_index[pos + 1], &user_index[pos],
			(index_count - pos) * sizeof(user_index[0]));
	}
	user_index[pos] = *e;
	index_count++;
	return 0;
}

static int user_mgr_index_remove(uint32_t user_id)
{
	bool found = false;
	size_t pos = 0;

	user_mgr_index_locate(user_id, &pos, &found);
	if (!found) {
		return -ENOENT;
	}

	if (pos + 1 < index_count) {
		memmove(&user_index[pos], &user_index[pos + 1],
			(index_count - pos - 1) * sizeof(user_index[0]));
	}
	index_count--;
	return 0;
}

static int user_mgr_index_update(uint32_t user_id, const struct user_index_entry *e)
{
	bool found = false;
	size_t pos = 0;

	if (e->user_id != user_id) {
		return -EINVAL;
	}

	user_mgr_index_locate(user_id, &pos, &found);
	if (!found) {
		return -ENOENT;
	}
	user_index[pos] = *e;
	return 0;
}

/* Portable bounded strlen. strnlen() itself is POSIX 2008 so it is
 * not guaranteed under -std=c11 without _POSIX_C_SOURCE; using a
 * local helper keeps the TU feature-macro-free and builds the same
 * on the target (picolibc/newlib) and host (native_sim). */
static size_t user_mgr_strnlen(const char *s, size_t max)
{
	size_t i = 0;

	for (i = 0; i < max && s[i] != '\0'; i++) {
	}
	return i;
}

static int user_mgr_validate_name(const char *name, size_t max_len)
{
	size_t n = 0;

	/* Last name may be empty, first name may not: caller enforces
	 * the non-empty rule by passing max_len==0 never; here we just
	 * cap at max_len. */
	n = user_mgr_strnlen(name, max_len + 1);
	if (n > max_len) {
		return -EINVAL;
	}
	return 0;
}

static int user_mgr_validate_password(const char *pwd, size_t *len_out)
{
	size_t n = 0;

	if (!pwd) {
		return -EINVAL;
	}
	n = user_mgr_strnlen(pwd, USER_MGR_PASSWORD_MAX_LEN + 1);
	if (n == 0 || n > USER_MGR_PASSWORD_MAX_LEN) {
		return -EINVAL;
	}
	if (len_out) {
		*len_out = n;
	}
	return 0;
}

static int user_mgr_validate_level(enum access_level level)
{
	if (level >= ACC_LEVEL_MAX) {
		return -EINVAL;
	}
	return 0;
}

static int user_mgr_fill_record(struct user_record *rec, uint32_t id, const char *first_name,
				const char *last_name, const char *password,
				enum access_level level, enum user_status status)
{
	size_t pwd_len = 0;
	int err = 0;

	memset(rec, 0, sizeof(*rec));
	rec->version = USER_STORE_VERSION;
	rec->user_id = id;
	rec->level = (uint8_t)level;
	rec->status = (uint8_t)status;

	strncpy(rec->first_name, first_name, USER_MGR_FIRST_NAME_LEN);
	rec->first_name[USER_MGR_FIRST_NAME_LEN] = '\0';

	strncpy(rec->last_name, last_name ? last_name : "", USER_MGR_LAST_NAME_LEN);
	rec->last_name[USER_MGR_LAST_NAME_LEN] = '\0';

	err = user_mgr_validate_password(password, &pwd_len);
	if (err) {
		return err;
	}

	err = sys_csrand_get(rec->salt, sizeof(rec->salt));
	if (err) {
		LOG_ERR("salt generation failed (%d)", err);
		return -EIO;
	}

	rec->iterations = CONFIG_GZM_USER_MGR_PBKDF2_ITERATIONS;

	err = user_mgr_derive(password, pwd_len, rec->salt, sizeof(rec->salt), rec->iterations,
			      rec->key, sizeof(rec->key));
	if (err) {
		return err;
	}

	return 0;
}

static void user_mgr_record_to_public(const struct user_record *rec, struct user_public *out)
{
	out->user_id = rec->user_id;
	memcpy(out->first_name, rec->first_name, sizeof(out->first_name));
	out->first_name[sizeof(out->first_name) - 1] = '\0';
	memcpy(out->last_name, rec->last_name, sizeof(out->last_name));
	out->last_name[sizeof(out->last_name) - 1] = '\0';
	out->level = (enum access_level)rec->level;
	out->status = (enum user_status)rec->status;
}

/* Load every <id>.bin of storage_dir into the index. A file that cannot
 * be read (truncated, old version, bad CRC) is skipped, not fatal. */
static int user_mgr_load_index(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	struct user_record rec;
	struct user_index_entry e;
	uint32_t id = 0;
	size_t skipped = 0;
	int ret = 0;
	int err = 0;

	fs_dir_t_init(&dir);
	ret = fs_opendir(&dir, storage_dir);
	if (ret) {
		return ret == -ENOENT ? 0 : ret;
	}

	for (;;) {
		ret = fs_readdir(&dir, &ent);
		if (ret || ent.name[0] == '\0') {
			break;
		}
		if (ent.type != FS_DIR_ENTRY_FILE || !user_mgr_parse_filename(ent.name, &id)) {
			continue;
		}

		err = user_mgr_store_read(storage_dir, id, &rec);
		if (err == 0) {
			e.user_id = rec.user_id;
			e.level = rec.level;
			e.status = rec.status;
			err = user_mgr_index_insert(&e);
		}
		if (err) {
			LOG_WRN("skip %s (%d)", ent.name, err);
			skipped++;
		}
	}
	(void)fs_closedir(&dir);

	LOG_INF("%s: %zu loaded, %zu skipped", storage_dir, index_count, skipped);
	return ret;
}

int user_mgr_init(const char *dir)
{
	int err = 0;

	if (!dir) {
		return -EINVAL;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	initialized = false;
	storage_dir = dir;
	index_count = 0;

	err = fs_mgr_mkdir_if_not_exists(storage_dir);
	if (err) {
		LOG_ERR("mkdir(%s) failed (%d)", storage_dir, err);
		goto unlock;
	}

	err = user_mgr_load_index();
	if (err) {
		LOG_ERR("scan(%s) failed (%d)", storage_dir, err);
		goto unlock;
	}

	initialized = true;

unlock:
	user_mgr_unlock();
	return err;
}

bool user_mgr_is_ready(void)
{
	return initialized;
}

int user_mgr_create_default_admin(uint32_t id, const char *password, enum access_level level)
{
	int err = 0;

	if (!initialized) {
		return -EINVAL;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	if (index_count > 0) {
		user_mgr_unlock();
		return -EEXIST;
	}
	user_mgr_unlock();

	return user_mgr_add(id, "admin", "", password, level);
}

int user_mgr_add(uint32_t id, const char *first_name, const char *last_name,
		 const char *password, enum access_level level)
{
	struct user_record rec;
	struct user_index_entry e;
	bool found = false;
	size_t pos = 0;
	int err = 0;

	if (!initialized) {
		return -EINVAL;
	}
	/* first_name is required (non-empty); last_name may be empty. */
	if (!first_name || first_name[0] == '\0') {
		return -EINVAL;
	}
	err = user_mgr_validate_name(first_name, USER_MGR_FIRST_NAME_LEN);
	if (err) {
		return err;
	}
	err = user_mgr_validate_name(last_name ? last_name : "", USER_MGR_LAST_NAME_LEN);
	if (err) {
		return err;
	}
	err = user_mgr_validate_level(level);
	if (err) {
		return err;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	user_mgr_index_locate(id, &pos, &found);
	if (found) {
		err = -EEXIST;
		goto unlock;
	}
	if (index_count >= ARRAY_SIZE(user_index)) {
		err = -ENOSPC;
		goto unlock;
	}

	err = user_mgr_fill_record(&rec, id, first_name, last_name ? last_name : "", password,
				   level, USER_STATUS_ENABLED);
	if (err) {
		goto unlock;
	}

	err = user_mgr_store_write(storage_dir, &rec);
	if (err) {
		LOG_ERR("write id=%u failed (%d)", id, err);
		goto unlock;
	}

	e.user_id = id;
	e.level = (uint8_t)level;
	e.status = (uint8_t)USER_STATUS_ENABLED;

	err = user_mgr_index_insert(&e);
	if (err) {
		LOG_ERR("index_insert id=%u failed (%d), rolling back", id, err);
		(void)user_mgr_store_delete(storage_dir, id);
	}

unlock:
	user_mgr_unlock();
	return err;
}

int user_mgr_update(uint32_t id, const char *first_name, const char *last_name,
		    enum access_level level)
{
	struct user_record rec;
	struct user_index_entry e;
	int err = 0;

	if (!initialized) {
		return -EINVAL;
	}
	if (!first_name || first_name[0] == '\0') {
		return -EINVAL;
	}
	err = user_mgr_validate_name(first_name, USER_MGR_FIRST_NAME_LEN);
	if (err) {
		return err;
	}
	err = user_mgr_validate_name(last_name ? last_name : "", USER_MGR_LAST_NAME_LEN);
	if (err) {
		return err;
	}
	err = user_mgr_validate_level(level);
	if (err) {
		return err;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	err = user_mgr_store_read(storage_dir, id, &rec);
	if (err) {
		goto unlock;
	}

	strncpy(rec.first_name, first_name, USER_MGR_FIRST_NAME_LEN);
	rec.first_name[USER_MGR_FIRST_NAME_LEN] = '\0';
	strncpy(rec.last_name, last_name ? last_name : "", USER_MGR_LAST_NAME_LEN);
	rec.last_name[USER_MGR_LAST_NAME_LEN] = '\0';
	rec.level = (uint8_t)level;

	err = user_mgr_store_write(storage_dir, &rec);
	if (err) {
		goto unlock;
	}

	e.user_id = id;
	e.level = (uint8_t)level;
	e.status = rec.status;
	err = user_mgr_index_update(id, &e);

unlock:
	user_mgr_unlock();
	return err;
}

int user_mgr_set_password(uint32_t id, const char *new_password)
{
	struct user_record rec;
	size_t pwd_len = 0;
	int err = 0;

	if (!initialized) {
		return -EINVAL;
	}
	err = user_mgr_validate_password(new_password, &pwd_len);
	if (err) {
		return err;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	err = user_mgr_store_read(storage_dir, id, &rec);
	if (err) {
		goto unlock;
	}

	err = sys_csrand_get(rec.salt, sizeof(rec.salt));
	if (err) {
		err = -EIO;
		goto unlock;
	}

	rec.iterations = CONFIG_GZM_USER_MGR_PBKDF2_ITERATIONS;
	err = user_mgr_derive(new_password, pwd_len, rec.salt, sizeof(rec.salt), rec.iterations,
			      rec.key, sizeof(rec.key));
	if (err) {
		goto unlock;
	}

	err = user_mgr_store_write(storage_dir, &rec);

unlock:
	user_mgr_unlock();
	return err;
}

int user_mgr_delete(uint32_t id)
{
	bool found = false;
	size_t pos = 0;
	int err = 0;

	if (!initialized) {
		return -EINVAL;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	user_mgr_index_locate(id, &pos, &found);
	if (!found) {
		err = -ENOENT;
		goto unlock;
	}

	err = user_mgr_store_delete(storage_dir, id);
	if (err) {
		goto unlock;
	}

	err = user_mgr_index_remove(id);

unlock:
	user_mgr_unlock();
	return err;
}

int user_mgr_set_status(uint32_t id, enum user_status status)
{
	struct user_record rec;
	struct user_index_entry e;
	int err = 0;

	if (!initialized) {
		return -EINVAL;
	}
	if (status != USER_STATUS_ENABLED && status != USER_STATUS_BLOCKED) {
		return -EINVAL;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	err = user_mgr_store_read(storage_dir, id, &rec);
	if (err) {
		goto unlock;
	}

	if (rec.status == (uint8_t)status) {
		goto unlock;
	}

	rec.status = (uint8_t)status;

	err = user_mgr_store_write(storage_dir, &rec);
	if (err) {
		goto unlock;
	}

	e.user_id = id;
	e.level = rec.level;
	e.status = (uint8_t)status;
	err = user_mgr_index_update(id, &e);

unlock:
	user_mgr_unlock();
	return err;
}

int user_mgr_get_by_id(uint32_t id, struct user_public *out)
{
	struct user_record rec;
	bool found = false;
	size_t pos = 0;
	int err = 0;

	if (!out || !initialized) {
		return -EINVAL;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	user_mgr_index_locate(id, &pos, &found);
	if (!found) {
		err = -ENOENT;
		goto unlock;
	}

	err = user_mgr_store_read(storage_dir, id, &rec);
	if (err) {
		goto unlock;
	}

	user_mgr_record_to_public(&rec, out);

unlock:
	user_mgr_unlock();
	return err;
}

int user_mgr_get_by_index(size_t idx, struct user_public *out)
{
	struct user_record rec;
	int err = 0;

	if (!out || !initialized) {
		return -EINVAL;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	if (idx >= index_count) {
		err = -ERANGE;
		goto unlock;
	}

	err = user_mgr_store_read(storage_dir, user_index[idx].user_id, &rec);
	if (err) {
		goto unlock;
	}

	user_mgr_record_to_public(&rec, out);

unlock:
	user_mgr_unlock();
	return err;
}

size_t user_mgr_count(void)
{
	if (!initialized) {
		return 0;
	}
	return index_count;
}

size_t user_mgr_count_blocked(void)
{
	size_t i = 0;
	size_t n = 0;

	if (!initialized || user_mgr_lock() != 0) {
		return 0;
	}

	for (i = 0; i < index_count; i++) {
		if (user_index[i].status == USER_STATUS_BLOCKED) {
			n++;
		}
	}
	user_mgr_unlock();
	return n;
}

size_t user_mgr_count_admins_enabled(void)
{
	size_t i = 0;
	size_t n = 0;

	if (!initialized || user_mgr_lock() != 0) {
		return 0;
	}

	for (i = 0; i < index_count; i++) {
		if (user_index[i].level >= ACC_LEVEL_ADMIN &&
		    user_index[i].status == USER_STATUS_ENABLED) {
			n++;
		}
	}
	user_mgr_unlock();
	return n;
}

int user_mgr_login(uint32_t id, const char *password, struct user_public *out)
{
	struct user_record rec;
	uint8_t candidate[USER_MGR_KEY_LEN];
	size_t pwd_len = 0;
	int err = 0;

	if (!initialized) {
		return -EINVAL;
	}
	err = user_mgr_validate_password(password, &pwd_len);
	if (err) {
		return err;
	}

	err = user_mgr_lock();
	if (err) {
		return err;
	}

	err = user_mgr_store_read(storage_dir, id, &rec);
	if (err) {
		goto unlock;
	}

	if (rec.status != USER_STATUS_ENABLED) {
		err = -EACCES;
		goto unlock;
	}

	err = user_mgr_derive(password, pwd_len, rec.salt, sizeof(rec.salt), rec.iterations,
			      candidate, sizeof(candidate));
	if (err) {
		goto unlock;
	}

	if (!user_mgr_ct_equal(candidate, rec.key, sizeof(candidate))) {
		err = -EACCES;
		goto unlock;
	}

	if (out) {
		user_mgr_record_to_public(&rec, out);
	}
	err = 0;

unlock:
	memset(candidate, 0, sizeof(candidate));
	user_mgr_unlock();
	return err;
}
