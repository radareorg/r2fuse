/* r2fuse - GPL-3.0-only - Copyright 2026 - pancake */

#define R_LOG_ORIGIN "r2fuse.check"

#include "rfs.h"

// Walks the tree through the same entry points used by the fuse callbacks,
// prints a manifest and, in stress mode, verifies that chunked and unaligned
// reads return the same bytes as a single whole-file read.

typedef struct {
	R2F *r;
	bool stress;
	size_t chunk;
	int errors;
	int files;
	int dirs;
	int specials;
	const char *dir;
	RList *subdirs;
} CheckCtx;

static ut64 fnv1a(const ut8 *buf, size_t len) {
	ut64 h = 0xcbf29ce484222325ULL;
	size_t i;
	for (i = 0; i < len; i++) {
		h ^= buf[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static void fail(CheckCtx *c, const char *path, const char *msg) {
	printf ("FAIL %s: %s\n", path, msg);
	c->errors++;
}

// read [off, off+len) with requests of at most chunk bytes
static st64 read_range(R2F *r, R2FHandle *h, ut8 *buf, ut64 off, size_t len, size_t chunk) {
	size_t done = 0;
	while (done < len) {
		size_t want = R_MIN (chunk, len - done);
		int n = r2f_read (r, h, buf + done, want, off + done);
		if (n < 0) {
			return n;
		}
		if (n == 0) {
			break;
		}
		done += n;
	}
	return (st64)done;
}

static void check_file(CheckCtx *c, const char *path, const R2FAttr *a) {
	R2FHandle *h = NULL;
	int err = r2f_open (c->r, path, &h);
	if (err) {
		fail (c, path, "open failed");
		return;
	}
	if (h->size != a->size) {
		char *msg = r_str_newf ("size mismatch: getattr %"PFMT64u" vs open %"PFMT64u, a->size, h->size);
		fail (c, path, msg);
		free (msg);
	}
	size_t size = (size_t)h->size;
	ut8 *whole = malloc (size + 1);
	if (!whole) {
		fail (c, path, "cannot allocate");
		r2f_close (c->r, h);
		return;
	}
	st64 n = read_range (c->r, h, whole, 0, size, size? size: 1);
	if (n < 0) {
		fail (c, path, "read failed");
	} else if ((size_t)n != size) {
		char *msg = r_str_newf ("short read: %"PFMT64d" of %"PFMT64u" bytes", n, (ut64)size);
		fail (c, path, msg);
		free (msg);
	}
	printf ("f %10"PFMT64u" %016"PFMT64x" %s\n", (ut64)size, fnv1a (whole, n > 0? n: 0), path);
	if (c->stress && n > 0) {
		ut8 *tmp = calloc (1, size + 1);
		st64 m = read_range (c->r, h, tmp, 0, size, c->chunk);
		if (m != n || memcmp (tmp, whole, n)) {
			fail (c, path, "chunked read differs from whole read");
		}
		// unaligned reads around the middle and the tail of the file
		ut64 offs[3] = { 1, size / 2 + 3, size > 7? size - 7: 0 };
		int i;
		for (i = 0; i < 3; i++) {
			if (offs[i] >= (ut64)n) {
				continue;
			}
			size_t len = R_MIN ((size_t)(n - offs[i]), c->chunk + 13);
			memset (tmp, 0, len);
			m = read_range (c->r, h, tmp, offs[i], len, len);
			if (m != (st64)len || memcmp (tmp, whole + offs[i], len)) {
				char *msg = r_str_newf ("unaligned read at %"PFMT64u" differs", offs[i]);
				fail (c, path, msg);
				free (msg);
			}
		}
		// reading past the end must return 0 bytes
		if (r2f_read (c->r, h, tmp, 16, h->size + 16) != 0) {
			fail (c, path, "read past EOF returned data");
		}
		free (tmp);
	}
	free (whole);
	r2f_close (c->r, h);
}

static bool check_entry(void *user, const char *name, const R2FAttr *a) {
	CheckCtx *c = user;
	char *path = r_str_newf ("%s/%s", strcmp (c->dir, "/")? c->dir: "", name);
	R2FAttr b;
	if (r2f_getattr (c->r, path, &b) || b.kind != a->kind) {
		fail (c, path, "getattr disagrees with readdir");
	}
	if (a->kind == R2F_KIND_DIR) {
		r_list_append (c->subdirs, path);
		return true;
	}
	if (a->kind == R2F_KIND_SPECIAL) {
		c->specials++;
		printf ("s %10d %016d %s (type %c)\n", 0, 0, path, a->type);
	} else {
		c->files++;
		check_file (c, path, a);
	}
	free (path);
	return true;
}

static void check_dir(CheckCtx *c, const char *dir, int depth) {
	if (depth > 64) {
		fail (c, dir, "directory nesting too deep, possible cycle");
		return;
	}
	c->dirs++;
	printf ("d %10d %016d %s\n", 0, 0, dir);
	RList *subdirs = r_list_newf (free);
	RList *saved = c->subdirs;
	const char *saved_dir = c->dir;
	c->subdirs = subdirs;
	c->dir = dir;
	if (r2f_readdir (c->r, dir, check_entry, c)) {
		fail (c, dir, "readdir failed");
	}
	c->subdirs = saved;
	c->dir = saved_dir;
	RListIter *iter;
	const char *sub;
	r_list_foreach (subdirs, iter, sub) {
		check_dir (c, sub, depth + 1);
	}
	r_list_free (subdirs);
}

int r2f_check(R2F *r, const char *path, bool stress, size_t chunk) {
	CheckCtx c = { .r = r, .stress = stress, .chunk = chunk? chunk: 4096 };
	R2FAttr a;
	if (r2f_getattr (r, path, &a)) {
		printf ("FAIL %s: not found\n", path);
		return 1;
	}
	if (a.kind == R2F_KIND_DIR) {
		check_dir (&c, path, 0);
	} else {
		c.files++;
		check_file (&c, path, &a);
	}
	printf ("# %s: %d dirs, %d files, %d specials, %d errors\n", r->fstype, c.dirs, c.files, c.specials, c.errors);
	return c.errors? 1: 0;
}
