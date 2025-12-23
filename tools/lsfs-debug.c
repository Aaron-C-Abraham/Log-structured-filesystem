/*
 * LSFS - Log-Structured Filesystem
 * lsfs-debug - Debug and inspection utility
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>

#include "ondisk.h"

static int g_fd = -1;

/*
 * Read a block
 */
static int read_block(uint64_t block_num, void *buf)
{
    off_t offset = (off_t)block_num * LSFS_BLOCK_SIZE;
    ssize_t ret = pread(g_fd, buf, LSFS_BLOCK_SIZE, offset);
    return (ret == LSFS_BLOCK_SIZE) ? 0 : -1;
}

/*
 * Format UUID
 */
static void format_uuid(const uint8_t uuid[16], char *str)
{
    sprintf(str, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3],
            uuid[4], uuid[5],
            uuid[6], uuid[7],
            uuid[8], uuid[9],
            uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

/*
 * Format timestamp
 */
static void format_time(uint64_t ts, char *str)
{
    time_t t = (time_t)ts;
    struct tm *tm = localtime(&t);
    strftime(str, 32, "%Y-%m-%d %H:%M:%S", tm);
}

/*
 * Dump superblock
 */
static void dump_superblock(void)
{
    struct lsfs_superblock sb;
    char uuid_str[40];
    char time_str[32];

    if (read_block(LSFS_SUPERBLOCK_BLOCK, &sb) < 0) {
        fprintf(stderr, "Failed to read superblock\n");
        return;
    }

    printf("=== SUPERBLOCK ===\n");
    printf("Magic:            0x%08X", sb.magic);
    if (sb.magic == LSFS_MAGIC) {
        printf(" (valid)\n");
    } else {
        printf(" (INVALID!)\n");
    }
    printf("Version:          %u\n", sb.version);
    printf("Block size:       %u bytes\n", sb.block_size);
    printf("Segment size:     %u blocks\n", sb.segment_size);
    printf("Total blocks:     %lu\n", (unsigned long)sb.total_blocks);
    printf("Total segments:   %lu\n", (unsigned long)sb.total_segments);
    printf("Inode count:      %lu\n", (unsigned long)sb.inode_count);
    printf("Free segments:    %lu\n", (unsigned long)sb.free_segments);
    printf("Active checkpoint: %u\n", sb.active_checkpoint);
    printf("Log head:         %lu\n", (unsigned long)sb.log_head);

    format_uuid(sb.uuid, uuid_str);
    printf("UUID:             %s\n", uuid_str);

    format_time(sb.created_at, time_str);
    printf("Created:          %s\n", time_str);

    if (sb.mounted_at > 0) {
        format_time(sb.mounted_at, time_str);
        printf("Last mounted:     %s\n", time_str);
    }

    printf("Mount count:      %u\n", sb.mount_count);
    printf("State:            %s\n", sb.state ? "dirty" : "clean");
    printf("\n");
}

/*
 * Dump checkpoint
 */
static void dump_checkpoint(int which)
{
    struct lsfs_checkpoint_header cp;
    uint8_t block[LSFS_BLOCK_SIZE];
    uint64_t cp_block;
    char time_str[32];

    cp_block = (which == 0) ? LSFS_CHECKPOINT0_START : LSFS_CHECKPOINT1_START;

    if (read_block(cp_block, block) < 0) {
        fprintf(stderr, "Failed to read checkpoint %d\n", which);
        return;
    }

    memcpy(&cp, block, sizeof(cp));

    printf("=== CHECKPOINT %d ===\n", which);
    printf("Magic:            0x%08X", cp.magic);
    if (cp.magic == LSFS_CHECKPOINT_MAGIC) {
        printf(" (valid)\n");
    } else {
        printf(" (INVALID!)\n");
        return;
    }

    printf("Version:          %u\n", cp.version);
    printf("Sequence:         %lu\n", (unsigned long)cp.sequence);

    format_time(cp.timestamp, time_str);
    printf("Timestamp:        %s\n", time_str);

    printf("Log head:         %lu\n", (unsigned long)cp.log_head);
    printf("Imap entries:     %u\n", cp.imap_entries);
    printf("Segment entries:  %u\n", cp.segment_entries);
    printf("Complete:         %s\n", cp.complete ? "yes" : "no");
    printf("\n");
}

/*
 * Dump inode
 */
static void dump_inode(uint64_t block_num, uint32_t offset)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    struct lsfs_inode *inode;
    char time_str[32];

    if (read_block(block_num, block) < 0) {
        fprintf(stderr, "Failed to read block %lu\n", (unsigned long)block_num);
        return;
    }

    inode = (struct lsfs_inode *)(block + offset * sizeof(struct lsfs_inode));

    printf("=== INODE %u ===\n", inode->ino);
    printf("Mode:             0%o", inode->mode);

    switch (inode->mode & S_IFMT) {
    case S_IFREG: printf(" (regular file)\n"); break;
    case S_IFDIR: printf(" (directory)\n"); break;
    case S_IFLNK: printf(" (symlink)\n"); break;
    default: printf(" (other)\n"); break;
    }

    printf("UID/GID:          %u/%u\n", inode->uid, inode->gid);
    printf("Size:             %lu bytes\n", (unsigned long)inode->size);
    printf("Blocks:           %lu\n", (unsigned long)inode->blocks);
    printf("Links:            %u\n", inode->nlink);

    format_time(inode->atime / 1000000000ULL, time_str);
    printf("Access time:      %s\n", time_str);

    format_time(inode->mtime / 1000000000ULL, time_str);
    printf("Modify time:      %s\n", time_str);

    format_time(inode->ctime / 1000000000ULL, time_str);
    printf("Change time:      %s\n", time_str);

    printf("Direct blocks:    ");
    for (int i = 0; i < LSFS_DIRECT_BLOCKS; i++) {
        if (inode->direct[i]) {
            printf("%lu ", (unsigned long)inode->direct[i]);
        }
    }
    printf("\n");

    if (inode->indirect) {
        printf("Indirect:         %lu\n", (unsigned long)inode->indirect);
    }
    if (inode->double_indirect) {
        printf("Double indirect:  %lu\n", (unsigned long)inode->double_indirect);
    }

    if ((inode->mode & S_IFMT) == S_IFLNK && inode->symlink[0]) {
        printf("Symlink target:   %s\n", inode->symlink);
    }

    printf("\n");
}

