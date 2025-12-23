/*
 * LSFS - Log-Structured Filesystem
 * Garbage Collection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>

#include "lsfs.h"

#define GC_THRESHOLD_LOW        10      /* Start GC when free segments < 10% */
#define GC_THRESHOLD_HIGH       20      /* Stop GC when free segments > 20% */
#define GC_UTILIZATION_THRESHOLD 50     /* Only clean segments with < 50% live data */

/*
 * Background GC thread function
 */
static void *gc_thread_func(void *arg)
{
    struct lsfs_context *ctx = (struct lsfs_context *)arg;

    LSFS_INFO("GC thread started");

    while (ctx->gc_running) {
        pthread_mutex_lock(&ctx->gc_lock);

        /* Wait for GC trigger or timeout */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;  /* Wake up every 5 seconds */

        pthread_cond_timedwait(&ctx->gc_cond, &ctx->gc_lock, &ts);

        pthread_mutex_unlock(&ctx->gc_lock);

        if (!ctx->gc_running) {
            break;
        }

        /* Check if GC is needed */
        if (lsfs_gc_needed(ctx)) {
            LSFS_DEBUG("GC triggered");
            lsfs_gc_run(ctx);
        }
    }

    LSFS_INFO("GC thread stopped");
    return NULL;
}

/*
 * Initialize garbage collector
 */
int lsfs_gc_init(struct lsfs_context *ctx)
{
    ctx->gc_running = true;

    if (pthread_mutex_init(&ctx->gc_lock, NULL) != 0) {
        return LSFS_ERR_NOMEM;
    }

    if (pthread_cond_init(&ctx->gc_cond, NULL) != 0) {
        pthread_mutex_destroy(&ctx->gc_lock);
        return LSFS_ERR_NOMEM;
    }

    if (pthread_create(&ctx->gc_thread, NULL, gc_thread_func, ctx) != 0) {
        pthread_cond_destroy(&ctx->gc_cond);
        pthread_mutex_destroy(&ctx->gc_lock);
        return LSFS_ERR_NOMEM;
    }

    return LSFS_OK;
}

/*
 * Destroy garbage collector
 */
void lsfs_gc_destroy(struct lsfs_context *ctx)
{
    ctx->gc_running = false;

    pthread_mutex_lock(&ctx->gc_lock);
    pthread_cond_signal(&ctx->gc_cond);
    pthread_mutex_unlock(&ctx->gc_lock);

    pthread_join(ctx->gc_thread, NULL);

    pthread_cond_destroy(&ctx->gc_cond);
    pthread_mutex_destroy(&ctx->gc_lock);
}

/*
 * Check if GC is needed
 */
bool lsfs_gc_needed(struct lsfs_context *ctx)
{
    struct lsfs_segment_table *table = &ctx->segtable;
    uint32_t free_percent;

    pthread_mutex_lock(&table->lock);
    free_percent = (table->free_count * 100) / table->count;
    pthread_mutex_unlock(&table->lock);

    return free_percent < GC_THRESHOLD_LOW;
}

/*
 * Trigger GC
 */
void lsfs_gc_trigger(struct lsfs_context *ctx)
{
    pthread_mutex_lock(&ctx->gc_lock);
    pthread_cond_signal(&ctx->gc_cond);
    pthread_mutex_unlock(&ctx->gc_lock);
}

/*
 * Calculate segment utility (higher = better candidate for cleaning)
 */
static double segment_utility(struct lsfs_segment_usage *seg, uint64_t now)
{
    if (seg->state != LSFS_SEG_FULL) {
        return -1.0;  /* Not a candidate */
    }

    double utilization = (double)seg->live_blocks / (double)(LSFS_SEGMENT_BLOCKS - 1);
    double age = (double)(now - seg->timestamp);

    if (utilization >= 1.0) {
        return -1.0;  /* Fully live, don't clean */
    }

    /* Cost-benefit formula: older and emptier segments are better candidates */
    return (age * (1.0 - utilization)) / (1.0 + utilization);
}

/*
 * Select the best segment to clean
 */
