.. _gzm_user_mgr:

user_mgr: account store with PBKDF2-HMAC-SHA256 passwords
##########################################################

Purpose
*******

Local account store with one file per user on a Zephyr filesystem
(LittleFS or FATFS). Passwords are never stored in clear: each file
keeps a random salt, the PBKDF2 iteration count and the derived key.
An in-RAM index sorted by id answers "does this id exist" and the
counters without touching the flash; the disk is read on login and
on the get calls, and written on create, update and delete.

The library has a single store per firmware. ``user_mgr_init()`` points
it at a directory; every other call works on that store.

Architecture
************

::

    user_mgr_init(storage_dir)
        ↓ mkdir storage_dir if missing
        ↓ read every <id>.bin, check version and CRC32
        ↓ fill the in-RAM index (id, level, status), sorted by id

    add / update / set_password / set_status / delete
        ↓ binary semaphore
        ↓ new salt + PBKDF2 when a password is set
        ↓ write <id>.bin with fs_mgr_save_file (write .new, then rename)
        ↓ update the index

    login(id, password)
        ↓ read <id>.bin (salt, iterations, key)
        ↓ PBKDF2(password, salt, iterations)
        ↓ constant-time compare with the stored key

Storage:

- One file per user, ``<storage_dir>/<id>.bin``: 126 bytes with the
  default name lengths (``USER_STORE_VERSION`` 3, CRC32 at the end).
- The index is a static array of ``CONFIG_GZM_USER_MGR_MAX`` entries
  (8 bytes each) inside the library.
- A file that fails the size, version or CRC check is skipped at init
  and logged; it does not stop the boot.

Public API
**********

.. code-block:: c

   int  user_mgr_init(const char *storage_dir);
   bool user_mgr_is_ready(void);
   int  user_mgr_create_default_admin(uint32_t id, const char *password,
                                      enum access_level level);

   int user_mgr_add(uint32_t id, const char *first_name, const char *last_name,
                    const char *password, enum access_level level);
   int user_mgr_update(uint32_t id, const char *first_name, const char *last_name,
                       enum access_level level);
   int user_mgr_set_password(uint32_t id, const char *new_password);
   int user_mgr_set_status(uint32_t id, enum user_status status);
   int user_mgr_delete(uint32_t id);

   int    user_mgr_get_by_id(uint32_t id, struct user_public *out);
   int    user_mgr_get_by_index(size_t idx, struct user_public *out);
   size_t user_mgr_count(void);
   size_t user_mgr_count_blocked(void);
   size_t user_mgr_count_admins_enabled(void);

   int user_mgr_login(uint32_t id, const char *password, struct user_public *out);

Errors are negative errno values: ``-EEXIST`` (duplicate id),
``-ENOSPC`` (store full), ``-ENOENT`` (unknown id), ``-EACCES`` (wrong
password or blocked user), ``-EINVAL`` (bad argument or not
initialised).

Shell (``CONFIG_GZM_USER_MGR_SHELL``): ``user count``, ``show``,
``show_all``, ``add``, ``del``, ``set_pwd``, ``set_status``,
``rename``, ``login`` and ``levels``.

Kconfig:

- ``CONFIG_GZM_USER_MGR``: enable. Depends on ``FILE_SYSTEM`` and a PSA
  Crypto provider; selects ``GZM_FS_MGR``, ``CRC``,
  ``PSA_WANT_ALG_PBKDF2_HMAC``, ``PSA_WANT_ALG_HMAC``,
  ``PSA_WANT_ALG_SHA_256`` and ``ENTROPY_GENERATOR``.
- ``CONFIG_GZM_USER_MGR_MAX`` (50): store capacity and index size.
- ``CONFIG_GZM_USER_MGR_FIRST_NAME_LEN`` (24) and
  ``CONFIG_GZM_USER_MGR_LAST_NAME_LEN`` (36): name lengths.
- ``CONFIG_GZM_USER_MGR_PASSWORD_MAX_LEN`` (32): longest accepted
  password; the password itself is never stored.
- ``CONFIG_GZM_USER_MGR_SALT_LEN`` (16): salt size in bytes.
- ``CONFIG_GZM_USER_MGR_PBKDF2_ITERATIONS`` (10000): work factor.

``enum access_level`` lives in ``gzm/access_level.h``.

Design decisions
****************

**Why one file per user.** Each update rewrites only one small file,
and ``fs_mgr_save_file`` makes that write power-cut safe (write
``.new``, then rename). A single file with every account would need
a rewrite of the whole list on each change.

**Why PBKDF2-HMAC-SHA256.** It is available through the PSA Crypto API
on every Zephyr target, and the iteration count can be tuned to the
CPU. The iteration count is stored per record, so raising
``CONFIG_GZM_USER_MGR_PBKDF2_ITERATIONS`` does not invalidate the
existing passwords.

**Why a constant-time comparison.** A plain ``memcmp`` returns as soon
as a byte differs, and the response time would tell how many bytes of
the derived key matched.

**Why an in-RAM index.** The id lookups and the counters run on every
add and on every refresh of the display; with the index they never
touch the flash. Only the fields they need (id, level, status) live
in RAM.

**Why ``create_default_admin`` exists.** The first boot needs an
account that can create the others. It only acts on an empty store,
so a re-init never overwrites a real admin.

Pitfalls and invariants
***********************

- ``add`` returns ``-ENOSPC`` once ``CONFIG_GZM_USER_MGR_MAX`` users
  exist, blocked users included.
- Passwords longer than ``PASSWORD_MAX_LEN`` are refused with
  ``-EINVAL``, never truncated.
- The salt comes from ``sys_csrand_get``: the target needs an entropy
  source.
- The store lock is held during the PBKDF2 derivation: with a high
  iteration count, a login blocks other calls for that long.
- Erasing the store brings back the default admin on the next boot.
  Its password is the one the application passes to
  ``create_default_admin``.

Tests
*****

In this repository (see the README, chapters 03 and 04):

- ``tests/user_mgr``: CRUD, login, validation, re-init from disk and
  the 50-user limit, on ``native_sim`` and on the board's SPI NOR.
- ``tests/user_mgr_mock``: storage faults forced with FFF fakes
  (disk full, failed delete, corrupted record) and a check that the
  password is never written in clear. PC only.
- ``tests/robot``: the same store through the app shell, before and
  after a firmware update.
