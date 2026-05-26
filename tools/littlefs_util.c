#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bd/lfs_filebd.h"
#include "lfs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_RW_SIZE 16U
#define DEFAULT_BLOCK_SIZE 4096U
#define DEFAULT_BLOCK_COUNT 256U
#define DEFAULT_CACHE_SIZE 64U
#define DEFAULT_LOOKAHEAD_SIZE 64U
#define DEFAULT_BLOCK_CYCLES 500
#define IO_BUFFER_SIZE 4096U

typedef enum {
    CMD_NONE = 0,
    CMD_CREATE,
    CMD_UNPACK,
    CMD_LS,
    CMD_CAT,
    CMD_TREE,
} command_t;

typedef struct {
    command_t cmd;
    const char *cmd_path;
    const char *in_image;
    const char *out_path;
    uint32_t read_size;
    uint32_t prog_size;
    uint32_t block_size;
    uint32_t block_count;
    uint64_t image_size;
    uint32_t cache_size;
    uint32_t lookahead_size;
    int32_t block_cycles;
} cli_opts_t;

typedef struct {
    lfs_t lfs;
    lfs_filebd_t bd;
    struct lfs_filebd_config bd_cfg;
    struct lfs_config cfg;
    void *read_buffer;
    void *prog_buffer;
    void *lookahead_buffer;
} lfs_ctx_t;

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --create <host_dir> --out <image.littlefs> --size <bytes> [options]\n"
            "  %s --unpack <image.littlefs> --out <host_dir> [options]\n"
            "  %s --ls <path> --in <image.littlefs> [options]\n"
            "  %s --cat <path> --in <image.littlefs> [options]\n"
            "  %s --tree <path> --in <image.littlefs> [options]\n"
            "\n"
            "Options:\n"
            "  --read-size <n>        default: %u\n"
            "  --prog-size <n>        default: %u\n"
            "  --block-size <n>       default: %u\n"
            "  --size <bytes>         required for --create\n"
            "  --block-count <n>      optional override\n"
            "  --cache-size <n>       default: %u\n"
            "  --lookahead-size <n>   default: %u\n"
            "  --block-cycles <n>     default: %d\n"
            "  --help\n",
            argv0, argv0, argv0, argv0, argv0,
            DEFAULT_RW_SIZE, DEFAULT_RW_SIZE,
            DEFAULT_BLOCK_SIZE,
            DEFAULT_CACHE_SIZE, DEFAULT_LOOKAHEAD_SIZE,
            DEFAULT_BLOCK_CYCLES);
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long v;

    if (!text || !value) {
        return false;
    }

    errno = 0;
    v = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v > UINT32_MAX) {
        return false;
    }

    *value = (uint32_t)v;
    return true;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long v;

    if (!text || !value) {
        return false;
    }

    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *value = (uint64_t)v;
    return true;
}

static bool parse_i32(const char *text, int32_t *value)
{
    char *end = NULL;
    long v;

    if (!text || !value) {
        return false;
    }

    errno = 0;
    v = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v < INT32_MIN ||
        v > INT32_MAX) {
        return false;
    }

    *value = (int32_t)v;
    return true;
}

static const char *next_arg(int argc, char **argv, int *idx)
{
    if (*idx + 1 >= argc) {
        return NULL;
    }
    (*idx)++;
    return argv[*idx];
}

