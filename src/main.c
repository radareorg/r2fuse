/* r2fuse - GPL-3.0-only - Copyright 2026 - pancake */

#define R_LOG_ORIGIN "r2fuse"

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif
#if __APPLE__
// use the portable libfuse3 api (macfuse enables struct fuse_darwin_attr by default)
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#endif

#include "rfs.h"
#include <r_getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>

#if R2__WINDOWS__ || _WIN32
#include <winfsp/winfsp.h>
// winfsp exposes its own portable types in the native (non-cygwin) build
#define R2F_STAT struct fuse_stat
#define R2F_STATVFS struct fuse_statvfs
#define R2F_OFF fuse_off_t
#define R2F_SET_MTIME(st, t) ((st)->st_mtim.tv_sec = (t))
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#define R2F_STAT struct stat
#define R2F_STATVFS struct statvfs
#define R2F_OFF off_t
#define R2F_SET_MTIME(st, t) ((st)->st_mtime = (t))
#endif

#ifndef S_IFDIR
#define S_IFDIR 0040000
#endif
#ifndef S_IFREG
#define S_IFREG 0100000
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif

#define BLKSIZE 4096

static R2F *r2f_ctx(void) {
	return (R2F *)fuse_get_context ()->private_data;
}

static void attr_to_stat(const R2FAttr *a, R2F_STAT *st) {
	memset (st, 0, sizeof (*st));
	// the volume is read-only, strip write bits and keep everything readable
	ut32 perm = a->perm & 0777 & ~0222;
	if (a->kind == R2F_KIND_DIR) {
		st->st_mode = S_IFDIR | perm | 0555;
		st->st_nlink = 2;
	} else if (a->kind == R2F_KIND_SPECIAL) {
		// shown as empty unreadable files until r_fs can resolve them
		st->st_mode = S_IFREG;
		st->st_nlink = 1;
	} else {
		st->st_mode = S_IFREG | perm | 0444;
		st->st_nlink = 1;
		st->st_size = (R2F_OFF)a->size;
	}
#if !(R2__WINDOWS__ || _WIN32)
	st->st_blksize = BLKSIZE;
	st->st_blocks = (a->size + 511) / 512;
	// image owners are meaningless on the host, show the mounting user instead
	st->st_uid = getuid ();
	st->st_gid = getgid ();
#endif
	R2F_SET_MTIME (st, (time_t)a->mtime);
}

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
	(void)conn;
	// contents never change under our feet, let the kernel cache everything
	cfg->kernel_cache = 1;
	cfg->entry_timeout = 3600;
	cfg->attr_timeout = 3600;
	cfg->negative_timeout = 3600;
	return r2f_ctx ();
}

static int op_getattr(const char *path, R2F_STAT *st, struct fuse_file_info *fi) {
	(void)fi;
	R2FAttr a;
	int ret = r2f_getattr (r2f_ctx (), path, &a);
	if (!ret) {
		attr_to_stat (&a, st);
	}
	return ret;
}

typedef struct {
	void *buf;
	fuse_fill_dir_t filler;
} DirCtx;

static bool dir_cb(void *user, const char *name, const R2FAttr *a) {
	DirCtx *d = user;
	R2F_STAT st;
	attr_to_stat (a, &st);
	return d->filler (d->buf, name, &st, 0, 0) == 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler, R2F_OFF off, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	(void)off; (void)fi; (void)flags;
	DirCtx d = { buf, filler };
	filler (buf, ".", NULL, 0, 0);
	filler (buf, "..", NULL, 0, 0);
	return r2f_readdir (r2f_ctx (), path, dir_cb, &d);
}