/*
 * Dump segment header
 */
static void dump_segment(uint32_t segment_id)
{
    uint8_t block[LSFS_BLOCK_SIZE];
    uint64_t seg_start = LSFS_LOG_START + (uint64_t)segment_id * LSFS_SEGMENT_BLOCKS;
    struct lsfs_segment_summary *summary;
    char time_str[32];

    if (read_block(seg_start, block) < 0) {
        fprintf(stderr, "Failed to read segment %u\n", segment_id);
        return;
    }

    summary = (struct lsfs_segment_summary *)block;

    printf("=== SEGMENT %u ===\n", segment_id);
    printf("Start block:      %lu\n", (unsigned long)seg_start);
    printf("Magic:            0x%08X", summary->header.magic);

    if (summary->header.magic == LSFS_SEGMENT_MAGIC) {
        printf(" (valid)\n");
    } else if (summary->header.magic == 0) {
        printf(" (free)\n");
        return;
    } else {
        printf(" (INVALID!)\n");
        return;
    }

    printf("Segment ID:       %u\n", summary->header.segment_id);

    format_time(summary->header.timestamp, time_str);
    printf("Timestamp:        %s\n", time_str);

    printf("Block count:      %u\n", summary->header.block_count);

    printf("Block contents:\n");
    uint32_t max_entries = (LSFS_BLOCK_SIZE - sizeof(struct lsfs_segment_header)) /
                           sizeof(struct lsfs_block_info);
    uint32_t num_entries = summary->header.block_count - 1;
    if (num_entries > max_entries) {
        num_entries = max_entries;
    }

    for (uint32_t i = 0; i < num_entries && i < 10; i++) {
        struct lsfs_block_info *info = &summary->blocks[i];
        const char *type_str;

        switch (info->type) {
        case LSFS_BLOCK_TYPE_DATA: type_str = "data"; break;
        case LSFS_BLOCK_TYPE_INODE: type_str = "inode"; break;
        case LSFS_BLOCK_TYPE_INDIRECT: type_str = "indirect"; break;
        case LSFS_BLOCK_TYPE_DIRENT: type_str = "dirent"; break;
        default: type_str = "unknown"; break;
        }

        printf("  Block %u: ino=%u offset=%u type=%s\n",
               i + 1, info->ino, info->offset, type_str);
    }

    if (num_entries > 10) {
        printf("  ... and %u more blocks\n", num_entries - 10);
    }

    printf("\n");
}

