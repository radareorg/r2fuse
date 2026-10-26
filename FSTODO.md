# FSTODO: r_fs gaps found while building r2fuse

These are the things in `libr/fs` that r2fuse had to work around or that stop
a FUSE volume from behaving like a real one. Each item lists where it is, what
breaks, and a proposed fix. The `r2fuse -s` stress mode and `make check`
(`test/images.txt`) reproduce most of them.

Status legend: **[api]** core r_fs API, **[plugin]** a specific plugin,
**[detect]** autodetection, **[workaround]** r2fuse already works around it.

---

## 1. API

### 1.1 `r_fs_read` buffer ownership is undefined [api] [workaround]

> **Crash fixed in radare2 (fs.c):** `r_fs_read` now allocates a buffer of
> `len + 1` bytes on every call. Before, a 1-byte read followed by a full read
> on fat.img wrote 19605 bytes into a 2-byte buffer and crashed inside
> `io_cache.c`. `fs_io_read` no longer leaks the previous buffer. The
> caller-owned buffer API proposed below is still the better long-term fix.

- **Where:** `fs.c:r_fs_read`, all `p/fs_*.c` `read` callbacks.
- **Problem:** `r_fs_read` allocates `file->data` (`len + 1` bytes) only when
  it is NULL. After that, plugins do different things:
  - grub-based plugins (ext2, fat, hfs, iso9660, ntfs, ...) and squashfs write `len` bytes into the buffer that already exists.
  - apfs, fatmacho, sep64, p9, posix and tmp free it and malloc a new one.
  - fossil reallocs it.
  - io allocates a new one without freeing the old (leak).

  So a second read that is larger than the first overflows the heap on grub
  and squashfs.
- **Workaround in r2fuse:** `R_FREE (file->data)` before every `r_fs_read`.
- **Proposal:** add `R_API int r_fs_read_at(RFS *fs, RFSFile *f, ut64 addr, ut8 *buf, int len)`
  with a caller-owned buffer, and change the plugin callback to
  `int (*read)(RFSFile *f, ut64 addr, ut8 *buf, int len)`. Keep `file->data`
  only for `slurp`.

### 1.2 Read size and file size limits [api]
- `RFSFile.size` is `ut32`, so files of 4 GiB or more are truncated (ISO, UDF, NTFS, APFS and XFS can all hold them).
- `len` is an `int` in `r_fs_read` and in the plugin `read` callback.
- **Proposal:** make `size` a `ut64` and `len` a `size_t`/`ut64`, with a separate return value for errors.

### 1.3 `file->size` changes after a read [api] [workaround]
- `fs_tmp.c` sets `file->size = tocopy` on every read, so after one read the size no longer means "file size".
- **Workaround:** r2fuse stores the size it got at open time in its handle.
- **Proposal:** document `size` as immutable after `open`. Return the byte count only through the read return value.

### 1.4 No `stat` [api]
- `r_fs_dir` entries from grub-based plugins have `size == 0` (the grub dirhook
  has no size). The only way to get a file's size is to `open` it, which on
  grub also does a 1-byte read to resolve `off`. `ls -l` on a directory
  therefore costs one open per entry.
- **Proposal:** add a `stat` callback,
  `bool (*stat)(RFSRoot *root, const char *path, RFSFile *out)`, plus
  `R_API RFSFile *r_fs_stat(RFS *fs, const char *path)`, filling type, size,
  times, perm, uid, gid, inode and nlink. Plugins that can fill the size
  cheaply in `dir` should do so.

### 1.5 File types are inconsistent [api] [workaround]
- `r_fs.h` defines `r`, `d`, `x`, `s`, `b`, `c`, `m` and the upper-case
  "symlinked" variants (`R`, `D`, ...). In practice:
  - grub plugins use `'f'` for regular files, which is not a defined constant.
  - ubifs uses `'l'` for symlinks, also undefined.
  - reiserfs and xfs report symlinks as `'f'`, and opening them fails
    (`reiserfs-link-root.img`, `xfs-symlink-size.img`).
  - no plugin uses the upper-case variants.
- **Proposal:**
  - define `R_FS_FILE_TYPE_SYMLINK 'l'`, `R_FS_FILE_TYPE_FIFO 'p'` and `R_FS_FILE_TYPE_SOCKET 'S'`;
  - drop or document the upper-case variants;
  - migrate grub's `'f'` to `'r'`.
- **Workaround:** r2fuse treats every type that is neither a directory nor a
  special type as a regular file. `l`, `s`, `b` and `c` are shown as empty,
  unopenable files.

