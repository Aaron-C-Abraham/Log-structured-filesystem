/*
 * LSFS - Log-Structured Filesystem
 * fsck.lsfs - Filesystem check and repair utility
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

#include "ondisk.h"

struct fsck_context {
    int fd;
    uint64_t size;
    struct lsfs_superblock sb;
    int errors;
    int warnings;
    int repair;
    int verbose;
};

/*
 * Read a block
 */
static int read_block(struct fsck_context *ctx, uint64_t block_num, void *buf)
{
    off_t offset = (off_t)block_num * LSFS_BLOCK_SIZE;
    ssize_t ret = pread(ctx->fd, buf, LSFS_BLOCK_SIZE, offset);
    return (ret == LSFS_BLOCK_SIZE) ? 0 : -1;
}

/*
 * Write a block (for repairs)
 */
static int write_block(struct fsck_context *ctx, uint64_t block_num, const void *buf)
{
    off_t offset = (off_t)block_num * LSFS_BLOCK_SIZE;
    ssize_t ret = pwrite(ctx->fd, buf, LSFS_BLOCK_SIZE, offset);
    return (ret == LSFS_BLOCK_SIZE) ? 0 : -1;
}

/*
 * Check superblock
 */
static int check_superblock(struct fsck_context *ctx)
{
    printf("Checking superblock...\n");

    if (read_block(ctx, LSFS_SUPERBLOCK_BLOCK, &ctx->sb) < 0) {
        fprintf(stderr, "ERROR: Cannot read superblock\n");
        ctx->errors++;
        return -1;
    }

    if (ctx->sb.magic != LSFS_MAGIC) {
        fprintf(stderr, "ERROR: Invalid magic number: 0x%08x (expected 0x%08x)\n",
                ctx->sb.magic, LSFS_MAGIC);
        ctx->errors++;
        return -1;
    }

    if (ctx->sb.version != LSFS_VERSION) {
        fprintf(stderr, "ERROR: Unsupported version: %u\n", ctx->sb.version);
        ctx->errors++;
        return -1;
    }

    if (ctx->sb.block_size != LSFS_BLOCK_SIZE) {
        fprintf(stderr, "ERROR: Invalid block size: %u\n", ctx->sb.block_size);
        ctx->errors++;
        return -1;
    }

    if (ctx->sb.segment_size != LSFS_SEGMENT_BLOCKS) {
        fprintf(stderr, "ERROR: Invalid segment size: %u\n", ctx->sb.segment_size);
        ctx->errors++;
        return -1;
    }

    uint64_t expected_blocks = ctx->size / LSFS_BLOCK_SIZE;
    if (ctx->sb.total_blocks > expected_blocks) {
        fprintf(stderr, "WARNING: Superblock claims more blocks than file size\n");
        ctx->warnings++;
    }

    if (ctx->sb.state != 0) {
        fprintf(stderr, "WARNING: Filesystem was not cleanly unmounted\n");
        ctx->warnings++;
    }

    if (ctx->verbose) {
        printf("  Version: %u\n", ctx->sb.version);
        printf("  Total blocks: %lu\n", (unsigned long)ctx->sb.total_blocks);
        printf("  Total segments: %lu\n", (unsigned long)ctx->sb.total_segments);
        printf("  Inode count: %lu\n", (unsigned long)ctx->sb.inode_count);
        printf("  Free segments: %lu\n", (unsigned long)ctx->sb.free_segments);
        printf("  Active checkpoint: %u\n", ctx->sb.active_checkpoint);
        printf("  Log head: %lu\n", (unsigned long)ctx->sb.log_head);
    }

    return 0;
}

/*
 * Check checkpoints
 */
