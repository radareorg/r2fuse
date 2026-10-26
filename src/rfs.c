/* r2fuse - GPL-3.0-only - Copyright 2026 - pancake */

#define R_LOG_ORIGIN "r2fuse"

#include "rfs.h"
#include <errno.h>

struct r2f_node_t {
	char *path;
	R2FAttr attr;
	bool size_known;
	bool listed;
	RList *children; // R2FNode* (not owned), valid once listed
};

static bool type_is_dir(char t) {
	switch (t) {
	case R_FS_FILE_TYPE_DIRECTORY:
	case R_FS_FILE_TYPE_MOUNTPOINT:
	case R_FS_FILE_TYPE_S_DIRECTORY:
	case R_FS_FILE_TYPE_S_MOUNTPOINT:
		return true;
	}
	return false;
}

static R2FKind type_kind(char t) {
	if (type_is_dir (t)) {
		return R2F_KIND_DIR;
	}
	switch (t) {
	case 'l': // ubifs symlinks, not defined in r_fs.h
	case R_FS_FILE_TYPE_SPECIAL:
	case R_FS_FILE_TYPE_S_SPECIAL:
	case R_FS_FILE_TYPE_BLOCK:
	case R_FS_FILE_TYPE_S_BLOCK:
	case R_FS_FILE_TYPE_CHAR:
	case R_FS_FILE_TYPE_S_CHAR:
		return R2F_KIND_SPECIAL;
	}
	return R2F_KIND_FILE;
}

static void node_free(R2FNode *n) {
	if (n) {
		r_list_free (n->children);
		free (n->path);
		free (n);
	}
}

static R2FNode *node_add(R2F *r, const char *path, const RFSFile *f) {
	R2FNode *n = R_NEW0 (R2FNode);
	n->path = strdup (path);
	if (f) {
		n->attr.type = f->type;
		n->attr.kind = type_kind (f->type);
		n->attr.size = f->size;
		n->attr.mtime = f->time;
		n->attr.perm = f->perm;
		n->attr.uid = f->uid;
		n->attr.gid = f->gid;
		// plugins listing directories rarely fill the size, trust only non-zero values
		n->size_known = n->attr.kind != R2F_KIND_FILE || f->size > 0;
	} else {
		n->attr.type = R_FS_FILE_TYPE_DIRECTORY;
		n->attr.kind = R2F_KIND_DIR;
		n->size_known = true;
	}
	r_list_append (r->allnodes, n);
	ht_su_insert (r->nodes, n->path, (ut64)(size_t)n);
	return n;
}

static R2FNode *node_get(R2F *r, const char *path) {
	bool found = false;
	ut64 v = ht_su_find (r->nodes, path, &found);
	return found? (R2FNode *)(size_t)v: NULL;
}

static char *child_path(const char *dir, const char *name) {
	return r_str_newf ("%s/%s", strcmp (dir, "/")? dir: "", name);
}

static bool valid_name(const char *name) {
	return R_STR_ISNOTEMPTY (name) && strcmp (name, ".") && strcmp (name, "..") && !strchr (name, '/');
}

static void node_list(R2F *r, R2FNode *n) {
	if (n->listed) {
		return;
	}
	n->listed = true;
	n->children = r_list_new ();
	RList *list = r_fs_dir (r->fs, n->path);
	RListIter *iter;
	RFSFile *f;
	r_list_foreach (list, iter, f) {
		if (!valid_name (f->name)) {
			if (f->name && strcmp (f->name, ".") && strcmp (f->name, "..")) {
				R_LOG_WARN ("Skipping invalid entry name '%s' in %s", f->name, n->path);
			}
			continue;
		}
		char *cp = child_path (n->path, f->name);
		if (node_get (r, cp)) {
			R_LOG_DEBUG ("Duplicated entry %s", cp);
		} else {
			r_list_append (n->children, node_add (r, cp, f));
		}
		free (cp);
	}
	r_list_free (list);
}