static int op_open(const char *path, struct fuse_file_info *fi) {
	if ((fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EROFS;
	}
	R2FHandle *h = NULL;
	int ret = r2f_open (r2f_ctx (), path, &h);
	if (!ret) {
		fi->fh = (uint64_t)(size_t)h;
		fi->keep_cache = 1;
	}
	return ret;
}

static int op_read(const char *path, char *buf, size_t size, R2F_OFF off, struct fuse_file_info *fi) {
	(void)path;
	R2FHandle *h = (R2FHandle *)(size_t)fi->fh;
	if (!h || off < 0) {
		return -EBADF;
	}
	return r2f_read (r2f_ctx (), h, (ut8 *)buf, size, (ut64)off);
}

static int op_release(const char *path, struct fuse_file_info *fi) {
	(void)path;
	r2f_close (r2f_ctx (), (R2FHandle *)(size_t)fi->fh);
	fi->fh = 0;
	return 0;
}

static int op_statfs(const char *path, R2F_STATVFS *sv) {
	(void)path;
	R2F *r = r2f_ctx ();
	memset (sv, 0, sizeof (*sv));
	sv->f_bsize = BLKSIZE;
	sv->f_frsize = BLKSIZE;
	sv->f_blocks = (r->image_size + BLKSIZE - 1) / BLKSIZE;
	sv->f_files = r_list_length (r->allnodes);
	sv->f_namemax = 255;
	return 0;
}

static const struct fuse_operations ops = {
	.init = op_init,
	.getattr = op_getattr,
	.readdir = op_readdir,
	.open = op_open,
	.read = op_read,
	.release = op_release,
	.statfs = op_statfs,
};

static int show_help(int ret) {
	printf ("Usage: r2fuse [-options] <image> [mountpoint]\n"
		" -t [fstype]   filesystem type (autodetected by default, see -L)\n"
		" -O [offset]   offset of the filesystem inside the image\n"
		" -p [type:n]   use the n-th partition of the given map (e.g. gpt:0)\n"
		" -P [type]     list the partitions of the given map and exit (mbr, gpt, ...)\n"
		" -m [opts]     r_fs mount options passed to the plugin\n"
		" -x            show deleted files too (R_FS_VIEW_ALL)\n"
		" -n [volname]  volume name (defaults to the image basename)\n"
		" -o [opts]     extra fuse mount options (can be repeated)\n"
		" -f            run in foreground\n"
		" -d            debug mode (fuse and r2 logs, implies -f)\n"
		" -c            check: walk the tree and print a manifest, no mounting\n"
		" -s            stress: like -c but also verify chunked/unaligned reads\n"
		" -b [size]     chunk size used by -s (default 4096)\n"
		" -L            list filesystem plugins\n"
		" -v            show version\n"
		" -h            show this help\n"
		"Examples:\n"
		"  r2fuse ext2.img /tmp/mnt\n"
		"  r2fuse -t iso9660 -f cdrom.iso /tmp/mnt\n"
		"  r2fuse -s fat.img\n"
		"  umount /tmp/mnt\n");
	return ret;
}

static bool select_partition(const char *image, const char *spec, ut64 *offset) {
	char *type = strdup (spec);
	char *colon = strchr (type, ':');
	int idx = 0;
	if (colon) {
		*colon = 0;
		idx = atoi (colon + 1);
	}
	bool ok = false;
	RList *parts = r2f_partitions (image, type);
	RFSPartition *p = parts? r_list_get_n (parts, idx): NULL;
	if (p) {
		*offset = p->start;
		ok = true;
	} else {
		R_LOG_ERROR ("Cannot find %s partition %d", type, idx);
	}
	r_list_free (parts);
	free (type);
	return ok;
}

static int list_partitions(const char *image, const char *type) {
	RList *parts = r2f_partitions (image, type);
	if (!parts) {
		return 1;
	}
	RListIter *iter;
	RFSPartition *p;
	int i = 0;
	r_list_foreach (parts, iter, p) {
		printf ("%s:%d 0x%08"PFMT64x" 0x%08"PFMT64x" type=0x%x\n", type, i++, p->start, p->length, p->type);
	}
	r_list_free (parts);
	return 0;
}

int main(int argc, const char **argv) {
	R2FOptions opt = { .view = R_FS_VIEW_NORMAL };
	const char *part = NULL;
	const char *ptype = NULL;
	const char *volname = NULL;
	bool foreground = false;
	bool debug = false;
	bool check = false;
	bool stress = false;
	size_t chunk = 4096;
	RList *fuseopts = r_list_new ();
	RGetopt go;
	int c;

	r_getopt_init (&go, argc, argv, "t:O:p:P:m:xn:o:fdcsb:Lvh");
	while ((c = r_getopt_next (&go)) != -1) {
		switch (c) {
		case 't': opt.fstype = go.arg; break;
		case 'O': opt.offset = r_num_math (NULL, go.arg); break;
		case 'p': part = go.arg; break;
		case 'P': ptype = go.arg; break;
		case 'm': opt.options = go.arg; break;
		case 'x': opt.view = R_FS_VIEW_ALL; break;
		case 'n': volname = go.arg; break;
		case 'o': r_list_append (fuseopts, (void *)go.arg); break;
		case 'f': foreground = true; break;
		case 'd': debug = foreground = true; break;
		case 'c': check = true; break;
		case 's': check = stress = true; break;
		case 'b': chunk = (size_t)r_num_math (NULL, go.arg); break;
		case 'L': r2f_list_plugins (); r_list_free (fuseopts); return 0;
		case 'v': printf ("r2fuse %s (r2 %s)\n", R2FUSE_VERSION, R2_VERSION); r_list_free (fuseopts); return 0;
		case 'h': r_list_free (fuseopts); return show_help (0);
		default: r_list_free (fuseopts); return show_help (1);
		}
	}
	const char *mountpoint = (go.ind + 1 < argc)? argv[go.ind + 1]: NULL;
	if (go.ind >= argc || (!mountpoint && !check && !ptype)) {
		r_list_free (fuseopts);
		return show_help (1);
	}
	opt.image = argv[go.ind];
	r_log_set_level (debug? R_LOG_LEVEL_DEBUG: R_LOG_LEVEL_WARN);
	if (ptype) {
		r_list_free (fuseopts);
		return list_partitions (opt.image, ptype);
	}
	if (part && !select_partition (opt.image, part, &opt.offset)) {
		r_list_free (fuseopts);
		return 1;
	}
#if R2__WINDOWS__ || _WIN32
	// winfsp-*.dll is delay loaded from the WinFsp install directory
	if (!check && !NT_SUCCESS (FspLoad (0))) {
		R_LOG_ERROR ("Cannot load WinFsp, is it installed?");
		r_list_free (fuseopts);
		return 1;
	}
#endif
	R2F *r = r2f_new (&opt);
	if (!r) {
		r_list_free (fuseopts);
		return 1;
	}
	int ret;
	if (check) {
		ret = r2f_check (r, "/", stress, chunk);
	} else {
		RStrBuf *mo = r_strbuf_new ("");
#if R2__WINDOWS__ || _WIN32
		r_strbuf_appendf (mo, "volname=%s", volname? volname: r_file_basename (opt.image));
#elif __APPLE__
		r_strbuf_appendf (mo, "ro,noappledouble,fsname=r2fuse,volname=%s", volname? volname: r_file_basename (opt.image));
#else
		r_strbuf_appendf (mo, "ro,fsname=r2fuse:%s,subtype=r2fuse", r_file_basename (opt.image));
		(void)volname;
#endif
		RList *args = r_list_newf (free);
		r_list_append (args, strdup (argv[0]));
		r_list_append (args, strdup ("-o"));
		r_list_append (args, r_strbuf_drain (mo));
		RListIter *iter;
		const char *o;
		r_list_foreach (fuseopts, iter, o) {
			r_list_append (args, strdup ("-o"));
			r_list_append (args, strdup (o));
		}
		if (foreground) {
			r_list_append (args, strdup ("-f"));
		}
		if (debug) {
			r_list_append (args, strdup ("-d"));
		}
		r_list_append (args, strdup (mountpoint));
		int fargc = r_list_length (args);
		char **fargv = R_NEWS0 (char *, fargc + 1);
		int i = 0;
		char *a;
		r_list_foreach (args, iter, a) {
			fargv[i++] = a;
		}
		R_LOG_INFO ("Mounting %s (%s) on %s", opt.image, r->fstype, mountpoint);
		ret = fuse_main (fargc, fargv, &ops, r);
		free (fargv);
		r_list_free (args);
	}
	r2f_free (r);
	r_list_free (fuseopts);
	return ret;
}
