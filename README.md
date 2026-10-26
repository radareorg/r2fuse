# r2fuse

[![CI](https://github.com/radareorg/r2fuse/actions/workflows/ci.yml/badge.svg)](https://github.com/radareorg/r2fuse/actions/workflows/ci.yml)

Mount disk images as real, **read-only** volumes without root or kernel
drivers. `r2fuse` uses the `r_fs` library from
[radare2](https://github.com/radareorg/radare2) and serves the files through
FUSE (libfuse3 / macFUSE / Fusetta / WinFsp), so `ls`, `cp`, Finder or Explorer
just work on the image.

```sh
r2fuse disk.img ~/mnt    # mount (type is autodetected)
ls ~/mnt
umount ~/mnt             # Linux: fusermount3 -u ~/mnt
```

## Supported filesystems

Whatever your radare2 build provides; `r2fuse -L` prints the exact list.
With current radare2:

| Type | Formats |
|---|---|
| Disk | ext2/3/4, FAT, NTFS, XFS, UFS, JFS, ReiserFS, MINIX, BFS (Haiku), SFS (Amiga), Fossil (Plan 9), HFS, HFS+, APFS |
| Optical / embedded | ISO9660 (Joliet, Rock Ridge), UDF, SquashFS, UBIFS |
| Containers | tar, zip, fat Mach-O slices, Apple SEP64 firmware |

Partition tables (MBR, GPT...) are handled with `-P` / `-p`, see Usage.

The second goal is to **stress test `r_fs` and its plugins**. Every `ls`,
`cat` or `cp` from the OS becomes a burst of `r_fs_dir`, `r_fs_open` and
`r_fs_read` calls with arbitrary offsets and sizes. The `-s` mode replays the
same workload without a kernel, so plugin bugs can be found in CI. Known gaps
are tracked in [FSTODO.md](FSTODO.md).

## Platforms

| OS | FUSE provider | Notes |
|---|---|---|
| macOS 27+ | [Fusetta](https://github.com/hsorbo/fusetta) (FSKit, no kext) | recommended |
| macOS | [macFUSE](https://macfuse.github.io/) | works with its libfuse3 |
| Linux | libfuse3 (`fuse3`, `libfuse3-dev`) | |
| Windows | [WinFsp](https://winfsp.dev/) (FUSE3 compatibility layer) | build with meson + MSVC |

## Downloads

The [releases](https://github.com/radareorg/r2fuse/releases) page has one
self-contained binary per platform: Linux x86_64/arm64 (static musl), macOS
arm64 (for Fusetta) and Windows x64. Linux links radare2 and libfuse statically;
macOS and Windows bundle the libraries from the latest radare2 release.
Keep the archive's binary and libraries together; radare2 does not need to
be installed. You only need the OS
FUSE provider: `/dev/fuse` on Linux, Fusetta on macOS, WinFsp on Windows.

To build the same thing locally (radare2 and libfuse are compiled from source
into `build-static/`):

```sh
make static                      # clones radare2 master
R2_SRC=../radare2 make static    # or use a local checkout
```

## Building

Requirements: radare2 (with `pkg-config r_fs r_io` working), libfuse3 and a C compiler.

```sh
make            # produces ./r2fuse
make check      # stress test all images listed in test/images.txt
sudo make install
```

or with meson:

```sh
meson setup build
meson compile -C build
meson test -C build
```

### macOS with Fusetta

Install the Fusetta app and its patched libfuse3:

```sh
brew tap hsorbo/tap
brew install --cask fusetta          # Fusetta.app + fusermount3
brew install hsorbo/tap/libfuse      # libfuse3 with the Fusetta mount backend
open -a Fusetta                      # enable it in System Settings > General >
                                     # Login Items & Extensions > File System Extensions
make
```

The Makefile picks up `$(brew --prefix)/opt/libfuse/lib/pkgconfig` automatically
and embeds its rpath, so `r2fuse` uses Fusetta's libfuse even when macFUSE's
`libfuse3` is also installed in `/usr/local/lib`. Check with
`otool -L r2fuse | grep fuse`. To point at a custom libfuse build, set
`PKG_CONFIG_PATH=/path/to/prefix/lib/pkgconfig make`.

### Linux

```sh
sudo apt install libfuse3-dev fuse3 pkg-config
make
```

### Windows

Install [WinFsp](https://winfsp.dev/rel/) (with the "Developer" feature) and a
radare2 release, then build from a Visual Studio developer prompt:

```sh
meson setup build -Dr2_prefix=C:\radare2
meson compile -C build
```

`-Dwinfsp_prefix=` overrides the WinFsp location (default `C:/Program Files (x86)/WinFsp`).

## Usage

```
Usage: r2fuse [-options] <image> [mountpoint]
 -t [fstype]   filesystem type (autodetected by default, see -L)
 -O [offset]   offset of the filesystem inside the image
 -p [type:n]   use the n-th partition of the given map (e.g. gpt:0)
 -P [type]     list the partitions of the given map and exit (mbr, gpt, ...)
 -m [opts]     r_fs mount options passed to the plugin
 -x            show deleted files too (R_FS_VIEW_ALL)
 -n [volname]  volume name (defaults to the image basename)
 -o [opts]     extra fuse mount options (can be repeated)
 -f            run in foreground
 -d            debug mode (fuse and r2 logs, implies -f)
 -c            check: walk the tree and print a manifest, no mounting
 -s            stress: like -c but also verify chunked/unaligned reads
 -b [size]     chunk size used by -s (default 4096)
 -L            list filesystem plugins
 -v            show version
 -h            show this help
```

Mount and unmount:

```sh
mkdir -p ~/mnt/img
r2fuse ext4.img ~/mnt/img          # daemonizes; -f to stay in foreground
ls -l ~/mnt/img
umount ~/mnt/img                   # Linux: fusermount3 -u ~/mnt/img
```

More examples:

```sh
r2fuse -L                                  # supported filesystems
r2fuse -t udf disc.img ~/mnt/disc          # force the type when autodetection is wrong
r2fuse -P gpt disk.img                     # list partitions
r2fuse -p gpt:1 disk.img ~/mnt/part1       # mount the second GPT partition
r2fuse -O 0x100000 disk.img ~/mnt/part     # mount at an explicit offset
r2fuse -x fat.img ~/mnt/fat                # include deleted entries
```

The image can be any radare2 io URI, not only a file path.

On Windows, the mountpoint is a free drive letter (`r2fuse fat.img X:`) or a
directory that does not exist yet.

## Testing and stress mode

`r2fuse -c image` walks the whole tree through the same code paths the FUSE
callbacks use, and prints a manifest with the type, size, FNV-1a hash and path
of every entry:

```
d          0 0000000000000000 /
f         24 18176a581fb4a69d /README.md
d          0 0000000000000000 /etc
f      19605 c960f1896b3e8434 /etc/services
# fat: 3 dirs, 3 files, 0 specials, 0 errors
```

`r2fuse -s image` does the same and also checks, for every file:

- the `getattr` size matches the `open` size;
- a full read returns exactly `size` bytes;
- reads in `-b` sized chunks, and unaligned reads at the start, middle and tail,
  return the same bytes as the full read;
- reading past EOF returns nothing.

`make check` runs `-s` on every image in [test/images.txt](test/images.txt)
from the [radare2-testbins](https://github.com/radareorg/radare2-testbins)
`fs/` directory (default `../radare2/test/bins/fs`, override with
`make check TESTBINS=...`). Each image has an expected result. Plugin bugs show
up as `XFAIL`, and when one gets fixed it shows up as `XPASS` and fails the run
until its expectation is updated. Malformed images pass only when they are
rejected with the expected error; crashes and timeouts always fail.
The check/stress commands return 0 on success, 1 on a failed check, and 2 if
the image cannot be opened or mounted. Set `R2FUSE_MOUNT=1` to also mount every image and compare file sizes
as seen through the kernel.

## How it works

- `src/rfs.c` wraps `r_fs`. It opens the image with `r_io`, autodetects the
  type with `r_fs_name` and mounts it at `/` of an `RFS` instance. It also
  keeps a path cache (the image is read-only, so listings and attributes never go stale).
- `src/main.c` maps libfuse3 operations (`getattr`, `readdir`, `open`, `read`,
  `release`, `statfs`) onto that wrapper and parses the command line.
- `src/check.c` is the `-c`/`-s` walker.

All `r_fs` calls are serialized behind one lock, because several plugins
(everything based on grub) keep global state. Writes are rejected with `EROFS`.
Entries that `r_fs` cannot resolve yet (symlinks, devices) appear as empty
files. See [FSTODO.md](FSTODO.md) for the list of `r_fs` features needed to do better.

## License

GPL-3.0-only, see [LICENSE](LICENSE). The release archives include radare2
and its GPL-licensed grub filesystem plugins.
