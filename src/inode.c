/*
 * LSFS - Log-Structured Filesystem
 * Inode Operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "lsfs.h"

/*
 * Hash function for inode number
 */
static inline uint32_t inode_hash(uint32_t ino)
{
    return ino % LSFS_INODE_CACHE_BUCKETS;
}

/*
 * Get current time in nanoseconds
 */
uint64_t lsfs_get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Initialize inode cache
 */
int lsfs_inode_cache_init(struct lsfs_inode_cache *cache)
{
    memset(cache, 0, sizeof(*cache));

    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        return LSFS_ERR_NOMEM;
    }

    return LSFS_OK;
}

/*
 * Destroy inode cache
 */
void lsfs_inode_cache_destroy(struct lsfs_inode_cache *cache)
{
    /* Free all cached inodes */
    for (int i = 0; i < LSFS_INODE_CACHE_BUCKETS; i++) {
        struct lsfs_inode_mem *inode = cache->buckets[i];
        while (inode) {
            struct lsfs_inode_mem *next = inode->next;
            pthread_mutex_destroy(&inode->lock);
            free(inode);
            inode = next;
        }
    }

    pthread_mutex_destroy(&cache->lock);
}

/*
 * Remove inode from LRU list
 */
static void inode_lru_remove(struct lsfs_inode_cache *cache,
                             struct lsfs_inode_mem *inode)
{
    if (inode->lru_prev) {
        inode->lru_prev->lru_next = inode->lru_next;
    } else {
        cache->lru_head = inode->lru_next;
    }

    if (inode->lru_next) {
        inode->lru_next->lru_prev = inode->lru_prev;
    } else {
        cache->lru_tail = inode->lru_prev;
    }

    inode->lru_prev = NULL;
    inode->lru_next = NULL;
}

/*
 * Add inode to end of LRU list (most recently used)
 */
static void inode_lru_add(struct lsfs_inode_cache *cache,
                          struct lsfs_inode_mem *inode)
{
    inode->lru_prev = cache->lru_tail;
    inode->lru_next = NULL;

    if (cache->lru_tail) {
        cache->lru_tail->lru_next = inode;
    }
    cache->lru_tail = inode;

    if (!cache->lru_head) {
        cache->lru_head = inode;
    }
}

/*
 * Evict least recently used inode if cache is full
 */
static void inode_cache_evict(struct lsfs_context *ctx)
{
    struct lsfs_inode_cache *cache = &ctx->icache;

    while (cache->count >= LSFS_INODE_CACHE_SIZE) {
        struct lsfs_inode_mem *victim = cache->lru_head;

        /* Find an inode with refcount 0 */
        while (victim && victim->refcount > 0) {
            victim = victim->lru_next;
        }

        if (!victim) {
            break;  /* All inodes in use */
        }

        /* Write back if dirty */
        if (victim->dirty) {
            lsfs_inode_write(ctx, victim);
        }

        /* Remove from hash chain */
        uint32_t hash = inode_hash(victim->disk_inode.ino);
        struct lsfs_inode_mem **pp = &cache->buckets[hash];
        while (*pp) {
            if (*pp == victim) {
                *pp = victim->next;
                break;
            }
            pp = &(*pp)->next;
        }

        /* Remove from LRU */
        inode_lru_remove(cache, victim);

        /* Free */
        pthread_mutex_destroy(&victim->lock);
        free(victim);
        cache->count--;
    }
}

/*
 * Get an inode by number (from cache or disk)
 */
struct lsfs_inode_mem *lsfs_inode_get(struct lsfs_context *ctx, uint32_t ino)
{
    struct lsfs_inode_cache *cache = &ctx->icache;
    struct lsfs_inode_mem *inode;
    uint32_t hash;
    uint64_t location;
    uint32_t version;

    pthread_mutex_lock(&cache->lock);

    /* Check cache first */
    hash = inode_hash(ino);
    inode = cache->buckets[hash];
    while (inode) {
        if (inode->disk_inode.ino == ino) {
            inode->refcount++;
            inode_lru_remove(cache, inode);
            inode_lru_add(cache, inode);
            pthread_mutex_unlock(&cache->lock);
            return inode;
        }
        inode = inode->next;
    }

