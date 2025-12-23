/*
 * LSFS - Log-Structured Filesystem
 * Block I/O Layer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "lsfs.h"

/*
 * Initialize I/O layer - open the disk image file
 */
int lsfs_io_init(struct lsfs_context *ctx, const char *path)
{
    struct stat st;

    ctx->disk_path = strdup(path);
    if (!ctx->disk_path) {
        LSFS_ERROR("Failed to allocate disk path");
        return LSFS_ERR_NOMEM;
    }

    /* Open disk image */
    ctx->fd = open(path, O_RDWR);
    if (ctx->fd < 0) {
        LSFS_ERROR("Failed to open disk image: %s", strerror(errno));
        free(ctx->disk_path);
        ctx->disk_path = NULL;
        return LSFS_ERR_IO;
    }

    /* Get disk size */
    if (fstat(ctx->fd, &st) < 0) {
        LSFS_ERROR("Failed to stat disk image: %s", strerror(errno));
        close(ctx->fd);
        free(ctx->disk_path);
        ctx->disk_path = NULL;
        return LSFS_ERR_IO;
    }

    ctx->disk_size = st.st_size;
    LSFS_INFO("Opened disk image: %s (%" PRIu64 " bytes)",
              path, ctx->disk_size);

    return LSFS_OK;
}

/*
 * Cleanup I/O layer
 */
void lsfs_io_destroy(struct lsfs_context *ctx)
{
    if (ctx->fd >= 0) {
        fsync(ctx->fd);
        close(ctx->fd);
        ctx->fd = -1;
    }

    if (ctx->disk_path) {
        free(ctx->disk_path);
        ctx->disk_path = NULL;
    }
}

/*
 * Read a single block
 */
int lsfs_read_block(struct lsfs_context *ctx, uint64_t block_num, void *buf)
{
    off_t offset = (off_t)block_num * LSFS_BLOCK_SIZE;
    ssize_t ret;

    if (offset + LSFS_BLOCK_SIZE > (off_t)ctx->disk_size) {
        LSFS_ERROR("Read beyond end of disk: block %" PRIu64, block_num);
        return LSFS_ERR_IO;
    }

    ret = pread(ctx->fd, buf, LSFS_BLOCK_SIZE, offset);
    if (ret != LSFS_BLOCK_SIZE) {
        LSFS_ERROR("Failed to read block %" PRIu64 ": %s",
                   block_num, strerror(errno));
        return LSFS_ERR_IO;
    }

    return LSFS_OK;
}

/*
 * Write a single block
 */
int lsfs_write_block(struct lsfs_context *ctx, uint64_t block_num, const void *buf)
{
    off_t offset = (off_t)block_num * LSFS_BLOCK_SIZE;
    ssize_t ret;

    if (ctx->readonly) {
        LSFS_ERROR("Filesystem is read-only");
        return LSFS_ERR_IO;
    }

    if (offset + LSFS_BLOCK_SIZE > (off_t)ctx->disk_size) {
        LSFS_ERROR("Write beyond end of disk: block %" PRIu64, block_num);
        return LSFS_ERR_IO;
    }

    ret = pwrite(ctx->fd, buf, LSFS_BLOCK_SIZE, offset);
    if (ret != LSFS_BLOCK_SIZE) {
        LSFS_ERROR("Failed to write block %" PRIu64 ": %s",
                   block_num, strerror(errno));
        return LSFS_ERR_IO;
    }

    return LSFS_OK;
}

/*
 * Read multiple contiguous blocks
 */
int lsfs_read_blocks(struct lsfs_context *ctx, uint64_t start_block,
                     uint32_t count, void *buf)
{
    off_t offset = (off_t)start_block * LSFS_BLOCK_SIZE;
    size_t size = (size_t)count * LSFS_BLOCK_SIZE;
    ssize_t ret;

    if (offset + (off_t)size > (off_t)ctx->disk_size) {
        LSFS_ERROR("Read beyond end of disk");
        return LSFS_ERR_IO;
    }

    ret = pread(ctx->fd, buf, size, offset);
    if (ret != (ssize_t)size) {
        LSFS_ERROR("Failed to read blocks: %s", strerror(errno));
        return LSFS_ERR_IO;
    }

    return LSFS_OK;
}

/*
 * Write multiple contiguous blocks
 */
int lsfs_write_blocks(struct lsfs_context *ctx, uint64_t start_block,
                      uint32_t count, const void *buf)
{
    off_t offset = (off_t)start_block * LSFS_BLOCK_SIZE;
    size_t size = (size_t)count * LSFS_BLOCK_SIZE;
    ssize_t ret;

    if (ctx->readonly) {
        LSFS_ERROR("Filesystem is read-only");
        return LSFS_ERR_IO;
    }

    if (offset + (off_t)size > (off_t)ctx->disk_size) {
        LSFS_ERROR("Write beyond end of disk");
        return LSFS_ERR_IO;
    }

    ret = pwrite(ctx->fd, buf, size, offset);
    if (ret != (ssize_t)size) {
        LSFS_ERROR("Failed to write blocks: %s", strerror(errno));
        return LSFS_ERR_IO;
    }

    return LSFS_OK;
}

/*
 * Sync all data to disk
 */
int lsfs_sync(struct lsfs_context *ctx)
{
    if (fsync(ctx->fd) < 0) {
        LSFS_ERROR("Failed to sync: %s", strerror(errno));
        return LSFS_ERR_IO;
    }
    return LSFS_OK;
}

/*
 * Buffer Pool Implementation
 */

