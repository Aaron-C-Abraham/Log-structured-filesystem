/*
 * LSFS - Log-Structured Filesystem
 * Main Entry Point
 */

#define FUSE_USE_VERSION 35

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <fuse3/fuse_lowlevel.h>

#include "lsfs.h"

static struct lsfs_context lsfs_ctx;
static struct fuse_session *fuse_se = NULL;

/*
 * Signal handler for clean shutdown
 */
static void signal_handler(int sig)
{
    (void)sig;
    if (fuse_se) {
        fuse_session_exit(fuse_se);
    }
}

/*
 * Initialize the filesystem
 */
static int lsfs_init_fs(struct lsfs_context *ctx)
{
    int ret;

    /* Initialize buffer pool */
    ret = lsfs_buffer_pool_init(&ctx->bufpool);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to initialize buffer pool");
        return ret;
    }

    /* Read superblock */
    ret = lsfs_read_block(ctx, LSFS_SUPERBLOCK_BLOCK, &ctx->sb);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to read superblock");
        return ret;
    }

    /* Validate superblock */
    if (ctx->sb.magic != LSFS_MAGIC) {
        LSFS_ERROR("Invalid filesystem magic: 0x%08x (expected 0x%08x)",
                   ctx->sb.magic, LSFS_MAGIC);
        return LSFS_ERR_CORRUPT;
    }

    if (ctx->sb.version != LSFS_VERSION) {
        LSFS_ERROR("Unsupported filesystem version: %u", ctx->sb.version);
        return LSFS_ERR_CORRUPT;
    }

    LSFS_INFO("LSFS version %u, %lu blocks, %lu segments",
              ctx->sb.version,
              (unsigned long)ctx->sb.total_blocks,
              (unsigned long)ctx->sb.total_segments);

    /* Initialize inode cache */
    ret = lsfs_inode_cache_init(&ctx->icache);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to initialize inode cache");
        return ret;
    }

    /* Initialize inode map */
    ret = lsfs_imap_init(&ctx->imap);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to initialize inode map");
        return ret;
    }

    /* Initialize segment management */
    ret = lsfs_segment_init(ctx);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to initialize segment management");
        return ret;
    }

    /* Initialize checkpoint system */
    ret = lsfs_checkpoint_init(ctx);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to initialize checkpoint system");
        return ret;
    }

    /* Recover from last checkpoint */
    ret = lsfs_checkpoint_recover(ctx);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to recover filesystem");
        return ret;
    }

    /* Initialize locks */
    if (pthread_mutex_init(&ctx->write_lock, NULL) != 0) {
        LSFS_ERROR("Failed to initialize write lock");
        return LSFS_ERR_NOMEM;
    }

    if (pthread_rwlock_init(&ctx->fs_lock, NULL) != 0) {
        LSFS_ERROR("Failed to initialize fs lock");
        return LSFS_ERR_NOMEM;
    }

    /* Initialize garbage collector */
    ret = lsfs_gc_init(ctx);
    if (ret != LSFS_OK) {
        LSFS_ERROR("Failed to initialize garbage collector");
        return ret;
    }

    /* Update mount information */
    ctx->sb.mounted_at = (uint64_t)time(NULL);
    ctx->sb.mount_count++;
    ctx->sb.state = 1;  /* Dirty */
    lsfs_write_block(ctx, LSFS_SUPERBLOCK_BLOCK, &ctx->sb);

    ctx->mounted = true;
    g_lsfs = ctx;

    LSFS_INFO("Filesystem mounted successfully");
    return LSFS_OK;
}

/*
 * Cleanup the filesystem
 */
