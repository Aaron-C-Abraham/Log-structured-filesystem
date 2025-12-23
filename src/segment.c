/*
 * LSFS - Log-Structured Filesystem
 * Segment Management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "lsfs.h"

/*
 * Convert segment ID and offset to absolute block number
 */
uint64_t lsfs_segment_to_block(uint32_t segment_id, uint32_t offset)
{
    return LSFS_LOG_START + (uint64_t)segment_id * LSFS_SEGMENT_BLOCKS + offset;
}

/*
 * Convert absolute block number to segment ID and offset
 */
void lsfs_block_to_segment(uint64_t block, uint32_t *segment_id, uint32_t *offset)
{
    if (block < LSFS_LOG_START) {
        *segment_id = 0;
        *offset = 0;
        return;
    }

    uint64_t log_block = block - LSFS_LOG_START;
    *segment_id = (uint32_t)(log_block / LSFS_SEGMENT_BLOCKS);
    *offset = (uint32_t)(log_block % LSFS_SEGMENT_BLOCKS);
}

/*
 * Initialize segment buffer
 */
int lsfs_segment_buffer_init(struct lsfs_segment_buffer *segbuf)
{
    segbuf->data = calloc(1, LSFS_SEGMENT_SIZE);
    if (!segbuf->data) {
        return LSFS_ERR_NOMEM;
    }

    segbuf->block_info = calloc(LSFS_SEGMENT_BLOCKS, sizeof(struct lsfs_block_info));
    if (!segbuf->block_info) {
        free(segbuf->data);
        segbuf->data = NULL;
        return LSFS_ERR_NOMEM;
    }

    segbuf->segment_id = 0;
    segbuf->block_count = 1;  /* Reserve first block for segment header */

    if (pthread_mutex_init(&segbuf->lock, NULL) != 0) {
        free(segbuf->block_info);
        free(segbuf->data);
        return LSFS_ERR_NOMEM;
    }

    return LSFS_OK;
}

/*
 * Destroy segment buffer
 */
void lsfs_segment_buffer_destroy(struct lsfs_segment_buffer *segbuf)
{
    if (segbuf->data) {
        free(segbuf->data);
        segbuf->data = NULL;
    }
    if (segbuf->block_info) {
        free(segbuf->block_info);
        segbuf->block_info = NULL;
    }
    pthread_mutex_destroy(&segbuf->lock);
}

/*
 * Initialize segment table from disk
 */
int lsfs_segment_init(struct lsfs_context *ctx)
{
    struct lsfs_segment_table *table = &ctx->segtable;
    uint32_t num_segments = (uint32_t)ctx->sb.total_segments;

    /* Initialize segment buffer */
    if (lsfs_segment_buffer_init(&ctx->segbuf) != LSFS_OK) {
        return LSFS_ERR_NOMEM;
    }

    /* Allocate segment table */
    table->entries = calloc(num_segments, sizeof(struct lsfs_segment_usage));
    if (!table->entries) {
        lsfs_segment_buffer_destroy(&ctx->segbuf);
        return LSFS_ERR_NOMEM;
    }

    table->count = num_segments;
    table->free_count = num_segments;

    if (pthread_mutex_init(&table->lock, NULL) != 0) {
        free(table->entries);
        lsfs_segment_buffer_destroy(&ctx->segbuf);
        return LSFS_ERR_NOMEM;
    }

    /* Read segment usage table from disk */
    uint8_t *buf = calloc(LSFS_SEGTABLE_BLOCKS, LSFS_BLOCK_SIZE);
    if (!buf) {
        pthread_mutex_destroy(&table->lock);
        free(table->entries);
        lsfs_segment_buffer_destroy(&ctx->segbuf);
        return LSFS_ERR_NOMEM;
    }

    if (lsfs_read_blocks(ctx, LSFS_SEGTABLE_START, LSFS_SEGTABLE_BLOCKS, buf) == LSFS_OK) {
        size_t table_size = num_segments * sizeof(struct lsfs_segment_usage);
        if (table_size <= LSFS_SEGTABLE_BLOCKS * LSFS_BLOCK_SIZE) {
            memcpy(table->entries, buf, table_size);
        }

        /* Count free segments */
        table->free_count = 0;
        for (uint32_t i = 0; i < num_segments; i++) {
            if (table->entries[i].state == LSFS_SEG_FREE) {
                table->free_count++;
            }
        }
    }

    free(buf);

    /* Allocate initial segment for writes */
    if (lsfs_segment_alloc(ctx, &ctx->segbuf.segment_id) != LSFS_OK) {
        LSFS_ERROR("Failed to allocate initial segment");
        /* Continue anyway, will fail on first write */
    }

    LSFS_INFO("Segment table initialized: %u segments, %u free",
              table->count, table->free_count);

    return LSFS_OK;
}

