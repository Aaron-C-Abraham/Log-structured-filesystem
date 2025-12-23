/*
 * LSFS - Log-Structured Filesystem
 * FUSE Operations Implementation
 */

#define FUSE_USE_VERSION 35

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fuse3/fuse_lowlevel.h>

#include "lsfs.h"

/* Global context pointer */
struct lsfs_context *g_lsfs = NULL;

/*
 * Helper: Get inode and validate
 */
static struct lsfs_inode_mem *get_inode(fuse_ino_t ino)
{
    if (ino == FUSE_ROOT_ID) {
        ino = LSFS_ROOT_INO;
    }
    return lsfs_inode_get(g_lsfs, (uint32_t)ino);
}

/*
 * FUSE init
 */
static void lsfs_op_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    (void)conn;

    LSFS_INFO("FUSE filesystem initialized");
}

/*
 * FUSE destroy
 */
static void lsfs_op_destroy(void *userdata)
{
    (void)userdata;
    LSFS_INFO("FUSE filesystem destroyed");
}

/*
 * FUSE lookup
 */
static void lsfs_op_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct lsfs_inode_mem *parent_inode;
    struct lsfs_inode_mem *child_inode;
    struct fuse_entry_param e;
    uint32_t child_ino;
    uint8_t file_type;

    parent_inode = get_inode(parent);
    if (!parent_inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (lsfs_dir_lookup(g_lsfs, parent_inode, name, &child_ino, &file_type) != LSFS_OK) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOENT);
        return;
    }

    lsfs_inode_put(parent_inode);

    child_inode = lsfs_inode_get(g_lsfs, child_ino);
    if (!child_inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    memset(&e, 0, sizeof(e));
    e.ino = child_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    lsfs_inode_to_stat(child_inode, &e.attr);
    e.generation = child_inode->disk_inode.generation;

    lsfs_inode_put(child_inode);
    fuse_reply_entry(req, &e);
}

/*
 * FUSE getattr
 */
