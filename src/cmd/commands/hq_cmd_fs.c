#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hq_cmd.h"
#include "osal_dir.h"
#include "osal_file.h"
#include "osal_mount.h"

#define CMD_PATH_MAX 256

static bool g_fs_mounted = false;
static char g_fs_mount_point[CMD_PATH_MAX] = {0};
static char g_fs_devname[CMD_PATH_MAX] = {0};
static char g_out[512];

static void print_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(g_out, sizeof(g_out), fmt, ap);
    va_end(ap);
    hq_cmd_print(g_out);
}

static const char *default_mount_point(void)
{
#if CONFIG_HQ_PLATFORM_POSIX
    return "/";
#else
    return "/littlefs";
#endif
}

static int32_t copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || src == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (strlen(src) >= dst_size)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    return OSAL_SUCCESS;
}

static int32_t ensure_mounted(void)
{
    return g_fs_mounted ? OSAL_SUCCESS : OSAL_ERR_INCORRECT_OBJ_STATE;
}

static int32_t join_path(const char *base, const char *name, char *out_path, size_t out_size)
{
    int n = 0;

    if (base == NULL || name == NULL || out_path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    if (strcmp(base, "/") == 0)
    {
        n = snprintf(out_path, out_size, "/%s", name);
    }
    else
    {
        n = snprintf(out_path, out_size, "%s/%s", base, name);
    }

    if (n < 0 || (size_t)n >= out_size)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }

    return OSAL_SUCCESS;
}

static void print_indent(int depth)
{
    for (int i = 0; i < depth; ++i)
    {
        hq_cmd_print("  ");
    }
}

static int32_t list_dir_impl(const char *path, bool recursive, int depth)
{
    osal_dir_id_t dir = osal_dir_open(path);
    if (dir < 0)
    {
        return (int32_t)dir;
    }

    int32_t rc;
    osal_dirent_t entry;

    while ((rc = osal_dir_read(dir, &entry)) == OSAL_SUCCESS)
    {
        print_indent(depth);
        print_line("%s%s", entry.name, (entry.type == OSAL_DIRENT_TYPE_DIR) ? "/" : "");

        if (recursive && entry.type == OSAL_DIRENT_TYPE_DIR)
        {
            char child[CMD_PATH_MAX];
            if (join_path(path, entry.name, child, sizeof(child)) == OSAL_SUCCESS)
            {
                int32_t child_rc = list_dir_impl(child, true, depth + 1);
                if (child_rc != OSAL_SUCCESS)
                {
                    (void)osal_dir_close(dir);
                    return child_rc;
                }
            }
            else
            {
                (void)osal_dir_close(dir);
                return OSAL_FS_ERR_PATH_TOO_LONG;
            }
        }
    }

    (void)osal_dir_close(dir);

    if (rc == OSAL_ERR_EMPTY_SET)
    {
        return OSAL_SUCCESS;
    }

    return rc;
}

static void cmd_mount_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    const char *devname = hq_cmd_get_token(args, 1);
    const char *mount_point = hq_cmd_get_token(args, 2);

    if (devname == NULL)
    {
        hq_cmd_print("Usage: mount <devname> [mount_point]");
        return;
    }

    if (mount_point == NULL)
    {
        mount_point = default_mount_point();
    }

    int32_t rc = osal_mount(devname, mount_point);
    if (rc != OSAL_SUCCESS)
    {
        print_line("mount failed: %d", (int)rc);
        return;
    }

    g_fs_mounted = true;
    (void)copy_text(g_fs_devname, sizeof(g_fs_devname), devname);
    (void)copy_text(g_fs_mount_point, sizeof(g_fs_mount_point), mount_point);
    print_line("mounted %s at %s", devname, mount_point);
}

static void cmd_umount_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)args;
    (void)context;

    if (!g_fs_mounted)
    {
        hq_cmd_print("filesystem is not mounted");
        return;
    }

    int32_t rc = osal_unmount(g_fs_mount_point[0] ? g_fs_mount_point : default_mount_point());
    if (rc != OSAL_SUCCESS)
    {
        print_line("umount failed: %d", (int)rc);
        return;
    }

    g_fs_mounted = false;
    g_fs_mount_point[0] = '\0';
    g_fs_devname[0] = '\0';
    hq_cmd_print("unmounted");
}

static void cmd_ls_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    if (ensure_mounted() != OSAL_SUCCESS)
    {
        hq_cmd_print("filesystem is not mounted");
        return;
    }

    const char *path = hq_cmd_get_token(args, 1);
    if (path == NULL)
    {
        path = "/";
    }

    int32_t rc = list_dir_impl(path, false, 0);
    if (rc != OSAL_SUCCESS)
    {
        print_line("ls failed: %d", (int)rc);
    }
}

