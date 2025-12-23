/*
 * LSFS - Log-Structured Filesystem
 * Checkpoint Management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "lsfs.h"

#define CHECKPOINT_INTERVAL     100     /* Write checkpoint every N segment writes */
#define CHECKPOINT_TIME_SEC     30      /* Or every N seconds */

/*
 * Initialize checkpoint system
 */
int lsfs_checkpoint_init(struct lsfs_context *ctx)
{
    ctx->checkpoint_seq = 0;
    ctx->last_checkpoint = (uint64_t)time(NULL);
    ctx->writes_since_checkpoint = 0;

    return LSFS_OK;
}

/*
 * Check if a checkpoint is needed
 */
bool lsfs_checkpoint_needed(struct lsfs_context *ctx)
{
    uint64_t now = (uint64_t)time(NULL);

    if (ctx->writes_since_checkpoint >= CHECKPOINT_INTERVAL) {
        return true;
    }

    if (now - ctx->last_checkpoint >= CHECKPOINT_TIME_SEC) {
        return true;
    }

    return false;
}

/*
 * Write a checkpoint
 */
int lsfs_checkpoint_write(struct lsfs_context *ctx)
{
    struct lsfs_checkpoint_header header;
    uint64_t checkpoint_block;
    uint8_t *buf;
    int ret;

    pthread_mutex_lock(&ctx->write_lock);

    /* Flush any pending segment data */
    if (ctx->segbuf.block_count > 1) {
        pthread_mutex_unlock(&ctx->write_lock);
        ret = lsfs_segment_flush(ctx);
        if (ret != LSFS_OK) {
            return ret;
        }
        pthread_mutex_lock(&ctx->write_lock);
    }

    /* Determine which checkpoint region to use */
    uint32_t cp_region = ctx->sb.active_checkpoint ^ 1;  /* Alternate */
    checkpoint_block = (cp_region == 0) ? LSFS_CHECKPOINT0_START : LSFS_CHECKPOINT1_START;

    ctx->checkpoint_seq++;

    /* Prepare checkpoint header */
    memset(&header, 0, sizeof(header));
    header.magic = LSFS_CHECKPOINT_MAGIC;
    header.version = LSFS_VERSION;
    header.sequence = ctx->checkpoint_seq;
    header.timestamp = (uint64_t)time(NULL);
    header.log_head = ctx->sb.log_head;
    header.imap_entries = ctx->imap.count;
    header.segment_entries = ctx->segtable.count;
    header.checksum = 0;  /* TODO: Calculate CRC32 */
    header.complete = 0;  /* Will set to 1 when done */

    /* Allocate buffer for checkpoint data */
    buf = calloc(1, LSFS_BLOCK_SIZE);
    if (!buf) {
        pthread_mutex_unlock(&ctx->write_lock);
        return LSFS_ERR_NOMEM;
    }

    /* Write header */
    memcpy(buf, &header, sizeof(header));
    ret = lsfs_write_block(ctx, checkpoint_block, buf);
    if (ret != LSFS_OK) {
        free(buf);
        pthread_mutex_unlock(&ctx->write_lock);
        return ret;
    }

    /* Write inode map */
    ret = lsfs_imap_save(ctx, checkpoint_block + 1);
    if (ret != LSFS_OK) {
        free(buf);
        pthread_mutex_unlock(&ctx->write_lock);
        return ret;
    }

    /* Write segment usage table */
    size_t seg_table_size = ctx->segtable.count * sizeof(struct lsfs_segment_usage);
    uint32_t seg_blocks = LSFS_DIV_ROUND_UP(seg_table_size, LSFS_BLOCK_SIZE);
    uint64_t seg_start = LSFS_SEGTABLE_START;

    uint8_t *seg_buf = calloc(seg_blocks, LSFS_BLOCK_SIZE);
    if (seg_buf) {
        memcpy(seg_buf, ctx->segtable.entries, seg_table_size);
        lsfs_write_blocks(ctx, seg_start, seg_blocks, seg_buf);
        free(seg_buf);
    }

    /* Sync to disk */
    ret = lsfs_sync(ctx);
    if (ret != LSFS_OK) {
        free(buf);
        pthread_mutex_unlock(&ctx->write_lock);
        return ret;
    }

    /* Mark checkpoint as complete */
    header.complete = 1;
    memcpy(buf, &header, sizeof(header));
    ret = lsfs_write_block(ctx, checkpoint_block, buf);
    free(buf);

    if (ret != LSFS_OK) {
        pthread_mutex_unlock(&ctx->write_lock);
        return ret;
    }

    /* Update superblock */
    ctx->sb.active_checkpoint = cp_region;
    ret = lsfs_write_block(ctx, LSFS_SUPERBLOCK_BLOCK, &ctx->sb);
    if (ret != LSFS_OK) {
        pthread_mutex_unlock(&ctx->write_lock);
        return ret;
    }

    /* Final sync */
    lsfs_sync(ctx);

    ctx->last_checkpoint = header.timestamp;
    ctx->writes_since_checkpoint = 0;

    pthread_mutex_unlock(&ctx->write_lock);

    LSFS_INFO("Checkpoint %lu written to region %u",
              (unsigned long)ctx->checkpoint_seq, cp_region);

    return LSFS_OK;
}