static int parse_args(int argc, char **argv, cli_opts_t *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->read_size = DEFAULT_RW_SIZE;
    opts->prog_size = DEFAULT_RW_SIZE;
    opts->block_size = DEFAULT_BLOCK_SIZE;
    opts->block_count = DEFAULT_BLOCK_COUNT;
    opts->cache_size = DEFAULT_CACHE_SIZE;
    opts->lookahead_size = DEFAULT_LOOKAHEAD_SIZE;
    opts->block_cycles = DEFAULT_BLOCK_CYCLES;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            return 1;
        }

        if (strcmp(argv[i], "--create") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg) {
                fprintf(stderr, "Missing value for --create\n");
                return -1;
            }
            opts->cmd = CMD_CREATE;
            opts->cmd_path = arg;
            continue;
        }

        if (strcmp(argv[i], "--unpack") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg) {
                fprintf(stderr, "Missing value for --unpack\n");
                return -1;
            }
            opts->cmd = CMD_UNPACK;
            opts->cmd_path = arg;
            continue;
        }

        if (strcmp(argv[i], "--ls") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg) {
                fprintf(stderr, "Missing value for --ls\n");
                return -1;
            }
            opts->cmd = CMD_LS;
            opts->cmd_path = arg;
            continue;
        }

        if (strcmp(argv[i], "--cat") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg) {
                fprintf(stderr, "Missing value for --cat\n");
                return -1;
            }
            opts->cmd = CMD_CAT;
            opts->cmd_path = arg;
            continue;
        }

        if (strcmp(argv[i], "--tree") == 0) {
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) {
                opts->cmd_path = argv[++i];
            } else {
                opts->cmd_path = "/";
            }
            opts->cmd = CMD_TREE;
            continue;
        }

        if (strcmp(argv[i], "--in") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg) {
                fprintf(stderr, "Missing value for --in\n");
                return -1;
            }
            opts->in_image = arg;
            continue;
        }

        if (strcmp(argv[i], "--out") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg) {
                fprintf(stderr, "Missing value for --out\n");
                return -1;
            }
            opts->out_path = arg;
            continue;
        }

        if (strcmp(argv[i], "--read-size") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_u32(arg, &opts->read_size)) {
                fprintf(stderr, "Invalid --read-size\n");
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--prog-size") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_u32(arg, &opts->prog_size)) {
                fprintf(stderr, "Invalid --prog-size\n");
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--block-size") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_u32(arg, &opts->block_size)) {
                fprintf(stderr, "Invalid --block-size\n");
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--block-count") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_u32(arg, &opts->block_count)) {
                fprintf(stderr, "Invalid --block-count\n");
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--size") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_u64(arg, &opts->image_size)) {
                fprintf(stderr, "Invalid --size\n");
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--cache-size") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_u32(arg, &opts->cache_size)) {
                fprintf(stderr, "Invalid --cache-size\n");
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--lookahead-size") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_u32(arg, &opts->lookahead_size)) {
                fprintf(stderr, "Invalid --lookahead-size\n");
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--block-cycles") == 0) {
            const char *arg = next_arg(argc, argv, &i);
            if (!arg || !parse_i32(arg, &opts->block_cycles)) {
                fprintf(stderr, "Invalid --block-cycles\n");
                return -1;
            }
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        return -1;
    }

    if (opts->cmd == CMD_NONE) {
        fprintf(stderr, "No command selected\n");
        return -1;
    }

    switch (opts->cmd) {
    case CMD_CREATE:
        if (!opts->cmd_path || !opts->out_path) {
            fprintf(stderr, "--create requires --out\n");
            return -1;
        }
        if (opts->image_size == 0) {
            fprintf(stderr, "--create requires --size in bytes\n");
            return -1;
        }
        if (opts->image_size % opts->block_size != 0) {
            fprintf(stderr, "--size must be multiple of --block-size\n");
            return -1;
        }
        opts->block_count = (uint32_t)(opts->image_size / opts->block_size);
        if (opts->block_count == 0) {
            fprintf(stderr, "Calculated block-count is zero\n");
            return -1;
        }
        break;
    case CMD_UNPACK:
        if (!opts->cmd_path || !opts->out_path) {
            fprintf(stderr, "--unpack requires --out\n");
            return -1;
        }
        break;
    case CMD_LS:
    case CMD_CAT:
    case CMD_TREE:
        if (!opts->cmd_path || !opts->in_image) {
            fprintf(stderr,
                    "--ls/--cat/--tree require command path and --in\n");
            return -1;
        }
        break;
    default:
        return -1;
    }

    if (opts->read_size == 0 || opts->prog_size == 0 || opts->block_size == 0 ||
        opts->cache_size == 0 || opts->lookahead_size == 0) {
        fprintf(stderr, "Geometry options must be non-zero\n");
        return -1;
    }

    if ((opts->block_size % opts->read_size) != 0 ||
        (opts->block_size % opts->prog_size) != 0) {
        fprintf(stderr, "block-size must be multiple of read-size/prog-size\n");
        return -1;
    }

    return 0;
}