static inline uint32_t buffer_hash(uint64_t block_num)
{
    return (uint32_t)(block_num % LSFS_BUFFER_HASH_SIZE);
}

/*
 * Initialize buffer pool
 */
int lsfs_buffer_pool_init(struct lsfs_buffer_pool *pool)
{
    memset(pool, 0, sizeof(*pool));

    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        return LSFS_ERR_NOMEM;
    }

    /* Initialize LRU list */
    for (int i = 0; i < LSFS_BUFFER_POOL_SIZE; i++) {
        pool->buffers[i].valid = false;
        pool->buffers[i].dirty = false;
        pool->buffers[i].refcount = 0;

        /* Add to LRU list */
        if (i == 0) {
            pool->lru_head = &pool->buffers[i];
            pool->buffers[i].lru_prev = NULL;
        } else {
            pool->buffers[i].lru_prev = &pool->buffers[i - 1];
            pool->buffers[i - 1].lru_next = &pool->buffers[i];
        }

        if (i == LSFS_BUFFER_POOL_SIZE - 1) {
            pool->lru_tail = &pool->buffers[i];
            pool->buffers[i].lru_next = NULL;
        }
    }

    return LSFS_OK;
}

/*
 * Destroy buffer pool
 */
void lsfs_buffer_pool_destroy(struct lsfs_buffer_pool *pool)
{
    pthread_mutex_destroy(&pool->lock);
}

/*
 * Move buffer to end of LRU list (most recently used)
 */
static void buffer_touch(struct lsfs_buffer_pool *pool, struct lsfs_buffer *buf)
{
    /* Already at tail */
    if (buf == pool->lru_tail) {
        return;
    }

    /* Remove from current position */
    if (buf->lru_prev) {
        buf->lru_prev->lru_next = buf->lru_next;
    } else {
        pool->lru_head = buf->lru_next;
    }

    if (buf->lru_next) {
        buf->lru_next->lru_prev = buf->lru_prev;
    }

    /* Add to tail */
    buf->lru_prev = pool->lru_tail;
    buf->lru_next = NULL;
    if (pool->lru_tail) {
        pool->lru_tail->lru_next = buf;
    }
    pool->lru_tail = buf;

    if (!pool->lru_head) {
        pool->lru_head = buf;
    }
}

/*
 * Find a buffer to evict (LRU with refcount 0)
 */
static struct lsfs_buffer *buffer_evict(struct lsfs_context *ctx,
                                        struct lsfs_buffer_pool *pool)
{
    struct lsfs_buffer *buf = pool->lru_head;

    while (buf) {
        if (buf->refcount == 0) {
            /* If dirty, write back first */
            if (buf->dirty && buf->valid) {
                if (lsfs_write_block(ctx, buf->block_num, buf->data) == LSFS_OK) {
                    buf->dirty = false;
                }
            }

            /* Remove from hash chain */
            if (buf->valid) {
                uint32_t hash = buffer_hash(buf->block_num);
                struct lsfs_buffer **pp = &pool->hash[hash];
                while (*pp) {
                    if (*pp == buf) {
                        *pp = buf->hash_next;
                        break;
                    }
                    pp = &(*pp)->hash_next;
                }
            }

            buf->valid = false;
            buf->dirty = false;
            buf->hash_next = NULL;
            return buf;
        }
        buf = buf->lru_next;
    }

    return NULL;
}

/*
 * Get a buffer for a block
 */
struct lsfs_buffer *lsfs_buffer_get(struct lsfs_context *ctx, uint64_t block_num)
{
    struct lsfs_buffer_pool *pool = &ctx->bufpool;
    struct lsfs_buffer *buf;
    uint32_t hash;

    pthread_mutex_lock(&pool->lock);

    /* Check if already in cache */
    hash = buffer_hash(block_num);
    buf = pool->hash[hash];
    while (buf) {
        if (buf->valid && buf->block_num == block_num) {
            buf->refcount++;
            buffer_touch(pool, buf);
            pthread_mutex_unlock(&pool->lock);
            return buf;
        }
        buf = buf->hash_next;
    }

    /* Not in cache, find a buffer to use */
    buf = buffer_evict(ctx, pool);
    if (!buf) {
        LSFS_ERROR("No free buffers available");
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* Read block from disk */
    if (lsfs_read_block(ctx, block_num, buf->data) != LSFS_OK) {
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* Set up buffer */
    buf->block_num = block_num;
    buf->valid = true;
    buf->dirty = false;
    buf->refcount = 1;

    /* Add to hash chain */
    buf->hash_next = pool->hash[hash];
    pool->hash[hash] = buf;

    buffer_touch(pool, buf);
    pthread_mutex_unlock(&pool->lock);

    return buf;
}

/*
 * Release a buffer
 */
void lsfs_buffer_put(struct lsfs_buffer *buf)
{
    if (buf && buf->refcount > 0) {
        buf->refcount--;
    }
}

/*
 * Flush all dirty buffers
 */
int lsfs_buffer_flush(struct lsfs_context *ctx)
{
    struct lsfs_buffer_pool *pool = &ctx->bufpool;
    int ret = LSFS_OK;

    pthread_mutex_lock(&pool->lock);

    for (int i = 0; i < LSFS_BUFFER_POOL_SIZE; i++) {
        struct lsfs_buffer *buf = &pool->buffers[i];
        if (buf->valid && buf->dirty) {
            if (lsfs_write_block(ctx, buf->block_num, buf->data) == LSFS_OK) {
                buf->dirty = false;
            } else {
                ret = LSFS_ERR_IO;
            }
        }
    }

    pthread_mutex_unlock(&pool->lock);

    return ret;
}
