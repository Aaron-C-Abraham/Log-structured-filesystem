/*
 * LSFS - Log-Structured Filesystem
 * On-disk format definitions
 */

#ifndef LSFS_ONDISK_H
#define LSFS_ONDISK_H

#include <stdint.h>

/* Magic numbers */
#define LSFS_MAGIC          0x4C534653  /* "LSFS" */
#define LSFS_SEGMENT_MAGIC  0x5345474D  /* "SEGM" */
#define LSFS_CHECKPOINT_MAGIC 0x43484B50 /* "CHKP" */

/* Version */
#define LSFS_VERSION        1

/* Size constants */
#define LSFS_BLOCK_SIZE         4096
#define LSFS_SEGMENT_BLOCKS     1024        /* 4 MB per segment */
#define LSFS_SEGMENT_SIZE       (LSFS_SEGMENT_BLOCKS * LSFS_BLOCK_SIZE)
#define LSFS_MAX_SEGMENTS       256         /* Up to 1 GB */
#define LSFS_MAX_INODES         65536       /* 16-bit inode numbers */

/* Disk layout constants */
#define LSFS_SUPERBLOCK_BLOCK   0
#define LSFS_CHECKPOINT0_START  1
#define LSFS_CHECKPOINT0_BLOCKS 256
#define LSFS_CHECKPOINT1_START  257
#define LSFS_CHECKPOINT1_BLOCKS 256
#define LSFS_SEGTABLE_START     513
#define LSFS_SEGTABLE_BLOCKS    512
#define LSFS_LOG_START          1025

/* Inode constants */
#define LSFS_ROOT_INO           1
#define LSFS_DIRECT_BLOCKS      12
#define LSFS_SYMLINK_INLINE_MAX 64

/* Name lengths */
#define LSFS_NAME_MAX           255

/* File types for directory entries */
#define LSFS_FT_UNKNOWN         0
#define LSFS_FT_REG_FILE        1
#define LSFS_FT_DIR             2
#define LSFS_FT_CHRDEV          3
#define LSFS_FT_BLKDEV          4
#define LSFS_FT_FIFO            5
#define LSFS_FT_SOCK            6
#define LSFS_FT_SYMLINK         7

/* Inode flags */
#define LSFS_INODE_DELETED      (1 << 0)
#define LSFS_INODE_DIRTY        (1 << 1)

/* Segment states */
#define LSFS_SEG_FREE           0
#define LSFS_SEG_ACTIVE         1
#define LSFS_SEG_FULL           2
#define LSFS_SEG_CLEANING       3

/*
 * Superblock - stored at block 0
 * Total size: 4096 bytes
 */
struct lsfs_superblock {
    uint32_t magic;                 /* LSFS_MAGIC (0x4C534653) */
    uint32_t version;               /* Filesystem version */
    uint32_t block_size;            /* Block size in bytes (4096) */
    uint32_t segment_size;          /* Segment size in blocks */
    uint64_t total_blocks;          /* Total blocks in filesystem */
    uint64_t total_segments;        /* Total segments */
    uint64_t inode_count;           /* Number of allocated inodes */
    uint64_t checkpoint_region[2];  /* Alternating checkpoint locations */
    uint32_t active_checkpoint;     /* Which checkpoint is current (0 or 1) */
    uint32_t padding1;              /* Alignment padding */
    uint64_t log_head;              /* Current log write position (block) */
    uint64_t free_segments;         /* Count of free segments */
    uint8_t  uuid[16];              /* Filesystem UUID */
    uint64_t created_at;            /* Creation timestamp */
    uint64_t mounted_at;            /* Last mount timestamp */
    uint32_t mount_count;           /* Number of mounts */
    uint32_t state;                 /* Clean/dirty state */
    uint8_t  reserved[3960];        /* Pad to 4096 bytes */
} __attribute__((packed));

/*
 * Inode structure - 256 bytes
 */