/*
 * Dump inode map
 */
static void dump_imap(void)
{
    struct lsfs_superblock sb;
    struct lsfs_checkpoint_header cp;
    uint8_t block[LSFS_BLOCK_SIZE];
    uint64_t cp_block;

    if (read_block(LSFS_SUPERBLOCK_BLOCK, &sb) < 0) {
        fprintf(stderr, "Failed to read superblock\n");
        return;
    }

    cp_block = (sb.active_checkpoint == 0) ?
               LSFS_CHECKPOINT0_START : LSFS_CHECKPOINT1_START;

    if (read_block(cp_block, block) < 0) {
        fprintf(stderr, "Failed to read checkpoint\n");
        return;
    }
    memcpy(&cp, block, sizeof(cp));

    printf("=== INODE MAP ===\n");
    printf("Entries: %u\n\n", cp.imap_entries);

    uint32_t entries_per_block = LSFS_BLOCK_SIZE / sizeof(struct lsfs_imap_entry);
    uint32_t blocks_needed = (cp.imap_entries + entries_per_block - 1) / entries_per_block;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        if (read_block(cp_block + 1 + b, block) < 0) {
            continue;
        }

        struct lsfs_imap_entry *entries = (struct lsfs_imap_entry *)block;
        uint32_t entries_in_block = (b == blocks_needed - 1) ?
                                    (cp.imap_entries - b * entries_per_block) :
                                    entries_per_block;

        for (uint32_t i = 0; i < entries_in_block; i++) {
            struct lsfs_imap_entry *entry = &entries[i];
            if (entry->ino > 0) {
                printf("  Inode %u: block %lu, version %u\n",
                       entry->ino,
                       (unsigned long)entry->location,
                       entry->version);
            }
        }
    }

    printf("\n");
}

/*
 * Print usage
 */
static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s <disk_image> <command> [args]\n\n", progname);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  superblock              Dump superblock\n");
    fprintf(stderr, "  checkpoint [0|1]        Dump checkpoint (default: both)\n");
    fprintf(stderr, "  segment <id>            Dump segment header\n");
    fprintf(stderr, "  inode <block> [offset]  Dump inode at block (offset 0-15)\n");
    fprintf(stderr, "  imap                    Dump inode map\n");
    fprintf(stderr, "  all                     Dump all structures\n");
}

int main(int argc, char *argv[])
{
    char *path;
    char *command;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    path = argv[1];
    command = argv[2];

    g_fd = open(path, O_RDONLY);
    if (g_fd < 0) {
        perror("Failed to open filesystem");
        return 1;
    }

    if (strcmp(command, "superblock") == 0) {
        dump_superblock();
    } else if (strcmp(command, "checkpoint") == 0) {
        if (argc > 3) {
            int which = atoi(argv[3]);
            dump_checkpoint(which);
        } else {
            dump_checkpoint(0);
            dump_checkpoint(1);
        }
    } else if (strcmp(command, "segment") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s %s segment <id>\n", argv[0], path);
            close(g_fd);
            return 1;
        }
        dump_segment(atoi(argv[3]));
    } else if (strcmp(command, "inode") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s %s inode <block> [offset]\n", argv[0], path);
            close(g_fd);
            return 1;
        }
        uint64_t block = strtoull(argv[3], NULL, 10);
        uint32_t offset = (argc > 4) ? atoi(argv[4]) : 0;
        dump_inode(block, offset);
    } else if (strcmp(command, "imap") == 0) {
        dump_imap();
    } else if (strcmp(command, "all") == 0) {
        dump_superblock();
        dump_checkpoint(0);
        dump_checkpoint(1);
        dump_imap();
        dump_segment(0);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        usage(argv[0]);
        close(g_fd);
        return 1;
    }

    close(g_fd);
    return 0;
}