uint32_t lsfs_gc_select_segment(struct lsfs_context *ctx)
{
    struct lsfs_segment_table *table = &ctx->segtable;
    uint64_t now = (uint64_t)time(NULL);
    uint32_t best_segment = UINT32_MAX;
    double best_utility = -1.0;

    pthread_mutex_lock(&table->lock);

    for (uint32_t i = 0; i < table->count; i++) {
        struct lsfs_segment_usage *seg = &table->entries[i];

        /* Skip non-full segments */
        if (seg->state != LSFS_SEG_FULL) {
            continue;
        }

        /* Skip segments with high utilization */
        uint32_t utilization = (seg->live_blocks * 100) / (LSFS_SEGMENT_BLOCKS - 1);
        if (utilization > GC_UTILIZATION_THRESHOLD) {
            continue;
        }

        double utility = segment_utility(seg, now);
        if (utility > best_utility) {
            best_utility = utility;
            best_segment = i;
        }
    }

    pthread_mutex_unlock(&table->lock);

    return best_segment;
}

/*
 * Mark a block as dead (for GC tracking)
 */
void lsfs_gc_mark_block_dead(struct lsfs_context *ctx, uint64_t block)
{
    uint32_t segment_id, offset;
    lsfs_block_to_segment(block, &segment_id, &offset);

    if (segment_id >= ctx->segtable.count) {
        return;
    }

    pthread_mutex_lock(&ctx->segtable.lock);

    if (ctx->segtable.entries[segment_id].live_blocks > 0) {
        ctx->segtable.entries[segment_id].live_blocks--;
    }

    pthread_mutex_unlock(&ctx->segtable.lock);

    LSFS_DEBUG("Marked block %" PRIu64 " as dead (segment %u)", block, segment_id);
}

/*
 * Clean a single segment
 */