/*
 * Destroy segment table
 */
void lsfs_segment_destroy(struct lsfs_context *ctx)
{
    /* Flush current segment buffer */
    if (ctx->segbuf.block_count > 1) {
        lsfs_segment_flush(ctx);
    }

    lsfs_segment_buffer_destroy(&ctx->segbuf);

    if (ctx->segtable.entries) {
        /* Save segment table to disk */
        uint8_t *buf = calloc(LSFS_SEGTABLE_BLOCKS, LSFS_BLOCK_SIZE);
        if (buf) {
            size_t table_size = ctx->segtable.count * sizeof(struct lsfs_segment_usage);
            memcpy(buf, ctx->segtable.entries, table_size);
            lsfs_write_blocks(ctx, LSFS_SEGTABLE_START, LSFS_SEGTABLE_BLOCKS, buf);
            free(buf);
        }

        free(ctx->segtable.entries);
        ctx->segtable.entries = NULL;
    }

    pthread_mutex_destroy(&ctx->segtable.lock);
}

/*
 * Allocate a free segment
 */
int lsfs_segment_alloc(struct lsfs_context *ctx, uint32_t *segment_id)
{
    struct lsfs_segment_table *table = &ctx->segtable;

    pthread_mutex_lock(&table->lock);

    if (table->free_count == 0) {
        pthread_mutex_unlock(&table->lock);
        return LSFS_ERR_NOSPC;
    }

    /* Find a free segment */
    for (uint32_t i = 0; i < table->count; i++) {
        if (table->entries[i].state == LSFS_SEG_FREE) {
            table->entries[i].state = LSFS_SEG_ACTIVE;
            table->entries[i].segment_id = i;
            table->entries[i].live_blocks = 0;
            table->entries[i].timestamp = (uint64_t)time(NULL);
            table->free_count--;
            ctx->sb.free_segments = table->free_count;

            *segment_id = i;
            pthread_mutex_unlock(&table->lock);

            LSFS_DEBUG("Allocated segment %u (free: %u)", i, table->free_count);
            return LSFS_OK;
        }
    }

    pthread_mutex_unlock(&table->lock);
    return LSFS_ERR_NOSPC;
}

/*
 * Free a segment
 */
int lsfs_segment_free(struct lsfs_context *ctx, uint32_t segment_id)
{
    struct lsfs_segment_table *table = &ctx->segtable;

    if (segment_id >= table->count) {
        return LSFS_ERR_INVAL;
    }

    pthread_mutex_lock(&table->lock);

    table->entries[segment_id].state = LSFS_SEG_FREE;
    table->entries[segment_id].live_blocks = 0;
    table->free_count++;
    ctx->sb.free_segments = table->free_count;

    pthread_mutex_unlock(&table->lock);

    LSFS_DEBUG("Freed segment %u (free: %u)", segment_id, table->free_count);
    return LSFS_OK;
}

/*
 * Append a block to the current segment
 * Returns the absolute block address, or 0 on failure
 */
