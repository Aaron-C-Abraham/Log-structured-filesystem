/*
 * LSFS - Log-Structured Filesystem
 * Main header file
 */

#ifndef LSFS_H
#define LSFS_H

#define FUSE_USE_VERSION 35

#include <fuse3/fuse_lowlevel.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "ondisk.h"

/* Error codes */
#define LSFS_OK              0
#define LSFS_ERR_IO         -1
#define LSFS_ERR_NOMEM      -2
#define LSFS_ERR_NOSPC      -3
#define LSFS_ERR_CORRUPT    -4
#define LSFS_ERR_EXIST      -5
#define LSFS_ERR_NOENT      -6
#define LSFS_ERR_NOTDIR     -7
#define LSFS_ERR_ISDIR      -8
#define LSFS_ERR_NOTEMPTY   -9
#define LSFS_ERR_INVAL      -10

/* Forward declarations */
struct lsfs_context;
struct lsfs_inode_cache;
struct lsfs_segment_buffer;

/*
 * In-memory inode structure
 */
struct lsfs_inode_mem {
    struct lsfs_inode disk_inode;   /* On-disk inode data */
    uint64_t disk_location;         /* Where on disk this was read from */
    uint32_t version;               /* Version for stale detection */
    uint32_t refcount;              /* Reference count */
    bool dirty;                     /* Needs to be written */
    pthread_mutex_t lock;           /* Per-inode lock */
    struct lsfs_inode_mem *next;    /* Hash chain */
    struct lsfs_inode_mem *lru_prev; /* LRU list */
    struct lsfs_inode_mem *lru_next; /* LRU list */
};

/*
 * Inode cache
 */
#define LSFS_INODE_CACHE_SIZE   1024
#define LSFS_INODE_CACHE_BUCKETS 256

struct lsfs_inode_cache {
    struct lsfs_inode_mem *buckets[LSFS_INODE_CACHE_BUCKETS];
    struct lsfs_inode_mem *lru_head;
    struct lsfs_inode_mem *lru_tail;
    uint32_t count;
    pthread_mutex_t lock;
};

/*
 * In-memory inode map
 */
struct lsfs_imap {
    struct lsfs_imap_entry *entries;
    uint32_t capacity;
    uint32_t count;
    uint32_t next_ino;              /* Next available inode number */
    pthread_rwlock_t lock;
};

/*
 * Segment buffer for writes
 */
struct lsfs_segment_buffer {
    uint8_t *data;                  /* Buffer for segment data */
    struct lsfs_block_info *block_info; /* Info for each block */
    uint32_t segment_id;            /* Current segment ID */
    uint32_t block_count;           /* Blocks used in buffer */
    pthread_mutex_t lock;
};

/*
 * Segment usage tracking
 */
struct lsfs_segment_table {
    struct lsfs_segment_usage *entries;
    uint32_t count;
    uint32_t free_count;
    pthread_mutex_t lock;
};

/*
 * Buffer pool entry
 */
struct lsfs_buffer {
    uint8_t data[LSFS_BLOCK_SIZE];
    uint64_t block_num;
    bool valid;
    bool dirty;
    uint32_t refcount;
    struct lsfs_buffer *hash_next;
    struct lsfs_buffer *lru_prev;
    struct lsfs_buffer *lru_next;
};

/*
 * Buffer pool
 */
#define LSFS_BUFFER_POOL_SIZE   256
#define LSFS_BUFFER_HASH_SIZE   64

struct lsfs_buffer_pool {
    struct lsfs_buffer buffers[LSFS_BUFFER_POOL_SIZE];
    struct lsfs_buffer *hash[LSFS_BUFFER_HASH_SIZE];
    struct lsfs_buffer *lru_head;
    struct lsfs_buffer *lru_tail;
    pthread_mutex_t lock;
};

/*
 * Main filesystem context
 */
struct lsfs_context {
    /* Backing store */
    int fd;                         /* File descriptor for disk image */
    char *disk_path;                /* Path to disk image */
    uint64_t disk_size;             /* Size of disk image */

    /* On-disk metadata */
    struct lsfs_superblock sb;      /* Superblock (in memory copy) */