static void lsfs_cleanup_fs(struct lsfs_context *ctx)
{
    if (!ctx->mounted) {
        return;
    }

    LSFS_INFO("Unmounting filesystem...");

    /* Stop garbage collector */
    lsfs_gc_destroy(ctx);

    /* Flush pending writes */
    lsfs_segment_flush(ctx);

    /* Write final checkpoint */
    lsfs_checkpoint_write(ctx);

    /* Update superblock */
    ctx->sb.state = 0;  /* Clean */
    lsfs_write_block(ctx, LSFS_SUPERBLOCK_BLOCK, &ctx->sb);
    lsfs_sync(ctx);

    /* Cleanup */
    lsfs_segment_destroy(ctx);
    lsfs_imap_destroy(&ctx->imap);
    lsfs_inode_cache_destroy(&ctx->icache);
    lsfs_buffer_pool_destroy(&ctx->bufpool);

    pthread_mutex_destroy(&ctx->write_lock);
    pthread_rwlock_destroy(&ctx->fs_lock);

    lsfs_io_destroy(ctx);

    ctx->mounted = false;
    g_lsfs = NULL;

    LSFS_INFO("Filesystem unmounted");
}

/*
 * Print usage
 */
static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] <disk_image> <mount_point>\n\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -f, --foreground    Run in foreground\n");
    fprintf(stderr, "  -d, --debug         Enable debug output\n");
    fprintf(stderr, "  -o <options>        FUSE mount options\n");
    fprintf(stderr, "  -h, --help          Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s -f disk.img /mnt/lsfs\n", progname);
}

/*
 * Main entry point
 */
int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    char *disk_path = NULL;
    char *mount_point = NULL;
    int foreground = 0;
    int debug = 0;
    int ret = 1;
    int opt;

    static struct option long_options[] = {
        {"foreground", no_argument, NULL, 'f'},
        {"debug", no_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    /* Parse options */
    while ((opt = getopt_long(argc, argv, "fdo:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'f':
            foreground = 1;
            break;
        case 'd':
            debug = 1;
            foreground = 1;
            break;
        case 'o':
            fuse_opt_add_arg(&args, "-o");
            fuse_opt_add_arg(&args, optarg);
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Get disk image and mount point */
    if (optind + 2 != argc) {
        usage(argv[0]);
        return 1;
    }

    disk_path = argv[optind];
    mount_point = argv[optind + 1];

    /* Initialize context */
    memset(&lsfs_ctx, 0, sizeof(lsfs_ctx));
    lsfs_ctx.fd = -1;
    lsfs_ctx.debug = debug;

    /* Open disk image */
    ret = lsfs_io_init(&lsfs_ctx, disk_path);
    if (ret != LSFS_OK) {
        fprintf(stderr, "Failed to open disk image: %s\n", disk_path);
        return 1;
    }

    /* Initialize filesystem */
    ret = lsfs_init_fs(&lsfs_ctx);
    if (ret != LSFS_OK) {
        fprintf(stderr, "Failed to initialize filesystem\n");
        lsfs_io_destroy(&lsfs_ctx);
        return 1;
    }

    /* Build FUSE args */
    fuse_opt_add_arg(&args, argv[0]);
    if (foreground) {
        fuse_opt_add_arg(&args, "-f");
    }
    if (debug) {
        fuse_opt_add_arg(&args, "-d");
    }
    fuse_opt_add_arg(&args, mount_point);

    /* Create FUSE session */
    fuse_se = fuse_session_new(&args, &lsfs_fuse_ops,
                               sizeof(lsfs_fuse_ops), &lsfs_ctx);
    if (!fuse_se) {
        fprintf(stderr, "Failed to create FUSE session\n");
        lsfs_cleanup_fs(&lsfs_ctx);
        fuse_opt_free_args(&args);
        return 1;
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Mount */
    if (fuse_session_mount(fuse_se, mount_point) != 0) {
        fprintf(stderr, "Failed to mount filesystem\n");
        fuse_session_destroy(fuse_se);
        lsfs_cleanup_fs(&lsfs_ctx);
        fuse_opt_free_args(&args);
        return 1;
    }

    /* Enter main loop */
    if (foreground) {
        ret = fuse_session_loop(fuse_se);
    } else {
        ret = fuse_daemonize(0);
        if (ret == 0) {
            ret = fuse_session_loop(fuse_se);
        }
    }

    /* Cleanup */
    fuse_session_unmount(fuse_se);
    fuse_session_destroy(fuse_se);
    lsfs_cleanup_fs(&lsfs_ctx);
    fuse_opt_free_args(&args);

    return ret ? 1 : 0;
}