static int check_checkpoints(struct fsck_context *ctx)
{
    struct lsfs_checkpoint_header cp[2];
    uint8_t block[LSFS_BLOCK_SIZE];
    int valid[2] = {0, 0};

    printf("Checking checkpoints...\n");

    /* Read checkpoint 0 */
    if (read_block(ctx, LSFS_CHECKPOINT0_START, block) == 0) {
        memcpy(&cp[0], block, sizeof(cp[0]));
        if (cp[0].magic == LSFS_CHECKPOINT_MAGIC && cp[0].complete == 1) {
            valid[0] = 1;
            if (ctx->verbose) {
                printf("  Checkpoint 0: sequence %lu, timestamp %lu\n",
                       (unsigned long)cp[0].sequence,
                       (unsigned long)cp[0].timestamp);
            }
        }
    }

    /* Read checkpoint 1 */
    if (read_block(ctx, LSFS_CHECKPOINT1_START, block) == 0) {
        memcpy(&cp[1], block, sizeof(cp[1]));
        if (cp[1].magic == LSFS_CHECKPOINT_MAGIC && cp[1].complete == 1) {
            valid[1] = 1;
            if (ctx->verbose) {
                printf("  Checkpoint 1: sequence %lu, timestamp %lu\n",
                       (unsigned long)cp[1].sequence,
                       (unsigned long)cp[1].timestamp);
            }
        }
    }

    if (!valid[0] && !valid[1]) {
        fprintf(stderr, "ERROR: No valid checkpoints found\n");
        ctx->errors++;
        return -1;
    }

    if (!valid[ctx->sb.active_checkpoint]) {
        fprintf(stderr, "WARNING: Active checkpoint %u is invalid\n",
                ctx->sb.active_checkpoint);
        ctx->warnings++;

        if (ctx->repair) {
            /* Switch to other checkpoint */
            ctx->sb.active_checkpoint ^= 1;
            write_block(ctx, LSFS_SUPERBLOCK_BLOCK, &ctx->sb);
            printf("  REPAIRED: Switched to checkpoint %u\n",
                   ctx->sb.active_checkpoint);
        }
    }

    return 0;
}

/*
 * Check segments
 */
static int check_segments(struct fsck_context *ctx)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    uint32_t valid_segments = 0;
    uint32_t free_segments = 0;

    printf("Checking segments...\n");

    for (uint64_t seg = 0; seg < ctx->sb.total_segments; seg++) {
        uint64_t seg_start = LSFS_LOG_START + seg * LSFS_SEGMENT_BLOCKS;

        if (read_block(ctx, seg_start, block) < 0) {
            continue;
        }

        struct lsfs_segment_header *header = (struct lsfs_segment_header *)block;

        if (header->magic == LSFS_SEGMENT_MAGIC) {
            valid_segments++;

            if (header->segment_id != seg) {
                fprintf(stderr, "WARNING: Segment %lu has wrong ID %u\n",
                        (unsigned long)seg, header->segment_id);
                ctx->warnings++;
            }

            if (header->block_count > LSFS_SEGMENT_BLOCKS) {
                fprintf(stderr, "ERROR: Segment %lu has invalid block count %u\n",
                        (unsigned long)seg, header->block_count);
                ctx->errors++;
            }
        } else if (header->magic == 0) {
            free_segments++;
        }
    }

    if (ctx->verbose) {
        printf("  Valid segments: %u\n", valid_segments);
        printf("  Free segments: %u\n", free_segments);
    }

    if (free_segments != ctx->sb.free_segments) {
        fprintf(stderr, "WARNING: Free segment count mismatch: sb=%lu, actual=%u\n",
                (unsigned long)ctx->sb.free_segments, free_segments);
        ctx->warnings++;

        if (ctx->repair) {
            ctx->sb.free_segments = free_segments;
            write_block(ctx, LSFS_SUPERBLOCK_BLOCK, &ctx->sb);
            printf("  REPAIRED: Updated free segment count\n");
        }
    }

    return 0;
}

/*
 * Check inode map
 */
static int check_inode_map(struct fsck_context *ctx)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    struct lsfs_checkpoint_header cp;
    uint64_t cp_block;
    uint32_t valid_inodes = 0;

    printf("Checking inode map...\n");

    /* Read active checkpoint header */
    cp_block = (ctx->sb.active_checkpoint == 0) ?
               LSFS_CHECKPOINT0_START : LSFS_CHECKPOINT1_START;

    if (read_block(ctx, cp_block, block) < 0) {
        fprintf(stderr, "ERROR: Cannot read checkpoint header\n");
        ctx->errors++;
        return -1;
    }

    memcpy(&cp, block, sizeof(cp));

    if (ctx->verbose) {
        printf("  Inode map entries: %u\n", cp.imap_entries);
    }

    /* Read and validate inode map entries */
    uint32_t entries_per_block = LSFS_BLOCK_SIZE / sizeof(struct lsfs_imap_entry);
    uint32_t blocks_needed = (cp.imap_entries + entries_per_block - 1) / entries_per_block;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        if (read_block(ctx, cp_block + 1 + b, block) < 0) {
            fprintf(stderr, "ERROR: Cannot read inode map block %u\n", b);
            ctx->errors++;
            continue;
        }

        struct lsfs_imap_entry *entries = (struct lsfs_imap_entry *)block;
        uint32_t entries_in_block = (b == blocks_needed - 1) ?
                                    (cp.imap_entries - b * entries_per_block) :
                                    entries_per_block;

        for (uint32_t i = 0; i < entries_in_block; i++) {
            struct lsfs_imap_entry *entry = &entries[i];

            if (entry->ino == 0) {
                continue;
            }

            if (entry->location < LSFS_LOG_START ||
                entry->location >= ctx->sb.total_blocks) {
                fprintf(stderr, "ERROR: Inode %u has invalid location %lu\n",
                        entry->ino, (unsigned long)entry->location);
                ctx->errors++;
                continue;
            }

            valid_inodes++;
        }
    }

    if (ctx->verbose) {
        printf("  Valid inodes: %u\n", valid_inodes);
    }

    return 0;
}

