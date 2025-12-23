/*
 * LSFS - Log-Structured Filesystem
 * Directory Operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "lsfs.h"

/*
 * Convert file mode to directory entry type
 */
uint8_t lsfs_mode_to_type(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFREG:  return LSFS_FT_REG_FILE;
    case S_IFDIR:  return LSFS_FT_DIR;
    case S_IFCHR:  return LSFS_FT_CHRDEV;
    case S_IFBLK:  return LSFS_FT_BLKDEV;
    case S_IFIFO:  return LSFS_FT_FIFO;
    case S_IFSOCK: return LSFS_FT_SOCK;
    case S_IFLNK:  return LSFS_FT_SYMLINK;
    default:       return LSFS_FT_UNKNOWN;
    }
}

/*
 * Calculate directory entry size (must be 4-byte aligned)
 */
static inline uint16_t dirent_size(uint8_t name_len)
{
    uint16_t size = sizeof(struct lsfs_dirent) + name_len;
    return (size + 3) & ~3;  /* Align to 4 bytes */
}

/*
 * Lookup a name in a directory
 */
int lsfs_dir_lookup(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                    const char *name, uint32_t *ino, uint8_t *file_type)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    size_t name_len = strlen(name);
    uint64_t offset = 0;
    uint64_t dir_size = dir->disk_inode.size;

    if (!(dir->disk_inode.mode & S_IFDIR)) {
        return LSFS_ERR_NOTDIR;
    }

    if (name_len > LSFS_NAME_MAX) {
        return LSFS_ERR_INVAL;
    }

    /* Iterate through directory blocks */
    while (offset < dir_size) {
        uint64_t block_idx = offset / LSFS_BLOCK_SIZE;
        uint32_t block_offset = offset % LSFS_BLOCK_SIZE;

        if (block_offset == 0) {
            if (lsfs_inode_read_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }
        }

        struct lsfs_dirent *de = (struct lsfs_dirent *)(block + block_offset);

        /* Validate entry */
        if (de->rec_len == 0 || de->rec_len > LSFS_BLOCK_SIZE - block_offset) {
            break;  /* End of valid entries */
        }

        /* Check if this entry matches */
        if (de->ino != 0 && de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0) {
            *ino = de->ino;
            if (file_type) {
                *file_type = de->file_type;
            }
            return LSFS_OK;
        }

        offset += de->rec_len;
    }

    return LSFS_ERR_NOENT;
}

/*
 * Add an entry to a directory
 */