static void normalize_lfs_path(const char *in, char *out, size_t out_size)
{
    size_t len;

    if (!in || !out || out_size == 0) {
        return;
    }

    while (*in == '/') {
        in++;
    }

    if (*in == '\0') {
        strncpy(out, ".", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    len = strlen(in);
    if (len >= out_size) {
        len = out_size - 1;
    }

    memcpy(out, in, len);
    out[len] = '\0';
}

static int join_host_path(char *out, size_t out_size, const char *a,
                          const char *b)
{
    int n;

    if (!a || !b || !out || out_size == 0) {
        return -1;
    }

    if (a[0] == '\0') {
        n = snprintf(out, out_size, "%s", b);
    } else {
        n = snprintf(out, out_size, "%s/%s", a, b);
    }

    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }

    return 0;
}

static int join_lfs_path(char *out, size_t out_size, const char *parent,
                         const char *name)
{
    int n;

    if (!out || !parent || !name || out_size == 0) {
        return -1;
    }

    if (strcmp(parent, ".") == 0 || parent[0] == '\0') {
        n = snprintf(out, out_size, "%s", name);
    } else {
        n = snprintf(out, out_size, "%s/%s", parent, name);
    }

    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }

    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t i, len;

    if (!path) {
        return -1;
    }

    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }

    memcpy(tmp, path, len + 1);

    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0' && strcmp(tmp, ".") != 0) {
                if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                    return -1;
                }
            }
            tmp[i] = saved;
        }
    }

    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static int file_size_bytes(const char *path, uint64_t *size_out)
{
    struct stat st;

    if (!path || !size_out) {
        return -1;
    }

    if (stat(path, &st) != 0) {
        return -1;
    }

    *size_out = (uint64_t)st.st_size;
    return 0;
}

static int lfs_ctx_open(lfs_ctx_t *ctx, const cli_opts_t *opts,
                        const char *image_path, bool for_pack)
{
    int err;
    uint64_t image_size = 0;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bd.fd = -1;

    ctx->read_buffer = calloc(1, opts->cache_size);
    ctx->prog_buffer = calloc(1, opts->cache_size);
    ctx->lookahead_buffer = calloc(1, opts->lookahead_size);
    if (!ctx->read_buffer || !ctx->prog_buffer || !ctx->lookahead_buffer) {
        fprintf(stderr, "Out of memory allocating LFS buffers\n");
        goto error;
    }

    ctx->bd_cfg.read_size = opts->read_size;
    ctx->bd_cfg.prog_size = opts->prog_size;
    ctx->bd_cfg.erase_size = opts->block_size;

    if (for_pack) {
        ctx->bd_cfg.erase_count = opts->block_count;
    } else {
        if (file_size_bytes(image_path, &image_size) != 0) {
            fprintf(stderr, "Cannot stat image: %s\n", image_path);
            goto error;
        }
        if (opts->block_size == 0 || image_size < opts->block_size) {
            fprintf(stderr, "Invalid image size or block size\n");
            goto error;
        }
        if ((image_size % opts->block_size) != 0) {
            fprintf(stderr,
                    "Image size (%llu) must be a multiple of block size (%u)\n",
                    (unsigned long long)image_size,
                    (unsigned)opts->block_size);
            goto error;
        }
        ctx->bd_cfg.erase_count = (lfs_size_t)(image_size / opts->block_size);
    }

    memset(&ctx->cfg, 0, sizeof(ctx->cfg));
    ctx->cfg.context = &ctx->bd;
    ctx->cfg.read = lfs_filebd_read;
    ctx->cfg.prog = lfs_filebd_prog;
    ctx->cfg.erase = lfs_filebd_erase;
    ctx->cfg.sync = lfs_filebd_sync;
    ctx->cfg.read_size = opts->read_size;
    ctx->cfg.prog_size = opts->prog_size;
    ctx->cfg.block_size = opts->block_size;
    ctx->cfg.block_count = ctx->bd_cfg.erase_count;
    ctx->cfg.block_cycles = opts->block_cycles;
    ctx->cfg.cache_size = opts->cache_size;
    ctx->cfg.lookahead_size = opts->lookahead_size;
    ctx->cfg.read_buffer = ctx->read_buffer;
    ctx->cfg.prog_buffer = ctx->prog_buffer;
    ctx->cfg.lookahead_buffer = ctx->lookahead_buffer;

    err = lfs_filebd_create(&ctx->cfg, image_path, &ctx->bd_cfg);
    if (err != 0) {
        fprintf(stderr, "lfs_filebd_create failed: %d\n", err);
        goto error;
    }

    if (for_pack) {
        off_t full_size =
            (off_t)ctx->bd_cfg.erase_size * (off_t)ctx->bd_cfg.erase_count;
        if (ftruncate(ctx->bd.fd, full_size) != 0) {
            fprintf(stderr, "ftruncate failed for image: %s\n", image_path);
            goto error;
        }
    }

    return 0;

error:
    lfs_ctx_close(ctx);
    return -1;
}