int lsfs_gc_clean_segment(struct lsfs_context *ctx, uint32_t segment_id)
{
    struct lsfs_segment_table *table = &ctx->segtable;
    uint8_t *segment_data;
    int ret = LSFS_OK;

    if (segment_id >= table->count) {
        return LSFS_ERR_INVAL;
    }

    pthread_mutex_lock(&table->lock);

    /* Check if segment is still a good candidate */
    if (table->entries[segment_id].state != LSFS_SEG_FULL) {
        pthread_mutex_unlock(&table->lock);
        return LSFS_OK;  /* Already cleaned or active */
    }

    uint32_t live_blocks = table->entries[segment_id].live_blocks;
    if (live_blocks == 0) {
        /* No live data, just free the segment */
        table->entries[segment_id].state = LSFS_SEG_FREE;
        table->free_count++;
        ctx->sb.free_segments = table->free_count;
        pthread_mutex_unlock(&table->lock);

        LSFS_DEBUG("Freed empty segment %u", segment_id);
        return LSFS_OK;
    }

    table->entries[segment_id].state = LSFS_SEG_CLEANING;
    pthread_mutex_unlock(&table->lock);

    LSFS_INFO("Cleaning segment %u (%u live blocks)", segment_id, live_blocks);

    /* Read entire segment */
    segment_data = malloc(LSFS_SEGMENT_SIZE);
    if (!segment_data) {
        pthread_mutex_lock(&table->lock);
        table->entries[segment_id].state = LSFS_SEG_FULL;
        pthread_mutex_unlock(&table->lock);
        return LSFS_ERR_NOMEM;
    }

    uint64_t seg_start = lsfs_segment_to_block(segment_id, 0);
    ret = lsfs_read_blocks(ctx, seg_start, LSFS_SEGMENT_BLOCKS, segment_data);
    if (ret != LSFS_OK) {
        free(segment_data);
        pthread_mutex_lock(&table->lock);
        table->entries[segment_id].state = LSFS_SEG_FULL;
        pthread_mutex_unlock(&table->lock);
        return ret;
    }

    /* Parse segment summary */
    struct lsfs_segment_summary *summary = (struct lsfs_segment_summary *)segment_data;

    if (summary->header.magic != LSFS_SEGMENT_MAGIC) {
        LSFS_ERROR("Invalid segment header in segment %u", segment_id);
        free(segment_data);
        pthread_mutex_lock(&table->lock);
        table->entries[segment_id].state = LSFS_SEG_FULL;
        pthread_mutex_unlock(&table->lock);
        return LSFS_ERR_CORRUPT;
    }

    /* Process each block in the segment */
    uint32_t num_blocks = summary->header.block_count;
    if (num_blocks > LSFS_SEGMENT_BLOCKS) {
        num_blocks = LSFS_SEGMENT_BLOCKS;
    }

    for (uint32_t i = 1; i < num_blocks; i++) {
        struct lsfs_block_info *info = &summary->blocks[i - 1];
        uint64_t current_loc;
        uint32_t version;

        /* Check if this block is still live */
        if (info->ino == 0) {
            continue;
        }

        /* For inode blocks, check if this is still the current location */
        if (info->type == LSFS_BLOCK_TYPE_INODE) {
            if (lsfs_imap_get(&ctx->imap, info->ino, &current_loc, &version) == LSFS_OK) {
                if (current_loc == seg_start + i) {
                    /* Still live, copy to new location */
                    uint8_t *block_data = segment_data + i * LSFS_BLOCK_SIZE;
                    uint64_t new_loc = lsfs_segment_append_block(ctx, block_data,
                                                                  info->ino, info->offset,
                                                                  info->type);
                    if (new_loc == 0) {
                        LSFS_ERROR("Failed to relocate block during GC");
                        ret = LSFS_ERR_NOSPC;
                        break;
                    }

                    /* Update inode map */
                    lsfs_imap_set(&ctx->imap, info->ino, new_loc);

                    LSFS_DEBUG("Relocated inode %u from %" PRIu64 " to %" PRIu64,
                               info->ino, seg_start + i, new_loc);
                }
            }
        }
        /* For data blocks, need to check if inode still references this location */
        else if (info->type == LSFS_BLOCK_TYPE_DATA) {
            struct lsfs_inode_mem *inode = lsfs_inode_get(ctx, info->ino);
            if (inode) {
                bool is_live = false;

                /* Check if this block is still referenced */
                if (info->offset < LSFS_DIRECT_BLOCKS) {
                    if (inode->disk_inode.direct[info->offset] == seg_start + i) {
                        is_live = true;
                    }
                }
                /* TODO: Check indirect blocks */

                if (is_live) {
                    uint8_t *block_data = segment_data + i * LSFS_BLOCK_SIZE;
                    uint64_t new_loc = lsfs_segment_append_block(ctx, block_data,
                                                                  info->ino, info->offset,
                                                                  info->type);
                    if (new_loc == 0) {
                        lsfs_inode_put(inode);
                        ret = LSFS_ERR_NOSPC;
                        break;
                    }

                    /* Update inode block pointer */
                    if (info->offset < LSFS_DIRECT_BLOCKS) {
                        inode->disk_inode.direct[info->offset] = new_loc;
                        inode->dirty = true;
                    }

                    LSFS_DEBUG("Relocated data block (ino %u, off %u) from %" PRIu64 " to %" PRIu64,
                               info->ino, info->offset, seg_start + i, new_loc);
                }

                lsfs_inode_put(inode);
            }
        }
    }

    free(segment_data);

    /* Mark segment as free */
    pthread_mutex_lock(&table->lock);
    table->entries[segment_id].state = LSFS_SEG_FREE;
    table->entries[segment_id].live_blocks = 0;
    table->free_count++;
    ctx->sb.free_segments = table->free_count;
    pthread_mutex_unlock(&table->lock);

    LSFS_INFO("Cleaned segment %u", segment_id);

    return ret;
}

/*
 * Run garbage collection
 */
int lsfs_gc_run(struct lsfs_context *ctx)
{
    struct lsfs_segment_table *table = &ctx->segtable;
    int cleaned = 0;
    int ret = LSFS_OK;

    /* Clean segments until we have enough free space */
    while (1) {
        uint32_t free_percent;

        pthread_mutex_lock(&table->lock);
        free_percent = (table->free_count * 100) / table->count;
        pthread_mutex_unlock(&table->lock);

        if (free_percent >= GC_THRESHOLD_HIGH) {
            break;  /* Enough free space */
        }

        /* Select a segment to clean */
        uint32_t segment_id = lsfs_gc_select_segment(ctx);
        if (segment_id == UINT32_MAX) {
            LSFS_DEBUG("No suitable segments for GC");
            break;
        }

        /* Clean the segment */
        ret = lsfs_gc_clean_segment(ctx, segment_id);
        if (ret != LSFS_OK) {
            break;
        }

        cleaned++;

        /* Limit number of segments cleaned in one run */
        if (cleaned >= 5) {
            break;
        }
    }

    if (cleaned > 0) {
        LSFS_INFO("GC completed: cleaned %d segments", cleaned);

        /* Flush segment buffer */
        lsfs_segment_flush(ctx);

        /* Write checkpoint to persist inode map changes */
        lsfs_checkpoint_write(ctx);
    }

    return ret;
}
