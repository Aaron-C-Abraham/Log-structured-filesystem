/*
 * LSFS - Log-Structured Filesystem
 * Inode Map Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lsfs.h"

#define IMAP_INITIAL_CAPACITY   1024
#define IMAP_GROWTH_FACTOR      2

/*
 * Initialize inode map
 */
int lsfs_imap_init(struct lsfs_imap *imap)
{
    imap->entries = calloc(IMAP_INITIAL_CAPACITY, sizeof(struct lsfs_imap_entry));
    if (!imap->entries) {
        return LSFS_ERR_NOMEM;
    }

    imap->capacity = IMAP_INITIAL_CAPACITY;
    imap->count = 0;
    imap->next_ino = LSFS_ROOT_INO + 1;  /* Start after root inode */

    if (pthread_rwlock_init(&imap->lock, NULL) != 0) {
        free(imap->entries);
        return LSFS_ERR_NOMEM;
    }

    return LSFS_OK;
}

/*
 * Destroy inode map
 */
void lsfs_imap_destroy(struct lsfs_imap *imap)
{
    if (imap->entries) {
        free(imap->entries);
        imap->entries = NULL;
    }
    pthread_rwlock_destroy(&imap->lock);
}

/*
 * Find entry index for inode number using binary search
 * Returns index if found, or -1 if not found
 */
static int imap_find(struct lsfs_imap *imap, uint32_t ino)
{
    if (imap->count == 0) {
        return -1;
    }

    int left = 0;
    int right = (int)imap->count - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (imap->entries[mid].ino == ino) {
            return mid;
        }
        if (imap->entries[mid].ino < ino) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return -1;
}

/*
 * Find insertion point for inode number
 * Returns index where entry should be inserted
 */
