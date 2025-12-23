/*
 * LSFS - Log-Structured Filesystem
 * mkfs.lsfs - Format utility
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/random.h>

#include "ondisk.h"

#define DEFAULT_SIZE_MB     256

/*
 * Generate UUID
 */
static void generate_uuid(uint8_t uuid[16])
{
    if (getrandom(uuid, 16, 0) != 16) {
        /* Fallback to time-based */
        srand((unsigned)time(NULL) ^ getpid());
        for (int i = 0; i < 16; i++) {
            uuid[i] = (uint8_t)rand();
        }
    }

    /* Set version (4) and variant (RFC 4122) */
    uuid[6] = (uuid[6] & 0x0f) | 0x40;
    uuid[8] = (uuid[8] & 0x3f) | 0x80;
}

/*
 * Format UUID as string
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
 * Write a block
 */
static int write_block(int fd, uint64_t block_num, const void *buf)
{
    off_t offset = (off_t)block_num * LSFS_BLOCK_SIZE;
    ssize_t ret = pwrite(fd, buf, LSFS_BLOCK_SIZE, offset);
    return (ret == LSFS_BLOCK_SIZE) ? 0 : -1;
}

/*
 * Create and format filesystem
 */
static int format_filesystem(const char *path, uint64_t size_bytes)
{
    int fd;
    struct lsfs_superblock sb;
    struct lsfs_checkpoint_header cp;
    struct lsfs_inode root_inode;
    struct lsfs_imap_entry root_imap;
    struct lsfs_segment_header seg_header;
    uint8_t block[LSFS_BLOCK_SIZE];
    uint8_t dir_block[LSFS_BLOCK_SIZE];
    char uuid_str[40];
    uint64_t now;

    /* Calculate filesystem parameters */
    uint64_t total_blocks = size_bytes / LSFS_BLOCK_SIZE;
    uint64_t total_segments = (total_blocks - LSFS_LOG_START) / LSFS_SEGMENT_BLOCKS;

    if (total_segments < 4) {
        fprintf(stderr, "Error: Filesystem too small, need at least 4 segments\n");
        return -1;
    }

    if (total_segments > LSFS_MAX_SEGMENTS) {
        total_segments = LSFS_MAX_SEGMENTS;
        total_blocks = LSFS_LOG_START + total_segments * LSFS_SEGMENT_BLOCKS;
        size_bytes = total_blocks * LSFS_BLOCK_SIZE;
    }

    printf("Creating LSFS filesystem:\n");
    printf("  Size: %lu MB\n", (unsigned long)(size_bytes / (1024 * 1024)));
    printf("  Blocks: %lu\n", (unsigned long)total_blocks);
    printf("  Segments: %lu\n", (unsigned long)total_segments);

    /* Create/open file */
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to create disk image");
        return -1;
    }

    /* Extend file to desired size */
    if (ftruncate(fd, size_bytes) < 0) {
        perror("Failed to set file size");
        close(fd);
        return -1;
    }

    now = (uint64_t)time(NULL);

    /* Initialize superblock */
    memset(&sb, 0, sizeof(sb));
    sb.magic = LSFS_MAGIC;
    sb.version = LSFS_VERSION;
    sb.block_size = LSFS_BLOCK_SIZE;
    sb.segment_size = LSFS_SEGMENT_BLOCKS;
    sb.total_blocks = total_blocks;
    sb.total_segments = total_segments;
    sb.inode_count = 1;  /* Root inode */
    sb.checkpoint_region[0] = LSFS_CHECKPOINT0_START;
    sb.checkpoint_region[1] = LSFS_CHECKPOINT1_START;
    sb.active_checkpoint = 0;
    sb.log_head = LSFS_LOG_START + 2;  /* After first segment header and root inode */
    sb.free_segments = total_segments - 1;  /* First segment used */
    generate_uuid(sb.uuid);
    sb.created_at = now;
    sb.mounted_at = 0;
    sb.mount_count = 0;
    sb.state = 0;  /* Clean */

    format_uuid(sb.uuid, uuid_str);
    printf("  UUID: %s\n", uuid_str);

    /* Write superblock */
    if (write_block(fd, LSFS_SUPERBLOCK_BLOCK, &sb) < 0) {
        fprintf(stderr, "Failed to write superblock\n");
        close(fd);
        return -1;
    }

    /* Initialize root inode */
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.ino = LSFS_ROOT_INO;
    root_inode.mode = S_IFDIR | 0755;
    root_inode.uid = getuid();
    root_inode.gid = getgid();
    root_inode.size = LSFS_BLOCK_SIZE;
    root_inode.blocks = 1;
    root_inode.atime = now * 1000000000ULL;
    root_inode.mtime = root_inode.atime;
    root_inode.ctime = root_inode.atime;
    root_inode.nlink = 2;  /* . and from parent (root's parent is itself) */
    root_inode.flags = 0;
    root_inode.direct[0] = LSFS_LOG_START + 2;  /* First data block after header and inode */
    root_inode.generation = (uint64_t)rand();

    /* Create root directory content */
    memset(dir_block, 0, LSFS_BLOCK_SIZE);

    /* . entry */
    struct lsfs_dirent *de = (struct lsfs_dirent *)dir_block;
    de->ino = LSFS_ROOT_INO;
    de->rec_len = 12;  /* 8 + 1 + 3 padding */
    de->name_len = 1;
    de->file_type = LSFS_FT_DIR;
    de->name[0] = '.';

    /* .. entry */
    de = (struct lsfs_dirent *)(dir_block + 12);
    de->ino = LSFS_ROOT_INO;
    de->rec_len = LSFS_BLOCK_SIZE - 12;
    de->name_len = 2;
    de->file_type = LSFS_FT_DIR;
    de->name[0] = '.';
    de->name[1] = '.';

    /* Write first segment with root inode and directory */
    /* Segment header */
    memset(block, 0, LSFS_BLOCK_SIZE);
    struct lsfs_segment_summary *summary = (struct lsfs_segment_summary *)block;
    summary->header.magic = LSFS_SEGMENT_MAGIC;
    summary->header.segment_id = 0;
    summary->header.timestamp = now;
    summary->header.block_count = 3;  /* Header + inode + dir data */
    summary->header.checksum = 0;

    /* Block info for inode */
    summary->blocks[0].ino = LSFS_ROOT_INO;
    summary->blocks[0].offset = 0;
    summary->blocks[0].type = LSFS_BLOCK_TYPE_INODE;

    /* Block info for directory data */
    summary->blocks[1].ino = LSFS_ROOT_INO;
    summary->blocks[1].offset = 0;
    summary->blocks[1].type = LSFS_BLOCK_TYPE_DIRENT;

    /* Write segment header */
    if (write_block(fd, LSFS_LOG_START, block) < 0) {
        fprintf(stderr, "Failed to write segment header\n");
        close(fd);
        return -1;
    }

    /* Write root inode block */
    memset(block, 0, LSFS_BLOCK_SIZE);
    memcpy(block, &root_inode, sizeof(root_inode));
    if (write_block(fd, LSFS_LOG_START + 1, block) < 0) {
        fprintf(stderr, "Failed to write root inode\n");
        close(fd);
        return -1;
    }

    /* Write root directory data */
    if (write_block(fd, LSFS_LOG_START + 2, dir_block) < 0) {
        fprintf(stderr, "Failed to write root directory\n");
        close(fd);
        return -1;
    }

    /* Initialize checkpoint region 0 */
    memset(&cp, 0, sizeof(cp));
    cp.magic = LSFS_CHECKPOINT_MAGIC;
    cp.version = LSFS_VERSION;
    cp.sequence = 1;
    cp.timestamp = now;
    cp.log_head = LSFS_LOG_START + 3;
    cp.imap_entries = 1;
    cp.segment_entries = total_segments;
    cp.checksum = 0;
    cp.complete = 1;

    memset(block, 0, LSFS_BLOCK_SIZE);
    memcpy(block, &cp, sizeof(cp));
    if (write_block(fd, LSFS_CHECKPOINT0_START, block) < 0) {
        fprintf(stderr, "Failed to write checkpoint header\n");
        close(fd);
        return -1;
    }

    /* Write inode map (just root inode) */
    memset(&root_imap, 0, sizeof(root_imap));
    root_imap.ino = LSFS_ROOT_INO;
    root_imap.location = LSFS_LOG_START + 1;
    root_imap.version = 1;

    memset(block, 0, LSFS_BLOCK_SIZE);
    memcpy(block, &root_imap, sizeof(root_imap));
    if (write_block(fd, LSFS_CHECKPOINT0_START + 1, block) < 0) {
        fprintf(stderr, "Failed to write inode map\n");
        close(fd);
        return -1;
    }

    /* Initialize segment usage table */
    memset(block, 0, LSFS_BLOCK_SIZE);
    struct lsfs_segment_usage *seg_usage = (struct lsfs_segment_usage *)block;

    /* First segment is used */
    seg_usage[0].segment_id = 0;
    seg_usage[0].state = LSFS_SEG_FULL;
    seg_usage[0].live_blocks = 2;
    seg_usage[0].timestamp = now;

    /* Rest are free */
    for (uint32_t i = 1; i < total_segments && i < (LSFS_BLOCK_SIZE / sizeof(struct lsfs_segment_usage)); i++) {
        seg_usage[i].segment_id = i;
        seg_usage[i].state = LSFS_SEG_FREE;
        seg_usage[i].live_blocks = 0;
        seg_usage[i].timestamp = 0;
    }

    if (write_block(fd, LSFS_SEGTABLE_START, block) < 0) {
        fprintf(stderr, "Failed to write segment table\n");
        close(fd);
        return -1;
    }

    /* Sync to disk */
    fsync(fd);
    close(fd);

    printf("\nFilesystem created successfully!\n");
    printf("Mount with: lsfs %s <mountpoint>\n", path);

    return 0;
}

/*
 * Print usage
 */
static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] <disk_image>\n\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s, --size <MB>     Filesystem size in MB (default: %d)\n", DEFAULT_SIZE_MB);
    fprintf(stderr, "  -h, --help          Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s -s 512 disk.img\n", progname);
}

int main(int argc, char *argv[])
{
    uint64_t size_mb = DEFAULT_SIZE_MB;
    char *path = NULL;
    int opt;

    static struct option long_options[] = {
        {"size", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "s:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 's':
            size_mb = strtoull(optarg, NULL, 10);
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

    /* Validate size */
    if (size_mb < 16) {
        fprintf(stderr, "Error: Minimum size is 16 MB\n");
        return 1;
    }

    if (size_mb > 1024) {
        fprintf(stderr, "Error: Maximum size is 1024 MB (1 GB)\n");
        return 1;
    }

    return format_filesystem(path, size_mb * 1024 * 1024);
}