    /* In-memory structures */
    struct lsfs_inode_cache icache; /* Inode cache */
    struct lsfs_imap imap;          /* Inode map */
    struct lsfs_segment_table segtable; /* Segment usage table */
    struct lsfs_segment_buffer segbuf; /* Current write segment */
    struct lsfs_buffer_pool bufpool; /* Block buffer pool */

    /* Checkpoint state */
    uint64_t checkpoint_seq;        /* Current checkpoint sequence */
    uint64_t last_checkpoint;       /* Time of last checkpoint */
    uint32_t writes_since_checkpoint; /* Writes since last checkpoint */

    /* GC state */
    pthread_t gc_thread;            /* Garbage collector thread */
    bool gc_running;                /* GC thread running flag */
    pthread_cond_t gc_cond;         /* GC wake condition */
    pthread_mutex_t gc_lock;        /* GC synchronization */

    /* Global locks */
    pthread_mutex_t write_lock;     /* Serialize writes */
    pthread_rwlock_t fs_lock;       /* Filesystem-wide lock */

    /* Runtime flags */
    bool mounted;
    bool readonly;
    bool debug;
};

/* Global context (set during mount) */
extern struct lsfs_context *g_lsfs;

/*
 * io.c - Block I/O operations
 */
int lsfs_io_init(struct lsfs_context *ctx, const char *path);
void lsfs_io_destroy(struct lsfs_context *ctx);
int lsfs_read_block(struct lsfs_context *ctx, uint64_t block_num, void *buf);
int lsfs_write_block(struct lsfs_context *ctx, uint64_t block_num, const void *buf);
int lsfs_read_blocks(struct lsfs_context *ctx, uint64_t start_block,
                     uint32_t count, void *buf);
int lsfs_write_blocks(struct lsfs_context *ctx, uint64_t start_block,
                      uint32_t count, const void *buf);
int lsfs_sync(struct lsfs_context *ctx);

/* Buffer pool operations */
int lsfs_buffer_pool_init(struct lsfs_buffer_pool *pool);
void lsfs_buffer_pool_destroy(struct lsfs_buffer_pool *pool);
struct lsfs_buffer *lsfs_buffer_get(struct lsfs_context *ctx, uint64_t block_num);
void lsfs_buffer_put(struct lsfs_buffer *buf);
int lsfs_buffer_flush(struct lsfs_context *ctx);

/*
 * inode.c - Inode operations
 */
int lsfs_inode_cache_init(struct lsfs_inode_cache *cache);
void lsfs_inode_cache_destroy(struct lsfs_inode_cache *cache);
struct lsfs_inode_mem *lsfs_inode_get(struct lsfs_context *ctx, uint32_t ino);
void lsfs_inode_put(struct lsfs_inode_mem *inode);
struct lsfs_inode_mem *lsfs_inode_alloc(struct lsfs_context *ctx, uint32_t mode);
int lsfs_inode_free(struct lsfs_context *ctx, struct lsfs_inode_mem *inode);
int lsfs_inode_write(struct lsfs_context *ctx, struct lsfs_inode_mem *inode);
int lsfs_inode_read_block(struct lsfs_context *ctx, struct lsfs_inode_mem *inode,
                          uint64_t block_idx, void *buf);
int lsfs_inode_write_block(struct lsfs_context *ctx, struct lsfs_inode_mem *inode,
                           uint64_t block_idx, const void *buf);
void lsfs_inode_to_stat(struct lsfs_inode_mem *inode, struct stat *st);
uint64_t lsfs_get_time_ns(void);

/*
 * directory.c - Directory operations
 */
int lsfs_dir_lookup(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                    const char *name, uint32_t *ino, uint8_t *file_type);
int lsfs_dir_add(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                 const char *name, uint32_t ino, uint8_t file_type);
int lsfs_dir_remove(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                    const char *name);
int lsfs_dir_is_empty(struct lsfs_context *ctx, struct lsfs_inode_mem *dir);
int lsfs_dir_iterate(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                     int (*callback)(void *ctx, const char *name, uint32_t ino,
                                     uint8_t type, off_t offset),
                     void *callback_ctx, off_t offset);
int lsfs_dir_init(struct lsfs_context *ctx, struct lsfs_inode_mem *dir,
                  uint32_t parent_ino);
uint8_t lsfs_mode_to_type(uint32_t mode);

/*
 * segment.c - Segment management
 */