/*
 * Check root directory
 */
static int check_root(struct fsck_context *ctx)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    struct lsfs_inode *root;

    printf("Checking root directory...\n");

    /* Find root inode location from checkpoint */
    uint64_t cp_block = (ctx->sb.active_checkpoint == 0) ?
                        LSFS_CHECKPOINT0_START : LSFS_CHECKPOINT1_START;

    struct lsfs_checkpoint_header cp;
    if (read_block(ctx, cp_block, block) < 0) {
        ctx->errors++;
        return -1;
    }
    memcpy(&cp, block, sizeof(cp));

    /* Read first inode map block */
    if (read_block(ctx, cp_block + 1, block) < 0) {
        ctx->errors++;
        return -1;
    }

    struct lsfs_imap_entry *entries = (struct lsfs_imap_entry *)block;
    uint64_t root_location = 0;

    for (uint32_t i = 0; i < cp.imap_entries; i++) {
        if (entries[i].ino == LSFS_ROOT_INO) {
            root_location = entries[i].location;
            break;
        }
    }

    if (root_location == 0) {
        fprintf(stderr, "ERROR: Root inode not found in inode map\n");
        ctx->errors++;
        return -1;
    }

    /* Read root inode */
    if (read_block(ctx, root_location, block) < 0) {
        fprintf(stderr, "ERROR: Cannot read root inode\n");
        ctx->errors++;
        return -1;
    }

    root = (struct lsfs_inode *)block;

    if (root->ino != LSFS_ROOT_INO) {
        fprintf(stderr, "ERROR: Root inode number mismatch: %u\n", root->ino);
        ctx->errors++;
        return -1;
    }

    if ((root->mode & S_IFMT) != S_IFDIR) {
        fprintf(stderr, "ERROR: Root is not a directory\n");
        ctx->errors++;
        return -1;
    }

    if (ctx->verbose) {
        printf("  Root inode: %u\n", root->ino);
        printf("  Root mode: 0%o\n", root->mode);
        printf("  Root size: %lu\n", (unsigned long)root->size);
        printf("  Root links: %u\n", root->nlink);
    }

    return 0;
}

/*
 * Run filesystem check
 */
static int run_fsck(const char *path, int repair, int verbose)
{
    struct fsck_context ctx;
    struct stat st;
    int ret = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.repair = repair;
    ctx.verbose = verbose;

    /* Open file */
    ctx.fd = open(path, repair ? O_RDWR : O_RDONLY);
    if (ctx.fd < 0) {
        perror("Failed to open filesystem");
        return 1;
    }

    /* Get file size */
    if (fstat(ctx.fd, &st) < 0) {
        perror("Failed to stat filesystem");
        close(ctx.fd);
        return 1;
    }
    ctx.size = st.st_size;

    printf("Checking LSFS filesystem: %s (%lu MB)\n\n",
           path, (unsigned long)(ctx.size / (1024 * 1024)));

    /* Run checks */
    if (check_superblock(&ctx) < 0) {
        ret = 1;
        goto done;
    }

    if (check_checkpoints(&ctx) < 0) {
        ret = 1;
        goto done;
    }

    check_segments(&ctx);
    check_inode_map(&ctx);
    check_root(&ctx);

done:
    if (ctx.repair && (ctx.errors > 0 || ctx.warnings > 0)) {
        fsync(ctx.fd);
    }

    close(ctx.fd);

    printf("\n");
    printf("Filesystem check complete.\n");
    printf("  Errors: %d\n", ctx.errors);
    printf("  Warnings: %d\n", ctx.warnings);

    if (ctx.errors > 0) {
        return 1;
    }

    return 0;
}

/*
 * Print usage
 */
static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] <disk_image>\n\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -r, --repair        Attempt to repair errors\n");
    fprintf(stderr, "  -v, --verbose       Verbose output\n");
    fprintf(stderr, "  -h, --help          Show this help\n");
}

int main(int argc, char *argv[])
{
    int repair = 0;
    int verbose = 0;
    char *path = NULL;
    int opt;

    static struct option long_options[] = {
        {"repair", no_argument, NULL, 'r'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "rvh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'r':
            repair = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    path = argv[optind];

    return run_fsck(path, repair, verbose);
}