static uint32_t imap_find_insert_pos(struct lsfs_imap *imap, uint32_t ino)
{
    if (imap->count == 0) {
        return 0;
    }

    uint32_t left = 0;
    uint32_t right = imap->count;

    while (left < right) {
        uint32_t mid = left + (right - left) / 2;
        if (imap->entries[mid].ino < ino) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return left;
}

/*
 * Grow inode map capacity
 */
static int imap_grow(struct lsfs_imap *imap)
{
    uint32_t new_capacity = imap->capacity * IMAP_GROWTH_FACTOR;
    struct lsfs_imap_entry *new_entries;

    if (new_capacity > LSFS_MAX_INODES) {
        new_capacity = LSFS_MAX_INODES;
        if (new_capacity <= imap->capacity) {
            return LSFS_ERR_NOSPC;
        }
    }

    new_entries = realloc(imap->entries, new_capacity * sizeof(struct lsfs_imap_entry));
    if (!new_entries) {
        return LSFS_ERR_NOMEM;
    }

    imap->entries = new_entries;
    imap->capacity = new_capacity;
    return LSFS_OK;
}

/*
 * Get inode location from map
 */
int lsfs_imap_get(struct lsfs_imap *imap, uint32_t ino, uint64_t *location,
                  uint32_t *version)
{
    int ret = LSFS_ERR_NOENT;

    pthread_rwlock_rdlock(&imap->lock);

    int idx = imap_find(imap, ino);
    if (idx >= 0) {
        *location = imap->entries[idx].location;
        if (version) {
            *version = imap->entries[idx].version;
        }
        ret = LSFS_OK;
    }

    pthread_rwlock_unlock(&imap->lock);
    return ret;
}

/*
 * Set inode location in map
 */
int lsfs_imap_set(struct lsfs_imap *imap, uint32_t ino, uint64_t location)
{
    int ret = LSFS_OK;

    pthread_rwlock_wrlock(&imap->lock);

    int idx = imap_find(imap, ino);
    if (idx >= 0) {
        /* Update existing entry */
        imap->entries[idx].location = location;
        imap->entries[idx].version++;
    } else {
        /* Insert new entry */
        if (imap->count >= imap->capacity) {
            ret = imap_grow(imap);
            if (ret != LSFS_OK) {
                pthread_rwlock_unlock(&imap->lock);
                return ret;
            }
        }

        uint32_t pos = imap_find_insert_pos(imap, ino);

        /* Shift entries to make room */
        if (pos < imap->count) {
            memmove(&imap->entries[pos + 1], &imap->entries[pos],
                    (imap->count - pos) * sizeof(struct lsfs_imap_entry));
        }

        /* Insert new entry */
        imap->entries[pos].ino = ino;
        imap->entries[pos].location = location;
        imap->entries[pos].version = 1;
        imap->count++;
    }

    pthread_rwlock_unlock(&imap->lock);
    return ret;
}

/*
 * Remove inode from map
 */
int lsfs_imap_remove(struct lsfs_imap *imap, uint32_t ino)
{
    pthread_rwlock_wrlock(&imap->lock);

    int idx = imap_find(imap, ino);
    if (idx < 0) {
        pthread_rwlock_unlock(&imap->lock);
        return LSFS_ERR_NOENT;
    }

    /* Shift entries to fill gap */
    if ((uint32_t)idx < imap->count - 1) {
        memmove(&imap->entries[idx], &imap->entries[idx + 1],
                (imap->count - idx - 1) * sizeof(struct lsfs_imap_entry));
    }

    imap->count--;

    pthread_rwlock_unlock(&imap->lock);
    return LSFS_OK;
}

/*
 * Allocate a new inode number
 */
uint32_t lsfs_imap_alloc_ino(struct lsfs_imap *imap)
{
    uint32_t ino;

    pthread_rwlock_wrlock(&imap->lock);

    if (imap->next_ino >= LSFS_MAX_INODES) {
        /* Try to find a free inode number */
        ino = 0;
        for (uint32_t i = LSFS_ROOT_INO + 1; i < LSFS_MAX_INODES; i++) {
            int idx = imap_find(imap, i);
            if (idx < 0) {
                ino = i;
                break;
            }
        }
    } else {
        ino = imap->next_ino++;
    }

    pthread_rwlock_unlock(&imap->lock);
    return ino;
}

/*
 * Save inode map to disk
 */
int lsfs_imap_save(struct lsfs_context *ctx, uint64_t start_block)
{
    struct lsfs_imap *imap = &ctx->imap;
    uint8_t *buf;
    uint32_t blocks_needed;
    uint32_t entries_per_block;

    pthread_rwlock_rdlock(&imap->lock);

    entries_per_block = LSFS_BLOCK_SIZE / sizeof(struct lsfs_imap_entry);
    blocks_needed = LSFS_DIV_ROUND_UP(imap->count, entries_per_block);

    if (blocks_needed > LSFS_CHECKPOINT0_BLOCKS - 1) {
        pthread_rwlock_unlock(&imap->lock);
        LSFS_ERROR("Inode map too large");
        return LSFS_ERR_NOSPC;
    }

    buf = calloc(blocks_needed, LSFS_BLOCK_SIZE);
    if (!buf) {
        pthread_rwlock_unlock(&imap->lock);
        return LSFS_ERR_NOMEM;
    }

    memcpy(buf, imap->entries, imap->count * sizeof(struct lsfs_imap_entry));

    pthread_rwlock_unlock(&imap->lock);

    int ret = lsfs_write_blocks(ctx, start_block, blocks_needed, buf);
    free(buf);

    if (ret == LSFS_OK) {
        LSFS_DEBUG("Saved inode map: %u entries in %u blocks",
                   imap->count, blocks_needed);
    }

    return ret;
}

/*
 * Load inode map from disk
 */
int lsfs_imap_load(struct lsfs_context *ctx, uint64_t start_block,
                   uint32_t entry_count)
{
    struct lsfs_imap *imap = &ctx->imap;
    uint8_t *buf;
    uint32_t blocks_needed;
    uint32_t entries_per_block;

    entries_per_block = LSFS_BLOCK_SIZE / sizeof(struct lsfs_imap_entry);
    blocks_needed = LSFS_DIV_ROUND_UP(entry_count, entries_per_block);

    buf = calloc(blocks_needed, LSFS_BLOCK_SIZE);
    if (!buf) {
        return LSFS_ERR_NOMEM;
    }

    int ret = lsfs_read_blocks(ctx, start_block, blocks_needed, buf);
    if (ret != LSFS_OK) {
        free(buf);
        return ret;
    }

    pthread_rwlock_wrlock(&imap->lock);

    /* Ensure capacity */
    if (entry_count > imap->capacity) {
        struct lsfs_imap_entry *new_entries = realloc(imap->entries,
                                                       entry_count * sizeof(struct lsfs_imap_entry));
        if (!new_entries) {
            pthread_rwlock_unlock(&imap->lock);
            free(buf);
            return LSFS_ERR_NOMEM;
        }
        imap->entries = new_entries;
        imap->capacity = entry_count;
    }

    memcpy(imap->entries, buf, entry_count * sizeof(struct lsfs_imap_entry));
    imap->count = entry_count;

    /* Find highest inode number for next allocation */
    imap->next_ino = LSFS_ROOT_INO + 1;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (imap->entries[i].ino >= imap->next_ino) {
            imap->next_ino = imap->entries[i].ino + 1;
        }
    }

    pthread_rwlock_unlock(&imap->lock);
    free(buf);

    LSFS_DEBUG("Loaded inode map: %u entries, next_ino=%u",
               entry_count, imap->next_ino);

    return LSFS_OK;
}