### 1.6 No symlink support [api]
- There is no way to get a link target, so FUSE cannot implement `readlink`.
  ubifs.img alone has 174 symlinks (`/bin/*` busybox links), and they show up as empty files.
- **Proposal:** add `char *(*readlink)(RFSRoot *root, const char *path)` and
  `R_API char *r_fs_readlink(RFS *fs, const char *path)`. Plugins to cover
  first: ubifs, ext2 (grub has `read_symlink`), squashfs, xfs, reiserfs, bfs, apfs, hfsplus.

### 1.7 Metadata is mostly empty or has unclear units [api]
- `perm`, `uid` and `gid` are filled only by a few plugins (apfs, bfs, fossil,
  squashfs, posix, p9, tmp, ubifs, zip). `time` has no documented unit or
  epoch. There is no atime/ctime/btime and no inode number, so hardlinks
  cannot be detected and FUSE `use_ino` is impossible.
- **Proposal:** document `time` as the mtime in unix seconds (UTC). Add `ut64 ino`,
  `ut32 nlink`, `ut64 atime`, `ut64 ctime` and `ut64 btime` to `RFSFile`.

### 1.8 Errors cannot be told apart [api]
- `r_fs_dir` returns NULL for "not found", "not a directory", "I/O error" and
  sometimes for "empty". `r_fs_open` and `r_fs_mount` return NULL with only a
  log line. FUSE needs `ENOENT`/`ENOTDIR`/`EIO`/`EINVAL`.
- **Proposal:** add an `int err` (errno-style) to `RFS`, set by the core and
  plugins on failure, or `R_API int r_fs_last_error(RFS *fs)`.

### 1.9 Thread safety [api] [workaround]
- grub-based plugins use global state: `grubfs_bind_io`, `grub_hack_lastoff`,
  the `R_TH_LOCAL` dirhook list and `grub_fshelp_view`. The `RFS` lists are not
  locked either.
- **Workaround:** r2fuse serializes every r_fs call with one lock.
- **Proposal:** move that state into `RFSRoot`/`RFSFile`, or add a plugin flag
  (`.threadsafe = true`) so frontends can parallelize the plugins that allow it.

### 1.10 No volume information [api]
- There is no API for total and free space, block size, label, UUID or case
  sensitivity. `details` prints free text only. grub's `case_insensitive` flag
  is dropped in `fs_grub_base.inc.c`.
- **Proposal:** add `bool (*statfs)(RFSRoot *root, RFSStatfs *out)` with
  `size`, `free`, `bsize`, `label`, `uuid`, `case_insensitive` and `readonly`.
  r2fuse would use it for `statfs` and as the default `volname`.

### 1.11 `r_fs_slurp` is broken [api]

> **Fixed in radare2 (fs.c):** it now strips the root path, reads through
> `r_fs_read`, closes the plugin handle, stops at the first root that works
> and frees the root list. `r_fs_open` also leaked its root list. The text below
> describes the old behavior.

- **Where:** `fs.c:r_fs_slurp`.
- It passes the full path to `root->p->open` without stripping
  `root->path`, unlike `r_fs_open`, so it fails for any mount that is not at `/`.
- It calls `root->p->read (file, 0, file->size)` without allocating
  `file->data`, which is a NULL write for grub and squashfs.
- With several roots, it loops on, leaking the previous `file`, and never
  calls `close`.
- **Proposal:** implement it as `r_fs_open` + `r_fs_read_at` into a new
  buffer + `r_fs_close`.

### 1.12 Plugin and mount lifecycle [api]
- `r_fs_plugin_remove` and `r_fs_del` are empty stubs.
- `r_fs_close` frees `data` and calls the plugin's `close`, but the caller must
  still call `r_fs_file_free`. Merge them, or rename `close` to make that clear.

### 1.13 Partition info is thin [api]
- `RFSPartition.type` is 0 for every GPT partition. GPT GUIDs, names and
  attributes are not exposed, so frontends cannot guess the filesystem of a partition.
- **Proposal:** add `char *name` and `char guid[37]` and fill them in `part_gpt.c`.

---

## 2. Autodetection (`fs.c:fstypes`, `r_fs_name`)

| Problem | Image | Proposal |
|---|---|---|
| FAT16 has no entry. Only `FAT12` at 0x36 and `FAT32` at 0x52 are matched | (any FAT16 image) | add `{ "fat", 0x36, "FAT16", 5 }` |
| UDF images are detected as `iso9660`, because the `CD001` entry comes before `BEA01` and UDF bridge discs have both | `udf.img` | check `BEA01`/`NSR02`/`NSR03` at 0x8000-0x9800 before iso9660 |
| No magic for `sfs` (Amiga `SFS\0` at block 0) | `sfs-*.img` | add the entry |
| No magic for `ufs`/`ufs2` (0x011954 / 0x19540119 at 0x55c of the superblock at 8 KiB or 64 KiB) | — | add the entries |
| Only the first match counts, and the plugin's `mount` is never used to confirm it | — | try each matching candidate with `mount` and return the first that succeeds |
| Compressed images (`apfs_evil.img.gz`) are not detected | `apfs_evil.img.gz` | frontends can open them through a gzip io URI. Document it |