    /* Not in cache, look up in inode map */
    if (lsfs_imap_get(&ctx->imap, ino, &location, &version) != LSFS_OK) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* Evict if necessary */
    inode_cache_evict(ctx);

    /* Allocate new cache entry */
    inode = calloc(1, sizeof(*inode));
    if (!inode) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* Read inode from disk */
    uint8_t block[LSFS_BLOCK_SIZE];
    if (lsfs_read_block(ctx, location, block) != LSFS_OK) {
        free(inode);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* Find inode within block (16 inodes per block) */
    uint32_t offset = (ino % 16) * sizeof(struct lsfs_inode);
    memcpy(&inode->disk_inode, block + offset, sizeof(struct lsfs_inode));

    /* Verify inode number matches */
    if (inode->disk_inode.ino != ino) {
        LSFS_ERROR("Inode mismatch: expected %u, got %u", ino, inode->disk_inode.ino);
        free(inode);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    inode->disk_location = location;
    inode->version = version;
    inode->refcount = 1;
    inode->dirty = false;
    pthread_mutex_init(&inode->lock, NULL);

    /* Add to cache */
    inode->next = cache->buckets[hash];
    cache->buckets[hash] = inode;
    inode_lru_add(cache, inode);
    cache->count++;

    pthread_mutex_unlock(&cache->lock);
    return inode;
}

/*
 * Release reference to an inode
 */
void lsfs_inode_put(struct lsfs_inode_mem *inode)
{
    if (inode && inode->refcount > 0) {
        inode->refcount--;
    }
}

/*
 * Allocate a new inode
 */
struct lsfs_inode_mem *lsfs_inode_alloc(struct lsfs_context *ctx, uint32_t mode)
{
    struct lsfs_inode_cache *cache = &ctx->icache;
    struct lsfs_inode_mem *inode;
    uint32_t ino;
    uint64_t now;

    /* Get a new inode number */
    ino = lsfs_imap_alloc_ino(&ctx->imap);
    if (ino == 0) {
        return NULL;
    }

    pthread_mutex_lock(&cache->lock);

    /* Evict if necessary */
    inode_cache_evict(ctx);

    /* Allocate cache entry */
    inode = calloc(1, sizeof(*inode));
    if (!inode) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    now = lsfs_get_time_ns();

    /* Initialize inode */
    inode->disk_inode.ino = ino;
    inode->disk_inode.mode = mode;
    inode->disk_inode.uid = getuid();
    inode->disk_inode.gid = getgid();
    inode->disk_inode.size = 0;
    inode->disk_inode.blocks = 0;
    inode->disk_inode.atime = now;
    inode->disk_inode.mtime = now;
    inode->disk_inode.ctime = now;
    inode->disk_inode.nlink = 1;
    inode->disk_inode.flags = 0;
    inode->disk_inode.generation = (uint64_t)rand();

    inode->disk_location = 0;
    inode->version = 0;
    inode->refcount = 1;
    inode->dirty = true;
    pthread_mutex_init(&inode->lock, NULL);

    /* Add to cache */
    uint32_t hash = inode_hash(ino);
    inode->next = cache->buckets[hash];
    cache->buckets[hash] = inode;
    inode_lru_add(cache, inode);
    cache->count++;

    ctx->sb.inode_count++;

    pthread_mutex_unlock(&cache->lock);

    LSFS_DEBUG("Allocated inode %u, mode 0%o", ino, mode);
    return inode;
}

/*
 * Free an inode
 */
int lsfs_inode_free(struct lsfs_context *ctx, struct lsfs_inode_mem *inode)
{
    uint32_t ino = inode->disk_inode.ino;

    /* Mark all blocks as dead for GC */
    for (int i = 0; i < LSFS_DIRECT_BLOCKS; i++) {
        if (inode->disk_inode.direct[i]) {
            lsfs_gc_mark_block_dead(ctx, inode->disk_inode.direct[i]);
            inode->disk_inode.direct[i] = 0;
        }
    }

    if (inode->disk_inode.indirect) {
        lsfs_gc_mark_block_dead(ctx, inode->disk_inode.indirect);
        /* TODO: Mark indirect block contents as dead */
        inode->disk_inode.indirect = 0;
    }

    if (inode->disk_inode.double_indirect) {
        lsfs_gc_mark_block_dead(ctx, inode->disk_inode.double_indirect);
        /* TODO: Mark double indirect block contents as dead */
        inode->disk_inode.double_indirect = 0;
    }

    /* Mark inode location as dead */
    if (inode->disk_location) {
        lsfs_gc_mark_block_dead(ctx, inode->disk_location);
    }

    /* Remove from inode map */
    lsfs_imap_remove(&ctx->imap, ino);

    /* Mark as deleted */
    inode->disk_inode.flags |= LSFS_INODE_DELETED;
    inode->dirty = false;

    if (ctx->sb.inode_count > 0) {
        ctx->sb.inode_count--;
    }

    LSFS_DEBUG("Freed inode %u", ino);
    return LSFS_OK;
}

/*
 * Write inode to log
 */
int lsfs_inode_write(struct lsfs_context *ctx, struct lsfs_inode_mem *inode)
{
    uint64_t new_location;

    if (!inode->dirty) {
        return LSFS_OK;
    }

    /* Mark old location as dead */
    if (inode->disk_location) {
        lsfs_gc_mark_block_dead(ctx, inode->disk_location);
    }

    /* Append inode to log */
    new_location = lsfs_segment_append_block(ctx, &inode->disk_inode,
                                             inode->disk_inode.ino, 0,
                                             LSFS_BLOCK_TYPE_INODE);
    if (new_location == 0) {
        return LSFS_ERR_NOSPC;
    }

    /* Update inode map */
    lsfs_imap_set(&ctx->imap, inode->disk_inode.ino, new_location);

    inode->disk_location = new_location;
    inode->version++;
    inode->dirty = false;

    LSFS_DEBUG("Wrote inode %u to block %" PRIu64,
               inode->disk_inode.ino, new_location);

    return LSFS_OK;
}

/*
 * Read a data block from an inode
 */
int lsfs_inode_read_block(struct lsfs_context *ctx, struct lsfs_inode_mem *inode,
                          uint64_t block_idx, void *buf)
{
    uint64_t block_addr = 0;
    uint64_t blocks_per_indirect = LSFS_BLOCK_SIZE / sizeof(uint64_t);

    /* Direct blocks */
    if (block_idx < LSFS_DIRECT_BLOCKS) {
        block_addr = inode->disk_inode.direct[block_idx];
    }
    /* Single indirect */
    else if (block_idx < LSFS_DIRECT_BLOCKS + blocks_per_indirect) {
        if (inode->disk_inode.indirect == 0) {
            memset(buf, 0, LSFS_BLOCK_SIZE);
            return LSFS_OK;
        }

        uint64_t indirect_block[LSFS_BLOCK_SIZE / sizeof(uint64_t)];
        if (lsfs_read_block(ctx, inode->disk_inode.indirect, indirect_block) != LSFS_OK) {
            return LSFS_ERR_IO;
        }

        block_addr = indirect_block[block_idx - LSFS_DIRECT_BLOCKS];
    }
    /* Double indirect */
    else if (block_idx < LSFS_DIRECT_BLOCKS + blocks_per_indirect +
             blocks_per_indirect * blocks_per_indirect) {
        if (inode->disk_inode.double_indirect == 0) {
            memset(buf, 0, LSFS_BLOCK_SIZE);
            return LSFS_OK;
        }

        uint64_t d_indirect_block[LSFS_BLOCK_SIZE / sizeof(uint64_t)];
        if (lsfs_read_block(ctx, inode->disk_inode.double_indirect, d_indirect_block) != LSFS_OK) {
            return LSFS_ERR_IO;
        }

        uint64_t idx = block_idx - LSFS_DIRECT_BLOCKS - blocks_per_indirect;
        uint64_t d_idx = idx / blocks_per_indirect;
        uint64_t i_idx = idx % blocks_per_indirect;

        if (d_indirect_block[d_idx] == 0) {
            memset(buf, 0, LSFS_BLOCK_SIZE);
            return LSFS_OK;
        }

        uint64_t indirect_block[LSFS_BLOCK_SIZE / sizeof(uint64_t)];
        if (lsfs_read_block(ctx, d_indirect_block[d_idx], indirect_block) != LSFS_OK) {
            return LSFS_ERR_IO;
        }

        block_addr = indirect_block[i_idx];
    } else {
        return LSFS_ERR_INVAL;
    }

    if (block_addr == 0) {
        memset(buf, 0, LSFS_BLOCK_SIZE);
        return LSFS_OK;
    }

    return lsfs_read_block(ctx, block_addr, buf);
}

/*
 * Write a data block to an inode
 */
int lsfs_inode_write_block(struct lsfs_context *ctx, struct lsfs_inode_mem *inode,
                           uint64_t block_idx, const void *buf)
{
    uint64_t blocks_per_indirect = LSFS_BLOCK_SIZE / sizeof(uint64_t);
    uint64_t old_addr = 0;
    uint64_t new_addr;

    /* Get current block address */
    if (block_idx < LSFS_DIRECT_BLOCKS) {
        old_addr = inode->disk_inode.direct[block_idx];
    }

    /* Mark old block as dead */
    if (old_addr) {
        lsfs_gc_mark_block_dead(ctx, old_addr);
    }

    /* Append new block to log */
    new_addr = lsfs_segment_append_block(ctx, buf, inode->disk_inode.ino,
                                         (uint32_t)block_idx, LSFS_BLOCK_TYPE_DATA);
    if (new_addr == 0) {
        return LSFS_ERR_NOSPC;
    }

    /* Update block pointer */
    if (block_idx < LSFS_DIRECT_BLOCKS) {
        inode->disk_inode.direct[block_idx] = new_addr;
    }
    /* Single indirect */
    else if (block_idx < LSFS_DIRECT_BLOCKS + blocks_per_indirect) {
        uint64_t indirect_block[LSFS_BLOCK_SIZE / sizeof(uint64_t)];

        if (inode->disk_inode.indirect == 0) {
            memset(indirect_block, 0, LSFS_BLOCK_SIZE);
        } else {
            if (lsfs_read_block(ctx, inode->disk_inode.indirect, indirect_block) != LSFS_OK) {
                return LSFS_ERR_IO;
            }
            lsfs_gc_mark_block_dead(ctx, inode->disk_inode.indirect);
        }

        indirect_block[block_idx - LSFS_DIRECT_BLOCKS] = new_addr;

        uint64_t new_indirect = lsfs_segment_append_block(ctx, indirect_block,
                                                          inode->disk_inode.ino, 0,
                                                          LSFS_BLOCK_TYPE_INDIRECT);
        if (new_indirect == 0) {
            return LSFS_ERR_NOSPC;
        }
        inode->disk_inode.indirect = new_indirect;
    }
    /* Double indirect - simplified, full implementation would be similar */
    else {
        LSFS_ERROR("Double indirect blocks not fully implemented");
        return LSFS_ERR_NOSPC;
    }

    inode->disk_inode.blocks = LSFS_MAX(inode->disk_inode.blocks, block_idx + 1);
    inode->dirty = true;

    return LSFS_OK;
}

/*
 * Convert inode to stat structure
 */
void lsfs_inode_to_stat(struct lsfs_inode_mem *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));

    st->st_ino = inode->disk_inode.ino;
    st->st_mode = inode->disk_inode.mode;
    st->st_nlink = inode->disk_inode.nlink;
    st->st_uid = inode->disk_inode.uid;
    st->st_gid = inode->disk_inode.gid;
    st->st_size = inode->disk_inode.size;
    st->st_blocks = (inode->disk_inode.size + 511) / 512;
    st->st_blksize = LSFS_BLOCK_SIZE;

    /* Convert nanoseconds to timespec */
    st->st_atim.tv_sec = inode->disk_inode.atime / 1000000000ULL;
    st->st_atim.tv_nsec = inode->disk_inode.atime % 1000000000ULL;
    st->st_mtim.tv_sec = inode->disk_inode.mtime / 1000000000ULL;
    st->st_mtim.tv_nsec = inode->disk_inode.mtime % 1000000000ULL;
    st->st_ctim.tv_sec = inode->disk_inode.ctime / 1000000000ULL;
    st->st_ctim.tv_nsec = inode->disk_inode.ctime % 1000000000ULL;
}