static void cmd_tree_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    if (ensure_mounted() != OSAL_SUCCESS)
    {
        hq_cmd_print("filesystem is not mounted");
        return;
    }

    const char *path = hq_cmd_get_token(args, 1);
    if (path == NULL)
    {
        path = "/";
    }

    int32_t rc = list_dir_impl(path, true, 0);
    if (rc != OSAL_SUCCESS)
    {
        print_line("tree failed: %d", (int)rc);
    }
}

static void cmd_mkdir_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    const char *path = hq_cmd_get_token(args, 1);
    if (path == NULL)
    {
        hq_cmd_print("Usage: mkdir <path>");
        return;
    }

    int32_t rc = osal_mkdir(path);
    if (rc != OSAL_SUCCESS)
    {
        print_line("mkdir failed: %d", (int)rc);
        return;
    }

    print_line("mkdir ok: %s", path);
}

static void cmd_rmdir_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    const char *path = hq_cmd_get_token(args, 1);
    if (path == NULL)
    {
        hq_cmd_print("Usage: rmdir <path>");
        return;
    }

    int32_t rc = osal_rmdir(path);
    if (rc != OSAL_SUCCESS)
    {
        print_line("rmdir failed: %d", (int)rc);
        return;
    }

    print_line("rmdir ok: %s", path);
}

static void cmd_stat_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    const char *path = hq_cmd_get_token(args, 1);
    if (path == NULL)
    {
        hq_cmd_print("Usage: stat <path>");
        return;
    }

    osal_fstat_t st;
    int32_t rc = osal_stat(path, &st);
    if (rc != OSAL_SUCCESS)
    {
        print_line("stat failed: %d", (int)rc);
        return;
    }

    print_line("Path: %s", path);
    print_line("Type: %s", OSAL_FILESTAT_ISDIR(st) ? "dir" : "file");
    print_line("Size: %u", (unsigned)OSAL_FILESTAT_SIZE(st));
    print_line("Mode: 0x%08x", (unsigned)OSAL_FILESTAT_MODE(st));
}

static void cmd_rm_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    const char *path = hq_cmd_get_token(args, 1);
    if (path == NULL)
    {
        hq_cmd_print("Usage: rm <path>");
        return;
    }

    int32_t rc = osal_remove(path);
    if (rc != OSAL_SUCCESS)
    {
        print_line("rm failed: %d", (int)rc);
        return;
    }

    print_line("removed %s", path);
}

static void cmd_cp_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    const char *src = hq_cmd_get_token(args, 1);
    const char *dst = hq_cmd_get_token(args, 2);
    if (src == NULL || dst == NULL)
    {
        hq_cmd_print("Usage: cp <src> <dest>");
        return;
    }

    int32_t rc = osal_cp(src, dst);
    if (rc != OSAL_SUCCESS)
    {
        print_line("cp failed: %d", (int)rc);
        return;
    }

    print_line("copied %s -> %s", src, dst);
}

static void cmd_mv_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    const char *src = hq_cmd_get_token(args, 1);
    const char *dst = hq_cmd_get_token(args, 2);
    if (src == NULL || dst == NULL)
    {
        hq_cmd_print("Usage: mv <src> <dest>");
        return;
    }

    int32_t rc = osal_mv(src, dst);
    if (rc != OSAL_SUCCESS)
    {
        print_line("mv failed: %d", (int)rc);
        return;
    }

    print_line("moved %s -> %s", src, dst);
}

void hq_cmd_fs_register(void)
{
    const hq_cmd_binding_t bindings[] = {
        {
            .name = "mount",
            .help = "Mount filesystem: mount <devname> [mount_point]",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_mount_handler,
        },
        {
            .name = "umount",
            .help = "Unmount filesystem",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_umount_handler,
        },
        {
            .name = "ls",
            .help = "List directory: ls [path]",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_ls_handler,
        },
        {
            .name = "tree",
            .help = "List directory tree: tree [path]",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_tree_handler,
        },
        {
            .name = "mkdir",
            .help = "Create directory: mkdir <path>",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_mkdir_handler,
        },
        {
            .name = "rmdir",
            .help = "Remove empty directory: rmdir <path>",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_rmdir_handler,
        },
        {
            .name = "stat",
            .help = "Show path metadata: stat <path>",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_stat_handler,
        },
        {
            .name = "rm",
            .help = "Remove file: rm <path>",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_rm_handler,
        },
        {
            .name = "cp",
            .help = "Copy file: cp <src> <dest>",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_cp_handler,
        },
        {
            .name = "mv",
            .help = "Move file: mv <src> <dest>",
            .tokenize_args = true,
            .context = NULL,
            .handler = cmd_mv_handler,
        },
    };

    for (size_t i = 0; i < (sizeof(bindings) / sizeof(bindings[0])); ++i)
    {
        (void)hq_cmd_register(&bindings[i]);
    }
}