int lsfs_dir_add(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                 const char *name, uint32_t ino, uint8_t file_type)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    size_t name_len = strlen(name);
    uint16_t needed_size = dirent_size((uint8_t)name_len);
    uint64_t offset = 0;
    uint64_t dir_size = dir->disk_inode.size;

    if (!(dir->disk_inode.mode & S_IFDIR)) {
        return LSFS_ERR_NOTDIR;
    }

    if (name_len > LSFS_NAME_MAX) {
        return LSFS_ERR_INVAL;
    }

    /* Check if name already exists */
    uint32_t existing_ino;
    if (lsfs_dir_lookup(ctx, dir, name, &existing_ino, NULL) == LSFS_OK) {
        return LSFS_ERR_EXIST;
    }

    /* Find space for new entry */
    while (offset < dir_size) {
        uint64_t block_idx = offset / LSFS_BLOCK_SIZE;
        uint32_t block_offset = offset % LSFS_BLOCK_SIZE;

        if (block_offset == 0) {
            if (lsfs_inode_read_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }
        }

        struct lsfs_dirent *de = (struct lsfs_dirent *)(block + block_offset);

        if (de->rec_len == 0) {
            /* Empty slot, use rest of block */
            uint16_t space = LSFS_BLOCK_SIZE - block_offset;
            if (space >= needed_size) {
                de->ino = ino;
                de->rec_len = space;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);

                if (lsfs_inode_write_block(ctx, dir, block_idx, block) != LSFS_OK) {
                    return LSFS_ERR_IO;
                }

                dir->disk_inode.mtime = lsfs_get_time_ns();
                dir->disk_inode.ctime = dir->disk_inode.mtime;
                dir->dirty = true;
                return LSFS_OK;
            }
            break;
        }

        /* Check if deleted entry has enough space */
        if (de->ino == 0 && de->rec_len >= needed_size) {
            uint16_t remaining = de->rec_len - needed_size;

            de->ino = ino;
            de->name_len = (uint8_t)name_len;
            de->file_type = file_type;
            memcpy(de->name, name, name_len);

            if (remaining >= dirent_size(1)) {
                /* Create new empty entry with remaining space */
                de->rec_len = needed_size;
                struct lsfs_dirent *next = (struct lsfs_dirent *)((uint8_t *)de + needed_size);
                next->ino = 0;
                next->rec_len = remaining;
                next->name_len = 0;
                next->file_type = 0;
            }

            if (lsfs_inode_write_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }

            dir->disk_inode.mtime = lsfs_get_time_ns();
            dir->disk_inode.ctime = dir->disk_inode.mtime;
            dir->dirty = true;
            return LSFS_OK;
        }

        /* Check if entry can be split */
        uint16_t actual_size = dirent_size(de->name_len);
        uint16_t free_space = de->rec_len - actual_size;

        if (free_space >= needed_size) {
            /* Split this entry */
            de->rec_len = actual_size;

            struct lsfs_dirent *new_de = (struct lsfs_dirent *)((uint8_t *)de + actual_size);
            new_de->ino = ino;
            new_de->rec_len = free_space;
            new_de->name_len = (uint8_t)name_len;
            new_de->file_type = file_type;
            memcpy(new_de->name, name, name_len);

            if (lsfs_inode_write_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }

            dir->disk_inode.mtime = lsfs_get_time_ns();
            dir->disk_inode.ctime = dir->disk_inode.mtime;
            dir->dirty = true;
            return LSFS_OK;
        }

        offset += de->rec_len;
    }

    /* Need to allocate a new block */
    uint64_t new_block_idx = dir_size / LSFS_BLOCK_SIZE;
    memset(block, 0, LSFS_BLOCK_SIZE);

    struct lsfs_dirent *de = (struct lsfs_dirent *)block;
    de->ino = ino;
    de->rec_len = LSFS_BLOCK_SIZE;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);

    if (lsfs_inode_write_block(ctx, dir, new_block_idx, block) != LSFS_OK) {
        return LSFS_ERR_IO;
    }

    dir->disk_inode.size = (new_block_idx + 1) * LSFS_BLOCK_SIZE;
    dir->disk_inode.mtime = lsfs_get_time_ns();
    dir->disk_inode.ctime = dir->disk_inode.mtime;
    dir->dirty = true;

    LSFS_DEBUG("Added entry '%s' -> %u in directory %u",
               name, ino, dir->disk_inode.ino);

    return LSFS_OK;
}

/*
 * Remove an entry from a directory
 */
int lsfs_dir_remove(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                    const char *name)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    size_t name_len = strlen(name);
    uint64_t offset = 0;
    uint64_t dir_size = dir->disk_inode.size;
    struct lsfs_dirent *prev_de = NULL;
    uint64_t prev_block_idx = 0;

    if (!(dir->disk_inode.mode & S_IFDIR)) {
        return LSFS_ERR_NOTDIR;
    }

    /* Find and remove the entry */
    while (offset < dir_size) {
        uint64_t block_idx = offset / LSFS_BLOCK_SIZE;
        uint32_t block_offset = offset % LSFS_BLOCK_SIZE;

        if (block_offset == 0) {
            if (lsfs_inode_read_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }
            prev_de = NULL;
        }

        struct lsfs_dirent *de = (struct lsfs_dirent *)(block + block_offset);

        if (de->rec_len == 0) {
            break;
        }

        if (de->ino != 0 && de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0) {
            /* Found it - mark as deleted */
            if (prev_de && prev_block_idx == block_idx) {
                /* Merge with previous entry */
                prev_de->rec_len += de->rec_len;
            } else {
                /* Just mark as deleted */
                de->ino = 0;
            }

            if (lsfs_inode_write_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }

            dir->disk_inode.mtime = lsfs_get_time_ns();
            dir->disk_inode.ctime = dir->disk_inode.mtime;
            dir->dirty = true;

            LSFS_DEBUG("Removed entry '%s' from directory %u", name, dir->disk_inode.ino);
            return LSFS_OK;
        }

        prev_de = de;
        prev_block_idx = block_idx;
        offset += de->rec_len;
    }

    return LSFS_ERR_NOENT;
}