---

## 3. Plugins (from `make check`)

| Plugin | Image | Symptom | Notes |
|---|---|---|---|
| bfs | `haiku.bfs` | `/radare2_test/gvpr`: reads stop at 2048 of 5736 bytes, and later reads return 0 | `fs_bfs_read` only looks at the block run that contains `addr` ("read from direct blocks only"). Indirect and double-indirect runs are needed |
| ext2 (grub) | `ext4_advanced.img` | mount fails | incompat flags 0x22c2: extents, 64bit, flex_bg and **metadata_csum_seed** (0x2000). grub rejects unknown incompat bits. Allow it read-only |
| ntfs | `ntfs.img` | `$Extend/$ObjId`, `$Quota` and `$Reparse` are listed but cannot be opened | these metafiles have no unnamed `$DATA` stream. Report them as special, or expose their named streams |
| ntfs | `ntfs-resident-oob.img` | `/file` cannot be opened | crafted image. Make sure the failure is a clean error |
| reiserfs | `reiserfs-link-root.img`, `reiserfs-corrupt.img` | `link` is reported as `'f'` and cannot be opened | symlink type missing (see 1.5, 1.6) |
| xfs | `xfs-symlink-size.img` | `link` is reported as `'f'` | same as above |
| ubifs | `ubifs.img` | 174 symlinks reported as `'l'` | readlink support (1.6) |
| ubifs | `ubifs-oversize.img` | `File size (11) exceeds filesystem size (0)`, then 0 bytes are read | `leb_cnt` is 0 here. The size clamp should not use the volume size when that size is unknown |
| iso9660 | `iso.img` | level-1 names without an extension keep the dot: `LS.`, `SERVICES.` | strip the trailing `.` together with the `;1` version |
| hfs | `hfs-oobread.img` | refuses to mount | crafted image, the expected result |
| bfs | `bfs-sibling-cycle.img` | refuses to mount | crafted image, the expected result |
| sfs | `sfs-blocksize-0.img` | refuses to mount | crafted image, the expected result |

---

## 4. Nice to have

- Directory handles: grub resolves the full path on every `open`/`dir`, so
  deep trees are O(depth) per lookup. A `lookup(parent, name)` callback, or a
  dentry cache in the core, would help every frontend.
- `R_FS_VIEW_DELETED` only works on grub fshelp-based plugins. Document which plugins support it.
- The name encoding is not specified (UTF-8?). Plugins should convert
  UTF-16 (NTFS, Joliet, HFS+, APFS) and legacy codepages (FAT, ISO level 1) to UTF-8.
- Extended attributes and alternate data streams (`listxattr`/`getxattr`), for HFS+/APFS resource forks and NTFS ADS.
- Write support (later): `write` and `mkdir` exist in the plugin struct, but
  there is no `truncate`, `rename`, `rmdir`, `setattr` or `fsync`.

---

## 5. radare2 build issues hit while producing static r2fuse binaries

| Issue | Status |
|---|---|
| meson: `fs_plugins` in `libr/meson.build` lacks `fossil`, `p9`, `tmp` (compiled but never registered) and `apfs` (not compiled at all), so meson builds have no APFS support | fixed in the working tree (`libr/meson.build`, `libr/fs/meson.build`) |
| meson: `libr/util/meson.build` links the system liblz4 whenever it is found, even with `use_sys_lz4=false` when the bundled rlz4 is used, so a dylib leaks into static builds | fixed in the working tree |
| msvc: `libr/util/arena.c` uses C11 `_Static_assert`, which fails without `/std:c11`, so r2 git master does not build on Windows | fixed in the working tree (array-size typedef check) |
| meson `--default-library=static`: the CLI tools and unit tests fail to link, with `gperf_*` symbols undefined from `r_asm_get_gperf` (`libr_asm.a`) and `r_anal_get_gperf_cc` (`libr_anal.a`). They are generated only with `sdb_cgen` | **open**, worked around with `-Dcli=disabled -Denable_tests=false` (r2fuse does not link r_asm/r_anal) |
| `sys/static.sh` always configures `--without-gpl`, so its static libs have no grub filesystems | open, by design? r2fuse builds with meson instead |
