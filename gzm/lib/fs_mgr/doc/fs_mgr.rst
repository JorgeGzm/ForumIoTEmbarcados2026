.. _gzm_fs_mgr:

fs_mgr: filesystem mount and high-level file helpers
#####################################################

Purpose
*******

Wraps Zephyr's ``zephyr/fs/fs.h`` mount-point + path API into a set
of one-shot helpers (``save_file``, ``load_file``, ``read_file``, ``delete_file``,
``list_files``, ``mem_info``) that the rest of the firmware uses
without touching ``fs_open`` / ``fs_seek`` / ``fs_close`` directly.
Provides a single mount-with-recovery flow, a power-cut safe
write-then-rename helper, and free-space management.

Reach for it whenever the firmware needs to persist a config blob,
a fault dump, an OTA descriptor, or any other small file. In this
project :ref:`gzm_user_mgr` stores one file per user through it.

Architecture
************

::

    fs_mgr_init(struct fs_mount_t *)
        │
        ▼
    fs_mount → if fail → fs_mgr_format → fs_mount
        │
        ▼ caller's mount point is live (e.g. /lfs)

    application threads
        │  fs_mgr_save_file / load_file / recover
        │  fs_mgr_read_file / delete_file / list_files
        ▼
    +---------------------------------+
    | k_sem g_xfs_mutex (count 1)    |  5 s timeout
    |   open → seek → read/write →   |
    |   close                         |
    +---------------------------------+
        │
        ▼
    Zephyr fs/fs.h backend (FATFS / LittleFS / ...)

The lib does not own the ``fs_mount_t``: the caller passes it to
``init``. The backend (FATFS, LittleFS) is whatever the
project's BSP and Kconfig wire up. fs_mgr's helpers take a
``path`` plus ``file_name`` so the application never has to splice
mount points into paths.

A single ``k_sem`` (count 1, 5 s timeout) serialises every fs_mgr
call. Concurrent FS access from outside fs_mgr is not coordinated
by this lock; each backend has its own concurrency story.

Public API
**********

Lifecycle:

.. code-block:: c

   int fs_mgr_init(struct fs_mount_t *fsmout);     /* mount, format on fail */
   int fs_mgr_deinit(struct fs_mount_t *fsmout);
   int fs_mgr_format(struct fs_mount_t *fsmout);

Power-cut-safe save and load (``<name>.new`` + rename):

.. code-block:: c

   int fs_mgr_save_file(const char *path, const char *file_name,
                        const void *buf, int buflen);
   int fs_mgr_load_file(const char *path, const char *file_name,
                        void *buf, int buflen);
   int fs_mgr_recover(const char *path, const char *file_name);

One-shot file ops:

.. code-block:: c

   int fs_mgr_read_file(const char *path, const char *file_name,
                        void *buf, int buflen);
   int fs_mgr_delete_file(const char *path, const char *file_name);
   int fs_mgr_get_file_size(const char *path, const char *file_name,
                            int *file_size);

Directory ops and bulk:

.. code-block:: c

   int fs_mgr_search_file_in_dir(const char *path, char *fname, int fname_len,
                                 int *file_size, int *num_files_found);
   int fs_mgr_get_num_files_in_dir(const char *path, int *num_files,
                                   int *total_size);
   int fs_mgr_list_files(const char *path,
                         struct fs_mgr_file_entry *entries, int max_entries);
   int fs_mgr_delete_files_of_path(const char *path);
   int fs_mgr_release_space_in_path(const char *path, int size);
   int fs_mgr_mkdir_if_not_exists(const char *path);
   int fs_mgr_mem_info(struct fs_mount_t *fsmout, enum fs_mgr_mem_info ch,
                       float *val);

Partial read and write at an offset (``FS_MGR_OFFSET_END`` appends):

.. code-block:: c

   int fs_mgr_pread(const char *path, void *buf, uint32_t buflen,
                    uint32_t offset, enum fs_mgr_open_mode mode);
   int fs_mgr_pwrite(const char *path, const void *buf, uint32_t buflen,
                     uint32_t offset, enum fs_mgr_open_mode mode);
   int fs_mgr_preallocate(const char *path, uint32_t size);