static R2FNode *node_lookup(R2F *r, const char *path) {
	R2FNode *n = node_get (r, path);
	if (n || !strcmp (path, "/") || *path != '/') {
		return n;
	}
	char *parent = strdup (path);
	char *slash = (char *)r_str_rchr (parent, NULL, '/');
	if (slash == parent) {
		slash[1] = 0;
	} else {
		*slash = 0;
	}
	R2FNode *pn = node_lookup (r, parent);
	free (parent);
	if (pn && pn->attr.kind == R2F_KIND_DIR) {
		node_list (r, pn);
		n = node_get (r, path);
	}
	return n;
}

static void node_resolve_size(R2F *r, R2FNode *n) {
	if (n->size_known) {
		return;
	}
	n->size_known = true;
	RFSFile *f = r_fs_open (r->fs, n->path, false);
	if (f) {
		n->attr.size = f->size;
		if (!n->attr.mtime) {
			n->attr.mtime = f->time;
		}
		r_fs_close (r->fs, f);
		r_fs_file_free (f);
	}
}

static void load_plugins(R2F *r) {
	r_libstore_load (r->io->libstore);
	r_libstore_load (r->fs->libstore);
}

R2F *r2f_new(const R2FOptions *opt) {
	R2F *r = R_NEW0 (R2F);
	r->io = r_io_new ();
	r->fs = r_fs_new ();
	r->lock = r_th_lock_new (true);
	r->nodes = ht_su_new0 ();
	r->allnodes = r_list_newf ((RListFree)node_free);
	load_plugins (r);
	if (!r_io_open (r->io, opt->image, R_PERM_R, 0)) {
		R_LOG_ERROR ("Cannot open %s", opt->image);
		r2f_free (r);
		return NULL;
	}
	r->image_size = r_io_size (r->io);
	r_io_bind (r->io, &r->fs->iob);
	r_fs_view (r->fs, opt->view);
	r->fstype = R_STR_ISNOTEMPTY (opt->fstype)? strdup (opt->fstype): r_fs_name (r->fs, opt->offset);
	if (!r->fstype) {
		R_LOG_ERROR ("Cannot detect the filesystem type at 0x%"PFMT64x", use -t", opt->offset);
		r2f_free (r);
		return NULL;
	}
	r->root = r_fs_mount_with_options (r->fs, r->fstype, "/", opt->offset, opt->options);
	if (!r->root) {
		R_LOG_ERROR ("Cannot mount %s at offset 0x%"PFMT64x, r->fstype, opt->offset);
		r2f_free (r);
		return NULL;
	}
	node_add (r, "/", NULL);
	return r;
}

void r2f_free(R2F *r) {
	if (r) {
		r_list_free (r->allnodes);
		ht_su_free (r->nodes);
		r_fs_free (r->fs);
		r_io_free (r->io);
		r_th_lock_free (r->lock);
		free (r->fstype);
		free (r);
	}
}

RList *r2f_partitions(const char *image, const char *ptype) {
	RIO *io = r_io_new ();
	RFS *fs = r_fs_new ();
	r_libstore_load (io->libstore);
	RList *list = NULL;
	if (r_io_open (io, image, R_PERM_R, 0)) {
		r_io_bind (io, &fs->iob);
		list = r_fs_partitions (fs, ptype, 0);
		if (list) {
			// detach the partitions from the RFS lifetime
			list->free = (RListFree)r_fs_partition_free;
		}
	} else {
		R_LOG_ERROR ("Cannot open %s", image);
	}
	r_fs_free (fs);
	r_io_free (io);
	return list;
}

void r2f_list_plugins(void) {
	RFS *fs = r_fs_new ();
	r_libstore_load (fs->libstore);
	RListIter *iter;
	RFSPlugin *p;
	r_list_foreach (fs->libstore->plugins, iter, p) {
		printf ("%-12s %s\n", p->meta.name, r_str_get (p->meta.desc));
	}
	r_fs_free (fs);
}

int r2f_getattr(R2F *r, const char *path, R2FAttr *attr) {
	r_th_lock_enter (r->lock);
	R2FNode *n = node_lookup (r, path);
	if (n) {
		if (n->attr.kind == R2F_KIND_FILE) {
			node_resolve_size (r, n);
		}
		*attr = n->attr;
	}
	r_th_lock_leave (r->lock);
	return n? 0: -ENOENT;
}