static void lsfs_op_getattr(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi)
{
    struct lsfs_inode_mem *inode;
    struct stat st;
    (void)fi;

    inode = get_inode(ino);
    if (!inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    lsfs_inode_to_stat(inode, &st);
    lsfs_inode_put(inode);

    fuse_reply_attr(req, &st, 1.0);
}

/*
 * FUSE setattr
 */
static void lsfs_op_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                            int to_set, struct fuse_file_info *fi)
{
    struct lsfs_inode_mem *inode;
    struct stat st;
    (void)fi;

    inode = get_inode(ino);
    if (!inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_mutex_lock(&inode->lock);

    if (to_set & FUSE_SET_ATTR_MODE) {
        inode->disk_inode.mode = (inode->disk_inode.mode & S_IFMT) | (attr->st_mode & ~S_IFMT);
        inode->dirty = true;
    }

    if (to_set & FUSE_SET_ATTR_UID) {
        inode->disk_inode.uid = attr->st_uid;
        inode->dirty = true;
    }

    if (to_set & FUSE_SET_ATTR_GID) {
        inode->disk_inode.gid = attr->st_gid;
        inode->dirty = true;
    }

    if (to_set & FUSE_SET_ATTR_SIZE) {
        /* Truncate */
        if ((uint64_t)attr->st_size < inode->disk_inode.size) {
            /* Mark blocks beyond new size as dead */
            uint64_t old_blocks = LSFS_BLOCKS_FOR_SIZE(inode->disk_inode.size);
            uint64_t new_blocks = LSFS_BLOCKS_FOR_SIZE(attr->st_size);

            for (uint64_t i = new_blocks; i < old_blocks && i < LSFS_DIRECT_BLOCKS; i++) {
                if (inode->disk_inode.direct[i]) {
                    lsfs_gc_mark_block_dead(g_lsfs, inode->disk_inode.direct[i]);
                    inode->disk_inode.direct[i] = 0;
                }
            }
        }
        inode->disk_inode.size = attr->st_size;
        inode->dirty = true;
    }

    if (to_set & FUSE_SET_ATTR_ATIME) {
        inode->disk_inode.atime = (uint64_t)attr->st_atim.tv_sec * 1000000000ULL +
                                  (uint64_t)attr->st_atim.tv_nsec;
        inode->dirty = true;
    }

    if (to_set & FUSE_SET_ATTR_MTIME) {
        inode->disk_inode.mtime = (uint64_t)attr->st_mtim.tv_sec * 1000000000ULL +
                                  (uint64_t)attr->st_mtim.tv_nsec;
        inode->dirty = true;
    }

    if (inode->dirty) {
        inode->disk_inode.ctime = lsfs_get_time_ns();
        lsfs_inode_write(g_lsfs, inode);
    }

    pthread_mutex_unlock(&inode->lock);

    lsfs_inode_to_stat(inode, &st);
    lsfs_inode_put(inode);

    fuse_reply_attr(req, &st, 1.0);
}

/*
 * FUSE readdir
 */
struct readdir_ctx {
    fuse_req_t req;
    char *buf;
    size_t size;
    size_t offset;
    off_t start_offset;
    int plus;
};

static int readdir_callback(void *ctx, const char *name, uint32_t ino,
                            uint8_t type, off_t offset)
{
    struct readdir_ctx *rctx = (struct readdir_ctx *)ctx;
    struct stat st;
    size_t entsize;

    (void)offset;

    memset(&st, 0, sizeof(st));
    st.st_ino = ino;

    switch (type) {
    case LSFS_FT_DIR:     st.st_mode = S_IFDIR; break;
    case LSFS_FT_REG_FILE: st.st_mode = S_IFREG; break;
    case LSFS_FT_SYMLINK: st.st_mode = S_IFLNK; break;
    default:              st.st_mode = 0; break;
    }

    entsize = fuse_add_direntry(rctx->req, rctx->buf + rctx->offset,
                                rctx->size - rctx->offset, name, &st, offset + 1);

    if (entsize > rctx->size - rctx->offset) {
        return 1;  /* Buffer full */
    }

    rctx->offset += entsize;
    return 0;
}

static void lsfs_op_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                            off_t off, struct fuse_file_info *fi)
{
    struct lsfs_inode_mem *inode;
    struct readdir_ctx ctx;
    (void)fi;

    inode = get_inode(ino);
    if (!inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    ctx.req = req;
    ctx.buf = malloc(size);
    if (!ctx.buf) {
        lsfs_inode_put(inode);
        fuse_reply_err(req, ENOMEM);
        return;
    }
    ctx.size = size;
    ctx.offset = 0;
    ctx.start_offset = off;
    ctx.plus = 0;

    lsfs_dir_iterate(g_lsfs, inode, readdir_callback, &ctx, off);

    lsfs_inode_put(inode);

    fuse_reply_buf(req, ctx.buf, ctx.offset);
    free(ctx.buf);
}

/*
 * FUSE open
 */
static void lsfs_op_open(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
    struct lsfs_inode_mem *inode;

    inode = get_inode(ino);
    if (!inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    lsfs_inode_put(inode);
    fuse_reply_open(req, fi);
}

/*
 * FUSE read
 */
static void lsfs_op_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                         off_t off, struct fuse_file_info *fi)
{
    struct lsfs_inode_mem *inode;
    char *buf;
    size_t bytes_read = 0;
    (void)fi;

    inode = get_inode(ino);
    if (!inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if ((uint64_t)off >= inode->disk_inode.size) {
        lsfs_inode_put(inode);
        fuse_reply_buf(req, NULL, 0);
        return;
    }

    if ((uint64_t)off + size > inode->disk_inode.size) {
        size = inode->disk_inode.size - off;
    }

    buf = malloc(size);
    if (!buf) {
        lsfs_inode_put(inode);
        fuse_reply_err(req, ENOMEM);
        return;
    }

    uint8_t block_buf[LSFS_BLOCK_SIZE];

    while (bytes_read < size) {
        uint64_t block_idx = (off + bytes_read) / LSFS_BLOCK_SIZE;
        uint32_t block_off = (off + bytes_read) % LSFS_BLOCK_SIZE;
        size_t to_read = LSFS_MIN(LSFS_BLOCK_SIZE - block_off, size - bytes_read);

        if (lsfs_inode_read_block(g_lsfs, inode, block_idx, block_buf) != LSFS_OK) {
            break;
        }

        memcpy(buf + bytes_read, block_buf + block_off, to_read);
        bytes_read += to_read;
    }

    /* Update atime */
    inode->disk_inode.atime = lsfs_get_time_ns();

    lsfs_inode_put(inode);
    fuse_reply_buf(req, buf, bytes_read);
    free(buf);
}

/*
 * FUSE write
 */
static void lsfs_op_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                          size_t size, off_t off, struct fuse_file_info *fi)
{
    struct lsfs_inode_mem *inode;
    size_t bytes_written = 0;
    (void)fi;

    inode = get_inode(ino);
    if (!inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_mutex_lock(&inode->lock);

    uint8_t block_buf[LSFS_BLOCK_SIZE];

    while (bytes_written < size) {
        uint64_t block_idx = (off + bytes_written) / LSFS_BLOCK_SIZE;
        uint32_t block_off = (off + bytes_written) % LSFS_BLOCK_SIZE;
        size_t to_write = LSFS_MIN(LSFS_BLOCK_SIZE - block_off, size - bytes_written);

        /* If partial block write, read existing block first */
        if (to_write < LSFS_BLOCK_SIZE) {
            if (lsfs_inode_read_block(g_lsfs, inode, block_idx, block_buf) != LSFS_OK) {
                memset(block_buf, 0, LSFS_BLOCK_SIZE);
            }
        }

        memcpy(block_buf + block_off, buf + bytes_written, to_write);

        if (lsfs_inode_write_block(g_lsfs, inode, block_idx, block_buf) != LSFS_OK) {
            break;
        }

        bytes_written += to_write;
    }

    /* Update size and times */
    if ((uint64_t)(off + bytes_written) > inode->disk_inode.size) {
        inode->disk_inode.size = off + bytes_written;
    }
    inode->disk_inode.mtime = lsfs_get_time_ns();
    inode->disk_inode.ctime = inode->disk_inode.mtime;
    inode->dirty = true;

    lsfs_inode_write(g_lsfs, inode);

    pthread_mutex_unlock(&inode->lock);
    lsfs_inode_put(inode);

    fuse_reply_write(req, bytes_written);
}

/*
 * FUSE create
 */
static void lsfs_op_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                           mode_t mode, struct fuse_file_info *fi)
{
    struct lsfs_inode_mem *parent_inode;
    struct lsfs_inode_mem *new_inode;
    struct fuse_entry_param e;

    parent_inode = get_inode(parent);
    if (!parent_inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* Check if file already exists */
    uint32_t existing_ino;
    if (lsfs_dir_lookup(g_lsfs, parent_inode, name, &existing_ino, NULL) == LSFS_OK) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* Create new inode */
    new_inode = lsfs_inode_alloc(g_lsfs, S_IFREG | (mode & 0777));
    if (!new_inode) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOSPC);
        return;
    }

    /* Add to parent directory */
    if (lsfs_dir_add(g_lsfs, parent_inode, name, new_inode->disk_inode.ino,
                     LSFS_FT_REG_FILE) != LSFS_OK) {
        lsfs_inode_free(g_lsfs, new_inode);
        lsfs_inode_put(new_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Write inode and parent */
    lsfs_inode_write(g_lsfs, new_inode);
    lsfs_inode_write(g_lsfs, parent_inode);

    memset(&e, 0, sizeof(e));
    e.ino = new_inode->disk_inode.ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    lsfs_inode_to_stat(new_inode, &e.attr);
    e.generation = new_inode->disk_inode.generation;

    lsfs_inode_put(new_inode);
    lsfs_inode_put(parent_inode);

    fuse_reply_create(req, &e, fi);
}

/*
 * FUSE mkdir
 */
static void lsfs_op_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                          mode_t mode)
{
    struct lsfs_inode_mem *parent_inode;
    struct lsfs_inode_mem *new_inode;
    struct fuse_entry_param e;

    parent_inode = get_inode(parent);
    if (!parent_inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* Check if already exists */
    uint32_t existing_ino;
    if (lsfs_dir_lookup(g_lsfs, parent_inode, name, &existing_ino, NULL) == LSFS_OK) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* Create new directory inode */
    new_inode = lsfs_inode_alloc(g_lsfs, S_IFDIR | (mode & 0777));
    if (!new_inode) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOSPC);
        return;
    }

    /* Initialize directory with . and .. */
    uint32_t parent_ino = (parent == FUSE_ROOT_ID) ? LSFS_ROOT_INO : (uint32_t)parent;
    if (lsfs_dir_init(g_lsfs, new_inode, parent_ino) != LSFS_OK) {
        lsfs_inode_free(g_lsfs, new_inode);
        lsfs_inode_put(new_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Add to parent directory */
    if (lsfs_dir_add(g_lsfs, parent_inode, name, new_inode->disk_inode.ino,
                     LSFS_FT_DIR) != LSFS_OK) {
        lsfs_inode_free(g_lsfs, new_inode);
        lsfs_inode_put(new_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Increment parent link count for .. */
    parent_inode->disk_inode.nlink++;
    parent_inode->dirty = true;

    /* Write inodes */
    lsfs_inode_write(g_lsfs, new_inode);
    lsfs_inode_write(g_lsfs, parent_inode);

    memset(&e, 0, sizeof(e));
    e.ino = new_inode->disk_inode.ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    lsfs_inode_to_stat(new_inode, &e.attr);
    e.generation = new_inode->disk_inode.generation;

    lsfs_inode_put(new_inode);
    lsfs_inode_put(parent_inode);

    fuse_reply_entry(req, &e);
}

/*
 * FUSE unlink
 */
static void lsfs_op_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct lsfs_inode_mem *parent_inode;
    struct lsfs_inode_mem *file_inode;
    uint32_t file_ino;

    parent_inode = get_inode(parent);
    if (!parent_inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (lsfs_dir_lookup(g_lsfs, parent_inode, name, &file_ino, NULL) != LSFS_OK) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOENT);
        return;
    }

    file_inode = lsfs_inode_get(g_lsfs, file_ino);
    if (!file_inode) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* Can't unlink directories */
    if (S_ISDIR(file_inode->disk_inode.mode)) {
        lsfs_inode_put(file_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EISDIR);
        return;
    }

    /* Remove from directory */
    if (lsfs_dir_remove(g_lsfs, parent_inode, name) != LSFS_OK) {
        lsfs_inode_put(file_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Decrement link count */
    file_inode->disk_inode.nlink--;
    file_inode->disk_inode.ctime = lsfs_get_time_ns();

    if (file_inode->disk_inode.nlink == 0) {
        lsfs_inode_free(g_lsfs, file_inode);
    } else {
        file_inode->dirty = true;
        lsfs_inode_write(g_lsfs, file_inode);
    }

    lsfs_inode_write(g_lsfs, parent_inode);

    lsfs_inode_put(file_inode);
    lsfs_inode_put(parent_inode);

    fuse_reply_err(req, 0);
}

/*
 * FUSE rmdir
 */
static void lsfs_op_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct lsfs_inode_mem *parent_inode;
    struct lsfs_inode_mem *dir_inode;
    uint32_t dir_ino;

    parent_inode = get_inode(parent);
    if (!parent_inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (lsfs_dir_lookup(g_lsfs, parent_inode, name, &dir_ino, NULL) != LSFS_OK) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOENT);
        return;
    }

    dir_inode = lsfs_inode_get(g_lsfs, dir_ino);
    if (!dir_inode) {
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (!S_ISDIR(dir_inode->disk_inode.mode)) {
        lsfs_inode_put(dir_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    if (lsfs_dir_is_empty(g_lsfs, dir_inode) != LSFS_OK) {
        lsfs_inode_put(dir_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, ENOTEMPTY);
        return;
    }

    /* Remove from parent */
    if (lsfs_dir_remove(g_lsfs, parent_inode, name) != LSFS_OK) {
        lsfs_inode_put(dir_inode);
        lsfs_inode_put(parent_inode);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Decrement parent link count for .. */
    parent_inode->disk_inode.nlink--;
    parent_inode->dirty = true;

    /* Free the directory inode */
    lsfs_inode_free(g_lsfs, dir_inode);

    lsfs_inode_write(g_lsfs, parent_inode);

    lsfs_inode_put(dir_inode);
    lsfs_inode_put(parent_inode);

    fuse_reply_err(req, 0);
}

/*
 * FUSE rename
 */
static void lsfs_op_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                           fuse_ino_t newparent, const char *newname,
                           unsigned int flags)
{
    struct lsfs_inode_mem *old_parent_inode;
    struct lsfs_inode_mem *new_parent_inode = NULL;
    struct lsfs_inode_mem *target_inode;
    uint32_t target_ino;
    uint8_t file_type;
    (void)flags;

    old_parent_inode = get_inode(parent);
    if (!old_parent_inode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (parent != newparent) {
        new_parent_inode = get_inode(newparent);
        if (!new_parent_inode) {
            lsfs_inode_put(old_parent_inode);
            fuse_reply_err(req, ENOENT);
            return;
        }
    } else {
        new_parent_inode = old_parent_inode;
    }

    /* Lookup source */
    if (lsfs_dir_lookup(g_lsfs, old_parent_inode, name, &target_ino, &file_type) != LSFS_OK) {
        if (parent != newparent) {
            lsfs_inode_put(new_parent_inode);
        }
        lsfs_inode_put(old_parent_inode);
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* Check if destination exists */
    uint32_t dest_ino;
    uint8_t dest_type;
    if (lsfs_dir_lookup(g_lsfs, new_parent_inode, newname, &dest_ino, &dest_type) == LSFS_OK) {
        /* Remove existing destination */
        struct lsfs_inode_mem *dest_inode = lsfs_inode_get(g_lsfs, dest_ino);
        if (dest_inode) {
            if (S_ISDIR(dest_inode->disk_inode.mode)) {
                if (lsfs_dir_is_empty(g_lsfs, dest_inode) != LSFS_OK) {
                    lsfs_inode_put(dest_inode);
                    if (parent != newparent) {
                        lsfs_inode_put(new_parent_inode);
                    }
                    lsfs_inode_put(old_parent_inode);
                    fuse_reply_err(req, ENOTEMPTY);
                    return;
                }
            }
            lsfs_dir_remove(g_lsfs, new_parent_inode, newname);
            dest_inode->disk_inode.nlink--;
            if (dest_inode->disk_inode.nlink == 0) {
                lsfs_inode_free(g_lsfs, dest_inode);
            }
            lsfs_inode_put(dest_inode);
        }
    }

    /* Add new entry */
    if (lsfs_dir_add(g_lsfs, new_parent_inode, newname, target_ino, file_type) != LSFS_OK) {
        if (parent != newparent) {
            lsfs_inode_put(new_parent_inode);
        }
        lsfs_inode_put(old_parent_inode);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Remove old entry */
    lsfs_dir_remove(g_lsfs, old_parent_inode, name);

    /* Update .. if directory moved to different parent */
    if (parent != newparent && file_type == LSFS_FT_DIR) {
        target_inode = lsfs_inode_get(g_lsfs, target_ino);
        if (target_inode) {
            /* This is simplified - would need to update .. entry in directory */
            old_parent_inode->disk_inode.nlink--;
            new_parent_inode->disk_inode.nlink++;
            lsfs_inode_put(target_inode);
        }
    }

    /* Write changes */
    lsfs_inode_write(g_lsfs, old_parent_inode);
    if (parent != newparent) {
        lsfs_inode_write(g_lsfs, new_parent_inode);
        lsfs_inode_put(new_parent_inode);
    }
    lsfs_inode_put(old_parent_inode);

    fuse_reply_err(req, 0);
}

/*
 * FUSE statfs
 */
static void lsfs_op_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct statvfs st;
    (void)ino;

    memset(&st, 0, sizeof(st));

    st.f_bsize = LSFS_BLOCK_SIZE;
    st.f_frsize = LSFS_BLOCK_SIZE;
    st.f_blocks = g_lsfs->sb.total_blocks;
    st.f_bfree = g_lsfs->sb.free_segments * LSFS_SEGMENT_BLOCKS;
    st.f_bavail = st.f_bfree;
    st.f_files = LSFS_MAX_INODES;
    st.f_ffree = LSFS_MAX_INODES - g_lsfs->sb.inode_count;
    st.f_favail = st.f_ffree;
    st.f_namemax = LSFS_NAME_MAX;

    fuse_reply_statfs(req, &st);
}

/*
 * FUSE fsync
 */
static void lsfs_op_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                          struct fuse_file_info *fi)
{
    (void)ino;
    (void)datasync;
    (void)fi;

    lsfs_segment_flush(g_lsfs);
    lsfs_sync(g_lsfs);

    fuse_reply_err(req, 0);
}

/*
 * FUSE operations structure
 */
const struct fuse_lowlevel_ops lsfs_fuse_ops = {
    .init       = lsfs_op_init,
    .destroy    = lsfs_op_destroy,
    .lookup     = lsfs_op_lookup,
    .getattr    = lsfs_op_getattr,
    .setattr    = lsfs_op_setattr,
    .readdir    = lsfs_op_readdir,
    .open       = lsfs_op_open,
    .read       = lsfs_op_read,
    .write      = lsfs_op_write,
    .create     = lsfs_op_create,
    .mkdir      = lsfs_op_mkdir,
    .unlink     = lsfs_op_unlink,
    .rmdir      = lsfs_op_rmdir,
    .rename     = lsfs_op_rename,
    .statfs     = lsfs_op_statfs,
    .fsync      = lsfs_op_fsync,
};