uint64_t lsfs_segment_append_block(struct lsfs_context *ctx, const void *data,
                                   uint32_t ino, uint32_t offset, uint8_t type)
{
    struct lsfs_segment_buffer *segbuf = &ctx->segbuf;
    uint64_t block_addr;

    pthread_mutex_lock(&segbuf->lock);

    /* Check if segment is full */
    if (segbuf->block_count >= LSFS_SEGMENT_BLOCKS) {
        /* Flush current segment */
        pthread_mutex_unlock(&segbuf->lock);
        if (lsfs_segment_flush(ctx) != LSFS_OK) {
            return 0;
        }
        pthread_mutex_lock(&segbuf->lock);
    }

    /* Copy data to segment buffer */
    uint32_t block_idx = segbuf->block_count;
    memcpy(segbuf->data + block_idx * LSFS_BLOCK_SIZE, data, LSFS_BLOCK_SIZE);

    /* Record block info */
    segbuf->block_info[block_idx].ino = ino;
    segbuf->block_info[block_idx].offset = offset;
    segbuf->block_info[block_idx].type = type;

    /* Calculate block address */
    block_addr = lsfs_segment_to_block(segbuf->segment_id, block_idx);

    segbuf->block_count++;
    ctx->writes_since_checkpoint++;

    pthread_mutex_unlock(&segbuf->lock);

    LSFS_DEBUG("Appended block %" PRIu64 " (seg %u, off %u, ino %u)",
               block_addr, segbuf->segment_id, block_idx, ino);

    return block_addr;
}

/*
 * Flush the segment buffer to disk
 */
int lsfs_segment_flush(struct lsfs_context *ctx)
{
    struct lsfs_segment_buffer *segbuf = &ctx->segbuf;
    struct lsfs_segment_table *table = &ctx->segtable;
    int ret;

    pthread_mutex_lock(&segbuf->lock);
    pthread_mutex_lock(&ctx->write_lock);

    if (segbuf->block_count <= 1) {
        /* Nothing to flush (only header block) */
        pthread_mutex_unlock(&ctx->write_lock);
        pthread_mutex_unlock(&segbuf->lock);
        return LSFS_OK;
    }

    /* Prepare segment header */
    struct lsfs_segment_summary *summary = (struct lsfs_segment_summary *)segbuf->data;
    summary->header.magic = LSFS_SEGMENT_MAGIC;
    summary->header.segment_id = segbuf->segment_id;
    summary->header.timestamp = (uint64_t)time(NULL);
    summary->header.block_count = segbuf->block_count;
    summary->header.checksum = 0;  /* TODO: Calculate CRC32 */

    /* Copy block info to segment header */
    size_t info_size = (segbuf->block_count - 1) * sizeof(struct lsfs_block_info);
    if (sizeof(struct lsfs_segment_header) + info_size <= LSFS_BLOCK_SIZE) {
        memcpy(summary->blocks, segbuf->block_info + 1, info_size);
    }

    /* Write segment to disk */
    uint64_t start_block = lsfs_segment_to_block(segbuf->segment_id, 0);
    ret = lsfs_write_blocks(ctx, start_block, segbuf->block_count, segbuf->data);

    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to write segment %u", segbuf->segment_id);
        pthread_mutex_unlock(&ctx->write_lock);
        pthread_mutex_unlock(&segbuf->lock);
        return ret;
    }

    /* Update segment table */
    pthread_mutex_lock(&table->lock);
    table->entries[segbuf->segment_id].state = LSFS_SEG_FULL;
    table->entries[segbuf->segment_id].live_blocks = segbuf->block_count - 1;
    table->entries[segbuf->segment_id].timestamp = summary->header.timestamp;
    pthread_mutex_unlock(&table->lock);

    /* Update log head */
    ctx->sb.log_head = start_block + segbuf->block_count;

    LSFS_DEBUG("Flushed segment %u (%u blocks)", segbuf->segment_id, segbuf->block_count);

    /* Allocate new segment */
    uint32_t new_segment_id;
    if (lsfs_segment_alloc(ctx, &new_segment_id) != LSFS_OK) {
        LSFS_ERROR("Failed to allocate new segment after flush");
        /* Trigger GC */
        lsfs_gc_trigger(ctx);
    } else {
        segbuf->segment_id = new_segment_id;
    }

    /* Reset buffer */
    memset(segbuf->data, 0, LSFS_SEGMENT_SIZE);
    memset(segbuf->block_info, 0, LSFS_SEGMENT_BLOCKS * sizeof(struct lsfs_block_info));
    segbuf->block_count = 1;

    /* Check if checkpoint is needed */
    if (lsfs_checkpoint_needed(ctx)) {
        pthread_mutex_unlock(&ctx->write_lock);
        pthread_mutex_unlock(&segbuf->lock);
        lsfs_checkpoint_write(ctx);
        return LSFS_OK;
    }

    pthread_mutex_unlock(&ctx->write_lock);
    pthread_mutex_unlock(&segbuf->lock);

    return LSFS_OK;
}