static void lfs_ctx_close(lfs_ctx_t *ctx)
{
    if (ctx->bd.fd >= 0) {
        (void)lfs_filebd_destroy(&ctx->cfg);
    }
    free(ctx->read_buffer);
    free(ctx->prog_buffer);
    free(ctx->lookahead_buffer);
    memset(ctx, 0, sizeof(*ctx));
}

static int copy_host_file_to_lfs(lfs_t *lfs, const char *host_path,
                                 const char *lfs_path)
{
    FILE *in;
    lfs_file_t out;
    uint8_t buffer[IO_BUFFER_SIZE];
    size_t nread;
    int err;

    in = fopen(host_path, "rb");
    if (!in) {
        fprintf(stderr, "Cannot open host file: %s\n", host_path);
        return -1;
    }

    err = lfs_file_open(lfs, &out, lfs_path,
                        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != 0) {
        fprintf(stderr, "lfs_file_open write failed for %s: %d\n", lfs_path,
                err);
        fclose(in);
        return -1;
    }

    while ((nread = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        lfs_ssize_t nwritten = lfs_file_write(lfs, &out, buffer, nread);
        if (nwritten < 0 || (size_t)nwritten != nread) {
            fprintf(stderr, "lfs_file_write failed for %s: %d\n", lfs_path,
                    (int)nwritten);
            (void)lfs_file_close(lfs, &out);
            fclose(in);
            return -1;
        }
    }

    if (ferror(in)) {
        fprintf(stderr, "fread failed for %s\n", host_path);
        (void)lfs_file_close(lfs, &out);
        fclose(in);
        return -1;
    }

    err = lfs_file_close(lfs, &out);
    fclose(in);
    if (err != 0) {
        fprintf(stderr, "lfs_file_close failed for %s: %d\n", lfs_path, err);
        return -1;
    }

    return 0;
}

static int pack_dir_recursive(lfs_t *lfs, const char *host_root,
                              const char *rel_path)
{
    DIR *dir;
    struct dirent *entry;
    char host_dir[PATH_MAX];

    if (rel_path[0] == '\0') {
        if (snprintf(host_dir, sizeof(host_dir), "%s", host_root) >=
            (int)sizeof(host_dir)) {
            return -1;
        }
    } else {
        if (join_host_path(host_dir, sizeof(host_dir), host_root, rel_path) !=
            0) {
            return -1;
        }
    }

    dir = opendir(host_dir);
    if (!dir) {
        fprintf(stderr, "Cannot open directory: %s\n", host_dir);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        char child_rel[PATH_MAX];
        char child_host[PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (rel_path[0] == '\0') {
            if (snprintf(child_rel, sizeof(child_rel), "%s", entry->d_name) >=
                (int)sizeof(child_rel)) {
                closedir(dir);
                return -1;
            }
        } else {
            if (join_lfs_path(child_rel, sizeof(child_rel), rel_path,
                              entry->d_name) != 0) {
                closedir(dir);
                return -1;
            }
        }

        if (join_host_path(child_host, sizeof(child_host), host_dir,
                           entry->d_name) != 0) {
            closedir(dir);
            return -1;
        }

        if (stat(child_host, &st) != 0) {
            fprintf(stderr, "Cannot stat path: %s\n", child_host);
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            int err = lfs_mkdir(lfs, child_rel);
            if (err != 0 && err != LFS_ERR_EXIST) {
                fprintf(stderr, "lfs_mkdir failed for %s: %d\n", child_rel,
                        err);
                closedir(dir);
                return -1;
            }
            if (pack_dir_recursive(lfs, host_root, child_rel) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_host_file_to_lfs(lfs, child_host, child_rel) != 0) {
                closedir(dir);
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}

static int export_file_from_lfs(lfs_t *lfs, const char *lfs_path,
                                const char *host_path)
{
    lfs_file_t in;
    FILE *out;
    uint8_t buffer[IO_BUFFER_SIZE];
    int err;

    out = fopen(host_path, "wb");
    if (!out) {
        fprintf(stderr, "Cannot create host file: %s\n", host_path);
        return -1;
    }

    err = lfs_file_open(lfs, &in, lfs_path, LFS_O_RDONLY);
    if (err != 0) {
        fprintf(stderr, "lfs_file_open read failed for %s: %d\n", lfs_path,
                err);
        fclose(out);
        return -1;
    }

    while (1) {
        lfs_ssize_t nread = lfs_file_read(lfs, &in, buffer, sizeof(buffer));
        if (nread < 0) {
            fprintf(stderr, "lfs_file_read failed for %s: %d\n", lfs_path,
                    (int)nread);
            (void)lfs_file_close(lfs, &in);
            fclose(out);
            return -1;
        }
        if (nread == 0) {
            break;
        }
        if (fwrite(buffer, 1, (size_t)nread, out) != (size_t)nread) {
            fprintf(stderr, "fwrite failed for %s\n", host_path);
            (void)lfs_file_close(lfs, &in);
            fclose(out);
            return -1;
        }
    }

    err = lfs_file_close(lfs, &in);
    fclose(out);
    if (err != 0) {
        fprintf(stderr, "lfs_file_close failed for %s: %d\n", lfs_path, err);
        return -1;
    }

    return 0;
}

static int unpack_recursive(lfs_t *lfs, const char *lfs_dir_path,
                            const char *host_dir)
{
    lfs_dir_t dir;
    struct lfs_info info;
    int err;

    err = lfs_dir_open(lfs, &dir, lfs_dir_path);
    if (err != 0) {
        fprintf(stderr, "lfs_dir_open failed for %s: %d\n", lfs_dir_path,
                err);
        return -1;
    }

    while (1) {
        char child_lfs[PATH_MAX];
        char child_host[PATH_MAX];
        int n = lfs_dir_read(lfs, &dir, &info);

        if (n < 0) {
            fprintf(stderr, "lfs_dir_read failed for %s: %d\n", lfs_dir_path,
                    n);
            (void)lfs_dir_close(lfs, &dir);
            return -1;
        }
        if (n == 0) {
            break;
        }

        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        if (join_lfs_path(child_lfs, sizeof(child_lfs), lfs_dir_path,
                          info.name) != 0 ||
            join_host_path(child_host, sizeof(child_host), host_dir,
                           info.name) != 0) {
            (void)lfs_dir_close(lfs, &dir);
            return -1;
        }

        if (info.type == LFS_TYPE_DIR) {
            if (mkdir_p(child_host) != 0) {
                fprintf(stderr, "mkdir failed: %s\n", child_host);
                (void)lfs_dir_close(lfs, &dir);
                return -1;
            }
            if (unpack_recursive(lfs, child_lfs, child_host) != 0) {
                (void)lfs_dir_close(lfs, &dir);
                return -1;
            }
        } else if (info.type == LFS_TYPE_REG) {
            if (export_file_from_lfs(lfs, child_lfs, child_host) != 0) {
                (void)lfs_dir_close(lfs, &dir);
                return -1;
            }
        }
    }

    err = lfs_dir_close(lfs, &dir);
    if (err != 0) {
        fprintf(stderr, "lfs_dir_close failed for %s: %d\n", lfs_dir_path,
                err);
        return -1;
    }

    return 0;
}

static int cmd_create(const cli_opts_t *opts)
{
    lfs_ctx_t ctx;
    struct stat st;
    int err;

    if (stat(opts->cmd_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "--create source is not a directory: %s\n",
                opts->cmd_path);
        return 1;
    }

    if (lfs_ctx_open(&ctx, opts, opts->out_path, true) != 0) {
        return 1;
    }

    err = lfs_format(&ctx.lfs, &ctx.cfg);
    if (err != 0) {
        fprintf(stderr, "lfs_format failed: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    err = lfs_mount(&ctx.lfs, &ctx.cfg);
    if (err != 0) {
        fprintf(stderr, "lfs_mount failed after format: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    if (pack_dir_recursive(&ctx.lfs, opts->cmd_path, "") != 0) {
        (void)lfs_unmount(&ctx.lfs);
        lfs_ctx_close(&ctx);
        return 1;
    }

    err = lfs_unmount(&ctx.lfs);
    if (err != 0) {
        fprintf(stderr, "lfs_unmount failed: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    lfs_ctx_close(&ctx);
    return 0;
}

static int cmd_unpack(const cli_opts_t *opts)
{
    lfs_ctx_t ctx;
    int err;

    if (mkdir_p(opts->out_path) != 0) {
        fprintf(stderr, "Cannot create output directory: %s\n", opts->out_path);
        return 1;
    }

    if (lfs_ctx_open(&ctx, opts, opts->cmd_path, false) != 0) {
        return 1;
    }

    err = lfs_mount(&ctx.lfs, &ctx.cfg);
    if (err != 0) {
        fprintf(stderr, "lfs_mount failed: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    if (unpack_recursive(&ctx.lfs, ".", opts->out_path) != 0) {
        (void)lfs_unmount(&ctx.lfs);
        lfs_ctx_close(&ctx);
        return 1;
    }

    err = lfs_unmount(&ctx.lfs);
    if (err != 0) {
        fprintf(stderr, "lfs_unmount failed: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    lfs_ctx_close(&ctx);
    return 0;
}

static int cmd_ls(const cli_opts_t *opts)
{
    lfs_ctx_t ctx;
    lfs_dir_t dir;
    struct lfs_info info;
    char lfs_path[PATH_MAX];
    int err;

    normalize_lfs_path(opts->cmd_path, lfs_path, sizeof(lfs_path));

    if (lfs_ctx_open(&ctx, opts, opts->in_image, false) != 0) {
        return 1;
    }

    err = lfs_mount(&ctx.lfs, &ctx.cfg);
    if (err != 0) {
        fprintf(stderr, "lfs_mount failed: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    err = lfs_dir_open(&ctx.lfs, &dir, lfs_path);
    if (err != 0) {
        fprintf(stderr, "lfs_dir_open failed for %s: %d\n", lfs_path, err);
        (void)lfs_unmount(&ctx.lfs);
        lfs_ctx_close(&ctx);
        return 1;
    }

    while (1) {
        int n = lfs_dir_read(&ctx.lfs, &dir, &info);
        if (n < 0) {
            fprintf(stderr, "lfs_dir_read failed: %d\n", n);
            (void)lfs_dir_close(&ctx.lfs, &dir);
            (void)lfs_unmount(&ctx.lfs);
            lfs_ctx_close(&ctx);
            return 1;
        }
        if (n == 0) {
            break;
        }
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        if (info.type == LFS_TYPE_DIR) {
            printf("d %s\n", info.name);
        } else {
            printf("f %10u %s\n", (unsigned)info.size, info.name);
        }
    }

    (void)lfs_dir_close(&ctx.lfs, &dir);
    (void)lfs_unmount(&ctx.lfs);
    lfs_ctx_close(&ctx);
    return 0;
}

static int tree_recursive(lfs_t *lfs, const char *path, int depth)
{
    lfs_dir_t dir;
    struct lfs_info info;
    int err;

    err = lfs_dir_open(lfs, &dir, path);
    if (err != 0) {
        fprintf(stderr, "lfs_dir_open failed for %s: %d\n", path, err);
        return -1;
    }

    while (1) {
        char child[PATH_MAX];
        int n = lfs_dir_read(lfs, &dir, &info);

        if (n < 0) {
            fprintf(stderr, "lfs_dir_read failed for %s: %d\n", path, n);
            (void)lfs_dir_close(lfs, &dir);
            return -1;
        }

        if (n == 0) {
            break;
        }

        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        for (int i = 0; i < depth; i++) {
            printf("  ");
        }

        if (info.type == LFS_TYPE_DIR) {
            printf("%s/\n", info.name);
            if (join_lfs_path(child, sizeof(child), path, info.name) != 0) {
                (void)lfs_dir_close(lfs, &dir);
                return -1;
            }
            if (tree_recursive(lfs, child, depth + 1) != 0) {
                (void)lfs_dir_close(lfs, &dir);
                return -1;
            }
        } else {
            printf("%s\n", info.name);
        }
    }

    err = lfs_dir_close(lfs, &dir);
    if (err != 0) {
        fprintf(stderr, "lfs_dir_close failed for %s: %d\n", path, err);
        return -1;
    }

    return 0;
}

static int cmd_tree(const cli_opts_t *opts)
{
    lfs_ctx_t ctx;
    char lfs_path[PATH_MAX];
    int err;

    normalize_lfs_path(opts->cmd_path, lfs_path, sizeof(lfs_path));

    if (lfs_ctx_open(&ctx, opts, opts->in_image, false) != 0) {
        return 1;
    }

    err = lfs_mount(&ctx.lfs, &ctx.cfg);
    if (err != 0) {
        fprintf(stderr, "lfs_mount failed: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    if (strcmp(lfs_path, ".") == 0) {
        printf("./\n");
    } else {
        printf("%s/\n", lfs_path);
    }

    if (tree_recursive(&ctx.lfs, lfs_path, 1) != 0) {
        (void)lfs_unmount(&ctx.lfs);
        lfs_ctx_close(&ctx);
        return 1;
    }

    (void)lfs_unmount(&ctx.lfs);
    lfs_ctx_close(&ctx);
    return 0;
}

static int cmd_cat(const cli_opts_t *opts)
{
    lfs_ctx_t ctx;
    lfs_file_t file;
    char lfs_path[PATH_MAX];
    uint8_t buffer[IO_BUFFER_SIZE];
    int err;

    normalize_lfs_path(opts->cmd_path, lfs_path, sizeof(lfs_path));

    if (lfs_ctx_open(&ctx, opts, opts->in_image, false) != 0) {
        return 1;
    }

    err = lfs_mount(&ctx.lfs, &ctx.cfg);
    if (err != 0) {
        fprintf(stderr, "lfs_mount failed: %d\n", err);
        lfs_ctx_close(&ctx);
        return 1;
    }

    err = lfs_file_open(&ctx.lfs, &file, lfs_path, LFS_O_RDONLY);
    if (err != 0) {
        fprintf(stderr, "lfs_file_open failed for %s: %d\n", lfs_path, err);
        (void)lfs_unmount(&ctx.lfs);
        lfs_ctx_close(&ctx);
        return 1;
    }

    while (1) {
        lfs_ssize_t nread = lfs_file_read(&ctx.lfs, &file, buffer, sizeof(buffer));
        if (nread < 0) {
            fprintf(stderr, "lfs_file_read failed for %s: %d\n", lfs_path,
                    (int)nread);
            (void)lfs_file_close(&ctx.lfs, &file);
            (void)lfs_unmount(&ctx.lfs);
            lfs_ctx_close(&ctx);
            return 1;
        }
        if (nread == 0) {
            break;
        }

        if (fwrite(buffer, 1, (size_t)nread, stdout) != (size_t)nread) {
            fprintf(stderr, "stdout write failed\n");
            (void)lfs_file_close(&ctx.lfs, &file);
            (void)lfs_unmount(&ctx.lfs);
            lfs_ctx_close(&ctx);
            return 1;
        }
    }

    (void)lfs_file_close(&ctx.lfs, &file);
    (void)lfs_unmount(&ctx.lfs);
    lfs_ctx_close(&ctx);
    return 0;
}

int main(int argc, char **argv)
{
    cli_opts_t opts;
    int parse_rc = parse_args(argc, argv, &opts);

    if (parse_rc == 1) {
        print_usage(argv[0]);
        return 0;
    }

    if (parse_rc != 0) {
        print_usage(argv[0]);
        return 1;
    }

    switch (opts.cmd) {
    case CMD_CREATE:
        return cmd_create(&opts);
    case CMD_UNPACK:
        return cmd_unpack(&opts);
    case CMD_LS:
        return cmd_ls(&opts);
    case CMD_TREE:
        return cmd_tree(&opts);
    case CMD_CAT:
        return cmd_cat(&opts);
    default:
        print_usage(argv[0]);
        return 1;
    }
}
