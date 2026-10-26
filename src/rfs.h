/* r2fuse - GPL-3.0-only - Copyright 2026 - pancake */

#ifndef R2FUSE_RFS_H
#define R2FUSE_RFS_H

#include <r_fs.h>
#include <r_io.h>
#include <sdb/ht_su.h>

#define R2FUSE_VERSION "0.1.0"

typedef enum {
	R2F_KIND_FILE = 0,
	R2F_KIND_DIR = 1,
	R2F_KIND_SPECIAL = 2, // symlinks and devices, r_fs has no api to resolve them
} R2FKind;

typedef struct {
	R2FKind kind;
	char type;      // raw RFSFile.type as reported by the plugin
	ut64 size;
	ut64 mtime;
	ut32 perm;      // plugin permission bits, 0 when unknown
	ut32 uid;
	ut32 gid;
} R2FAttr;

typedef struct r2f_node_t R2FNode;

typedef struct {
	const char *image;   // file path or any r2 io uri
	const char *fstype;  // NULL to autodetect
	const char *options; // r_fs mount options
	ut64 offset;         // filesystem offset inside the image
	int view;            // R_FS_VIEW_*
} R2FOptions;

typedef struct {
	RIO *io;
	RFS *fs;
	RFSRoot *root;
	char *fstype;
	ut64 image_size;
	HtSU *nodes;         // path -> R2FNode*
	RList *allnodes;     // owns every R2FNode
	RThreadLock *lock;   // r_fs and most plugins are not thread safe
} R2F;

typedef struct {
	RFSFile *file;       // NULL when the content was slurped
	ut8 *data;           // slurped content (fallback for plugins without read)
	ut64 size;
} R2FHandle;

typedef bool (*R2FDirCallback)(void *user, const char *name, const R2FAttr *attr);

R2F *r2f_new(const R2FOptions *opt);
void r2f_free(R2F *r);
// partition helpers: list partitions of the given type and return them
RList *r2f_partitions(const char *image, const char *ptype);
void r2f_list_plugins(void);

// all these return 0 on success or a negative errno value
int r2f_getattr(R2F *r, const char *path, R2FAttr *attr);
int r2f_readdir(R2F *r, const char *path, R2FDirCallback cb, void *user);
int r2f_open(R2F *r, const char *path, R2FHandle **h);
int r2f_read(R2F *r, R2FHandle *h, ut8 *buf, size_t len, ut64 off);
void r2f_close(R2F *r, R2FHandle *h);

// walk the whole tree using the same code paths as the fuse callbacks
int r2f_check(R2F *r, const char *path, bool stress, size_t chunk);

#endif