/*
 * Check if a directory is empty (only . and ..)
 */
int lsfs_dir_is_empty(struct lsfs_context *ctx, struct lsfs_inode_mem *dir)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    uint64_t offset = 0;
    uint64_t dir_size = dir->disk_inode.size;
    int entry_count = 0;

    if (!(dir->disk_inode.mode & S_IFDIR)) {
        return LSFS_ERR_NOTDIR;
    }

    while (offset < dir_size) {
        uint64_t block_idx = offset / LSFS_BLOCK_SIZE;
        uint32_t block_offset = offset % LSFS_BLOCK_SIZE;

        if (block_offset == 0) {
            if (lsfs_inode_read_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }
        }

        struct lsfs_dirent *de = (struct lsfs_dirent *)(block + block_offset);

        if (de->rec_len == 0) {
            break;
        }

        if (de->ino != 0) {
            /* Skip . and .. */
            if (de->name_len == 1 && de->name[0] == '.') {
                /* . entry */
            } else if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
                /* .. entry */
            } else {
                entry_count++;
            }
        }

        offset += de->rec_len;
    }

    return entry_count == 0 ? LSFS_OK : LSFS_ERR_NOTEMPTY;
}

/*
 * Iterate over directory entries
 */
int lsfs_dir_iterate(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                     int (*callback)(void *ctx, const char *name, uint32_t ino,
                                     uint8_t type, off_t offset),
                     void *callback_ctx, off_t start_offset)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    uint64_t offset = start_offset;
    uint64_t dir_size = dir->disk_inode.size;
    char name_buf[LSFS_NAME_MAX + 1];

    if (!(dir->disk_inode.mode & S_IFDIR)) {
        return LSFS_ERR_NOTDIR;
    }

    while (offset < dir_size) {
        uint64_t block_idx = offset / LSFS_BLOCK_SIZE;
        uint32_t block_offset = offset % LSFS_BLOCK_SIZE;

        if (block_offset == 0) {
            if (lsfs_inode_read_block(ctx, dir, block_idx, block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }
        }

        struct lsfs_dirent *de = (struct lsfs_dirent *)(block + block_offset);

        if (de->rec_len == 0) {
            break;
        }

        if (de->ino != 0 && de->name_len > 0) {
            memcpy(name_buf, de->name, de->name_len);
            name_buf[de->name_len] = '\0';

            int ret = callback(callback_ctx, name_buf, de->ino, de->file_type, offset);
            if (ret != 0) {
                return ret;
            }
        }

        offset += de->rec_len;
    }

    return LSFS_OK;
}

/*
 * Initialize a new directory with . and .. entries
 */
int lsfs_dir_init(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                  uint32_t parent_ino)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    struct lsfs_dirent *de;

    memset(block, 0, LSFS_BLOCK_SIZE);

    /* Create . entry */
    de = (struct lsfs_dirent *)block;
    de->ino = dir->disk_inode.ino;
    de->rec_len = dirent_size(1);
    de->name_len = 1;
    de->file_type = LSFS_FT_DIR;
    de->name[0] = '.';

    /* Create .. entry */
    struct lsfs_dirent *de_parent = (struct lsfs_dirent *)(block + de->rec_len);
    de_parent->ino = parent_ino;
    de_parent->rec_len = LSFS_BLOCK_SIZE - de->rec_len;
    de_parent->name_len = 2;
    de_parent->file_type = LSFS_FT_DIR;
    de_parent->name[0] = '.';
    de_parent->name[1] = '.';

    if (lsfs_inode_write_block(ctx, dir, 0, block) != LSFS_OK) {
        return LSFS_ERR_IO;
    }

    dir->disk_inode.size = LSFS_BLOCK_SIZE;
    dir->disk_inode.nlink = 2;  /* . and link from parent */
    dir->dirty = true;

    LSFS_DEBUG("Initialized directory %u with parent %u",
               dir->disk_inode.ino, parent_ino);

    return LSFS_OK;
}