struct lsfs_inode {
    uint32_t ino;                   /* Inode number */
    uint32_t mode;                  /* File type and permissions */
    uint32_t uid;                   /* Owner user ID */
    uint32_t gid;                   /* Owner group ID */
    uint64_t size;                  /* File size in bytes */
    uint64_t blocks;                /* Number of blocks allocated */
    uint64_t atime;                 /* Access time (nanoseconds since epoch) */
    uint64_t mtime;                 /* Modification time */
    uint64_t ctime;                 /* Change time */
    uint32_t nlink;                 /* Hard link count */
    uint32_t flags;                 /* Inode flags */

    /* Block pointers (direct + indirect) */
    uint64_t direct[LSFS_DIRECT_BLOCKS];  /* Direct block pointers */
    uint64_t indirect;              /* Single indirect block */
    uint64_t double_indirect;       /* Double indirect block */

    /* For symbolic links (inline if short) */
    char symlink[LSFS_SYMLINK_INLINE_MAX]; /* Inline symlink target */

    uint64_t generation;            /* Inode generation number */
    uint8_t reserved[48];           /* Future use, pad to 256 bytes */
} __attribute__((packed));

/*
 * Directory entry - variable length
 */
struct lsfs_dirent {
    uint32_t ino;                   /* Inode number */
    uint16_t rec_len;               /* Total entry length */
    uint8_t  name_len;              /* Name length */
    uint8_t  file_type;             /* File type (DT_REG, DT_DIR, etc.) */
    char     name[];                /* Filename (variable length) */
} __attribute__((packed));

/*
 * Inode map entry - maps inode number to disk location
 */
struct lsfs_imap_entry {
    uint32_t ino;                   /* Inode number */
    uint32_t version;               /* Version number (for stale detection) */
    uint64_t location;              /* Block address of inode */
} __attribute__((packed));

/*
 * Segment summary block - first block of each segment
 */
struct lsfs_segment_header {
    uint32_t magic;                 /* LSFS_SEGMENT_MAGIC */
    uint32_t segment_id;            /* Segment identifier */
    uint64_t timestamp;             /* Write timestamp */
    uint32_t block_count;           /* Number of blocks used in segment */
    uint32_t checksum;              /* CRC32 of segment data */
} __attribute__((packed));

/*
 * Block info in segment summary
 */
struct lsfs_block_info {
    uint32_t ino;                   /* Owning inode */
    uint32_t offset;                /* Offset within file (in blocks) */
    uint8_t  type;                  /* Block type (data, inode, indirect) */
    uint8_t  reserved[3];
} __attribute__((packed));

#define LSFS_BLOCK_TYPE_DATA      0
#define LSFS_BLOCK_TYPE_INODE     1
#define LSFS_BLOCK_TYPE_INDIRECT  2
#define LSFS_BLOCK_TYPE_DIRENT    3

/*
 * Segment summary - follows header in first block
 */
struct lsfs_segment_summary {
    struct lsfs_segment_header header;
    /* Array of block info follows, up to LSFS_SEGMENT_BLOCKS - 1 entries */
    struct lsfs_block_info blocks[];
} __attribute__((packed));

/*
 * Segment usage table entry
 */
struct lsfs_segment_usage {
    uint32_t segment_id;            /* Segment ID */
    uint32_t state;                 /* Segment state */
    uint32_t live_blocks;           /* Number of live blocks */
    uint32_t reserved;
    uint64_t timestamp;             /* Last write timestamp */
} __attribute__((packed));

/*
 * Checkpoint header
 */
struct lsfs_checkpoint_header {
    uint32_t magic;                 /* LSFS_CHECKPOINT_MAGIC */
    uint32_t version;               /* Checkpoint version */
    uint64_t sequence;              /* Checkpoint sequence number */
    uint64_t timestamp;             /* Creation timestamp */
    uint64_t log_head;              /* Log head at checkpoint time */
    uint32_t imap_entries;          /* Number of inode map entries */
    uint32_t segment_entries;       /* Number of segment table entries */
    uint32_t checksum;              /* Header checksum */
    uint32_t complete;              /* Completion marker */
} __attribute__((packed));

/* Static assertions for structure sizes */
_Static_assert(sizeof(struct lsfs_superblock) == LSFS_BLOCK_SIZE,
               "Superblock must be exactly one block");
_Static_assert(sizeof(struct lsfs_inode) == 256,
               "Inode must be exactly 256 bytes");

#endif /* LSFS_ONDISK_H */