/*
 * Load checkpoint from disk
 */
int lsfs_checkpoint_load(struct lsfs_context *ctx)
{
    struct lsfs_checkpoint_header header[2];
    uint64_t cp_blocks[2] = { LSFS_CHECKPOINT0_START, LSFS_CHECKPOINT1_START };
    int valid[2] = { 0, 0 };
    int best = -1;

    /* Read both checkpoint headers */
    for (int i = 0; i < 2; i++) {
        if (lsfs_read_block(ctx, cp_blocks[i], &header[i]) == LSFS_OK) {
            if (header[i].magic == LSFS_CHECKPOINT_MAGIC &&
                header[i].complete == 1) {
                valid[i] = 1;
            }
        }
    }

    /* Select the checkpoint with higher sequence number */
    if (valid[0] && valid[1]) {
        best = (header[0].sequence > header[1].sequence) ? 0 : 1;
    } else if (valid[0]) {
        best = 0;
    } else if (valid[1]) {
        best = 1;
    } else {
        LSFS_ERROR("No valid checkpoint found");
        return LSFS_ERR_CORRUPT;
    }

    LSFS_INFO("Loading checkpoint from region %d (seq %lu)",
              best, (unsigned long)header[best].sequence);

    /* Load inode map */
    int ret = lsfs_imap_load(ctx, cp_blocks[best] + 1, header[best].imap_entries);
    if (ret != LSFS_OK) {
        return ret;
    }

    /* Update context */
    ctx->checkpoint_seq = header[best].sequence;
    ctx->last_checkpoint = header[best].timestamp;
    ctx->sb.log_head = header[best].log_head;
    ctx->sb.active_checkpoint = best;

    return LSFS_OK;
}

/*
 * Recover from crash by replaying log
 */
int lsfs_checkpoint_recover(struct lsfs_context *ctx)
{
    int ret;

    /* Load last valid checkpoint */
    ret = lsfs_checkpoint_load(ctx);
    if (ret != LSFS_OK) {
        return ret;
    }

    /* Roll forward through any segments written after checkpoint */
    uint64_t log_head = ctx->sb.log_head;
    uint32_t segment_id, offset;
    lsfs_block_to_segment(log_head, &segment_id, &offset);

    LSFS_INFO("Rolling forward from block %" PRIu64 " (segment %u, offset %u)",
              log_head, segment_id, offset);

    /* Scan segments after checkpoint */
    for (uint32_t seg = segment_id; seg < ctx->sb.total_segments; seg++) {
        uint64_t seg_start = lsfs_segment_to_block(seg, 0);
        struct lsfs_segment_header seg_header;

        ret = lsfs_read_block(ctx, seg_start, &seg_header);
        if (ret != LSFS_OK) {
            break;
        }

        /* Check if this is a valid segment */
        if (seg_header.magic != LSFS_SEGMENT_MAGIC) {
            break;  /* End of log */
        }

        /* Verify timestamp is after checkpoint */
        if (seg_header.timestamp < ctx->last_checkpoint) {
            break;  /* Old segment, stop */
        }

        LSFS_DEBUG("Recovering segment %u (%u blocks)",
                   seg, seg_header.block_count);

        /* Read segment summary and update inode map */
        uint8_t summary_block[LSFS_BLOCK_SIZE];
        ret = lsfs_read_block(ctx, seg_start, summary_block);
        if (ret != LSFS_OK) {
            break;
        }

        struct lsfs_segment_summary *summary = (struct lsfs_segment_summary *)summary_block;
        uint32_t max_entries = (LSFS_BLOCK_SIZE - sizeof(struct lsfs_segment_header)) /
                               sizeof(struct lsfs_block_info);
        uint32_t num_entries = seg_header.block_count - 1;
        if (num_entries > max_entries) {
            num_entries = max_entries;
        }

        for (uint32_t i = 0; i < num_entries; i++) {
            struct lsfs_block_info *info = &summary->blocks[i];
            if (info->type == LSFS_BLOCK_TYPE_INODE && info->ino > 0) {
                uint64_t block_addr = seg_start + i + 1;
                lsfs_imap_set(&ctx->imap, info->ino, block_addr);
            }
        }

        /* Update log head */
        ctx->sb.log_head = seg_start + seg_header.block_count;

        /* Update segment table */
        pthread_mutex_lock(&ctx->segtable.lock);
        ctx->segtable.entries[seg].state = LSFS_SEG_FULL;
        ctx->segtable.entries[seg].live_blocks = seg_header.block_count - 1;
        ctx->segtable.entries[seg].timestamp = seg_header.timestamp;
        if (ctx->segtable.free_count > 0) {
            ctx->segtable.free_count--;
        }
        pthread_mutex_unlock(&ctx->segtable.lock);
    }

    LSFS_INFO("Recovery complete, log head at %" PRIu64, ctx->sb.log_head);

    /* Write a fresh checkpoint */
    return lsfs_checkpoint_write(ctx);
}