Kconfig:

- ``CONFIG_GZM_FS_MGR``: enable (depends on ``FILE_SYSTEM``).

Design decisions
****************

**Why ``init`` formats on a failed mount.** Some deployments ship
without a pre-formatted SD card; the first boot must format it
automatically rather than refuse to start. The format-on-fail
fallback is the simplest user-visible behaviour. Production
projects that want to refuse a missing card override this by
checking ``fs_mount`` directly first.

**Why the save is write-rename, not journalled.** Most
backends (FATFS, LittleFS) make ``fs_rename`` atomic at the
directory-entry level. Writing a ``.new`` file, fsync'ing, and
renaming over the final name covers the power-cut case without
requiring a journal, which neither FAT nor LittleFS expose. CRC
guarding is the caller's responsibility, because different consumers
want different framing.

**Why ``fs_mgr_recover`` is idempotent.** A power cut can leave a
``.new`` alone, a ``.new`` plus the previous final, or nothing.
``fs_mgr_recover`` resolves all three to "the most recent complete
file is the final name" without the caller having to know which
state it inherited. Calling it on a clean filesystem is a no-op.

**What ``release_space_in_path`` deletes.** Files of the directory,
in the order the filesystem lists them, until the requested size is
freed. That order is not guaranteed to be the oldest first: a caller
that needs it must name its files so the listing order matches.

**Why no async / streaming API.** Every helper is synchronous and
operates on a complete file body in one locked section. Continuous
streaming (logs written all the time) is out of scope: fs_mgr stays
focused on the "blob of N bytes" case.

**Why ``format`` dispatches ``fs_mkfs`` ``dev_id`` by FS type.**
``fs_mkfs`` is filesystem-specific in what it expects as ``dev_id``:
FATFS wants a drive path string without a leading ``/`` (e.g.
``"SD:"``); LittleFS wants the flash_area partition id stored in
``mount->storage_dev``. Passing the wrong form returns ``-ENODEV``
silently and leaves the mount as-is. Branching on ``fsmout->type``
is the only reliable way to support both backends from one call.

Pitfalls and invariants
***********************

- ``fs_mgr_init`` will format the device if mount fails. On a
  device that should never auto-format (because the data is
  precious), guard the init call upstream.

- The 5 s mutex timeout means a long-running ``list_files`` on a
  big directory can starve concurrent callers. The lib has no
  per-operation budget; fix this at call sites.

- ``fs_mgr_save_file`` adds **no CRC**. Pair with a header CRC if data
  corruption inside a complete file is a concern.

- ``fs_mgr_load_file`` calls ``fs_mgr_recover`` first, so it is the
  "use this to read a config blob" path. Plain ``fs_mgr_read_file``
  does not recover: use it only for files written outside
  ``fs_mgr_save_file``.

- There is no in-place save any more: ``fs_mgr_save_file`` always
  writes ``<name>.new`` and renames it, so a power cut mid-write
  keeps either the previous or the new complete file.

- The lock protects fs_mgr calls but not the underlying FS: if
  the application calls ``fs_open`` directly elsewhere, it bypasses
  this lock.

Tests
*****

``tests/fs_mgr/`` (``native_sim``, LittleFS on the flash simulator):

- save and load, including files larger than the 512-byte write chunk
- recovery of an interrupted save: only ``.new`` on disk, ``.new``
  plus the previous file, nothing to recover
- ``mkdir_if_not_exists`` on nested paths, and its errors
- count, list (full and truncated) and search in a directory
- delete one file, delete a directory's files, release space
- ``pread`` / ``pwrite`` at an offset and in append mode
- ``preallocate`` and ``mem_info``
- files survive ``deinit`` and ``init``; ``format`` wipes them

The same suite runs on the board (scenario ``demo.fs_mgr.board``),
on LittleFS over the SPI NOR, and ``tests/user_mgr`` stores its users
through fs_mgr.

Roadmap
*******

- The format-on-fail behaviour is unconditional. A Kconfig to
  refuse format would let projects detect a bad SD card and alarm
  rather than silently wipe.
- ``list_files`` walks the directory under the mutex; very large
  directories starve other callers. Chunked iteration is on the
  table.