int lsfs_segment_init(struct lsfs_context *ctx);
void lsfs_segment_destroy(struct lsfs_context *ctx);
int lsfs_segment_buffer_init(struct lsfs_segment_buffer *segbuf);
void lsfs_segment_buffer_destroy(struct lsfs_segment_buffer *segbuf);
int lsfs_segment_alloc(struct lsfs_context *ctx, uint32_t *segment_id);
int lsfs_segment_free(struct lsfs_context *ctx, uint32_t segment_id);
uint64_t lsfs_segment_append_block(struct lsfs_context *ctx, const void *data,
                                   uint32_t ino, uint32_t offset, uint8_t type);
int lsfs_segment_flush(struct lsfs_context *ctx);
uint64_t lsfs_segment_to_block(uint32_t segment_id, uint32_t offset);
void lsfs_block_to_segment(uint64_t block, uint32_t *segment_id, uint32_t *offset);

/*
 * imap.c - Inode map
 */
int lsfs_imap_init(struct lsfs_imap *imap);
void lsfs_imap_destroy(struct lsfs_imap *imap);
int lsfs_imap_get(struct lsfs_imap *imap, uint32_t ino, uint64_t *location,
                  uint32_t *version);
int lsfs_imap_set(struct lsfs_imap *imap, uint32_t ino, uint64_t location);
int lsfs_imap_remove(struct lsfs_imap *imap, uint32_t ino);
uint32_t lsfs_imap_alloc_ino(struct lsfs_imap *imap);
int lsfs_imap_save(struct lsfs_context *ctx, uint64_t start_block);
int lsfs_imap_load(struct lsfs_context *ctx, uint64_t start_block,
                   uint32_t entry_count);

/*
 * checkpoint.c - Checkpoint management
 */
int lsfs_checkpoint_init(struct lsfs_context *ctx);
int lsfs_checkpoint_write(struct lsfs_context *ctx);
int lsfs_checkpoint_load(struct lsfs_context *ctx);
int lsfs_checkpoint_recover(struct lsfs_context *ctx);
bool lsfs_checkpoint_needed(struct lsfs_context *ctx);

/*
 * gc.c - Garbage collection
 */
int lsfs_gc_init(struct lsfs_context *ctx);
void lsfs_gc_destroy(struct lsfs_context *ctx);
int lsfs_gc_run(struct lsfs_context *ctx);
int lsfs_gc_clean_segment(struct lsfs_context *ctx, uint32_t segment_id);
uint32_t lsfs_gc_select_segment(struct lsfs_context *ctx);
void lsfs_gc_mark_block_dead(struct lsfs_context *ctx, uint64_t block);
void lsfs_gc_trigger(struct lsfs_context *ctx);
bool lsfs_gc_needed(struct lsfs_context *ctx);

/*
 * fuse_ops.c - FUSE operations
 */
extern const struct fuse_lowlevel_ops lsfs_fuse_ops;

int lsfs_mount(const char *disk_path, const char *mount_point,
               bool foreground, bool debug);
void lsfs_unmount(struct lsfs_context *ctx);

/*
 * Utility macros
 */
#define LSFS_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define LSFS_ALIGN_UP(n, a)     (((n) + (a) - 1) & ~((a) - 1))
#define LSFS_MIN(a, b)          ((a) < (b) ? (a) : (b))
#define LSFS_MAX(a, b)          ((a) > (b) ? (a) : (b))

#define LSFS_BLOCKS_FOR_SIZE(size) LSFS_DIV_ROUND_UP(size, LSFS_BLOCK_SIZE)

/* Debug logging */
#ifdef DEBUG
#define LSFS_DEBUG(fmt, ...) \
    fprintf(stderr, "[LSFS DEBUG] %s:%d: " fmt "\n", \
            __func__, __LINE__, ##__VA_ARGS__)
#else
#define LSFS_DEBUG(fmt, ...) ((void)0)
#endif

#define LSFS_ERROR(fmt, ...) \
    fprintf(stderr, "[LSFS ERROR] %s:%d: " fmt "\n", \
            __func__, __LINE__, ##__VA_ARGS__)

#define LSFS_INFO(fmt, ...) \
    fprintf(stderr, "[LSFS] " fmt "\n", ##__VA_ARGS__)

#endif /* LSFS_H */