int r2f_readdir(R2F *r, const char *path, R2FDirCallback cb, void *user) {
	int ret = 0;
	r_th_lock_enter (r->lock);
	R2FNode *n = node_lookup (r, path);
	if (!n) {
		ret = -ENOENT;
	} else if (n->attr.kind != R2F_KIND_DIR) {
		ret = -ENOTDIR;
	} else {
		node_list (r, n);
		RListIter *iter;
		R2FNode *c;
		r_list_foreach (n->children, iter, c) {
			// names are always the last component of the node path
			const char *name = r_str_rchr (c->path, NULL, '/') + 1;
			if (c->attr.kind == R2F_KIND_FILE) {
				node_resolve_size (r, c);
			}
			if (!cb (user, name, &c->attr)) {
				break;
			}
		}
	}
	r_th_lock_leave (r->lock);
	return ret;
}

int r2f_open(R2F *r, const char *path, R2FHandle **h) {
	int ret = 0;
	*h = NULL;
	r_th_lock_enter (r->lock);
	R2FNode *n = node_lookup (r, path);
	if (!n) {
		ret = -ENOENT;
	} else if (n->attr.kind == R2F_KIND_DIR) {
		ret = -EISDIR;
	} else if (n->attr.kind == R2F_KIND_SPECIAL) {
		ret = -ENXIO;
	} else {
		RFSFile *f = r_fs_open (r->fs, path, false);
		if (f) {
			R2FHandle *fh = R_NEW0 (R2FHandle);
			fh->file = f;
			// keep the size from open time, some plugins update it on every read
			fh->size = f->size;
			n->attr.size = f->size;
			n->size_known = true;
			*h = fh;
		} else if (!r->root->p->open && r->root->p->slurp) {
			// plugins without open/read may still implement slurp
			f = r_fs_slurp (r->fs, path);
			if (f && f->data) {
				R2FHandle *fh = R_NEW0 (R2FHandle);
				fh->size = f->size;
				fh->data = f->data;
				f->data = NULL;
				*h = fh;
			} else {
				ret = -EIO;
			}
			if (f) {
				r_fs_close (r->fs, f);
				r_fs_file_free (f);
			}
		} else {
			ret = -EIO;
		}
	}
	r_th_lock_leave (r->lock);
	return ret;
}

int r2f_read(R2F *r, R2FHandle *h, ut8 *buf, size_t len, ut64 off) {
	if (off >= h->size || !len) {
		return 0;
	}
	if (len > h->size - off) {
		len = (size_t)(h->size - off);
	}
	if (len > INT_MAX) {
		len = INT_MAX;
	}
	if (!h->file) {
		memcpy (buf, h->data + off, len);
		return (int)len;
	}
	int done = 0;
	r_th_lock_enter (r->lock);
	// fuse treats short reads as EOF, but plugins like bfs stop at extent
	// boundaries, so keep reading until the request is satisfied
	while ((size_t)done < len) {
		int want = (int)(len - done);
		// r_fs_read only allocates file->data when it is NULL, and some plugins
		// write len bytes into it, so it must be sized for every request
		R_FREE (h->file->data);
		int n = r_fs_read (r->fs, h->file, off + done, want);
		if (n < 0) {
			if (!done) {
				done = -EIO;
			}
			break;
		}
		if (n == 0 || !h->file->data) {
			break;
		}
		if (n > want) {
			R_LOG_WARN ("Plugin returned %d bytes for a %d bytes read", n, want);
			n = want;
		}
		memcpy (buf + done, h->file->data, n);
		done += n;
	}
	r_th_lock_leave (r->lock);
	return done;
}

void r2f_close(R2F *r, R2FHandle *h) {
	if (h) {
		r_th_lock_enter (r->lock);
		if (h->file) {
			r_fs_close (r->fs, h->file);
			r_fs_file_free (h->file);
		}
		r_th_lock_leave (r->lock);
		free (h->data);
		free (h);
	}
}
