#include "../command.h"
#include "../core/object.h"
#include "../core/index.h"
#include "../core/ignore.h"
#include "../base/file.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/*
 * mgit add <file> [<file>...] | . | -A
 * 
 * 将文件添加到暂存区（Index）
 * 
 * 支持:
 * - mgit add file1 file2   添加指定文件
 * - mgit add .             添加当前目录所有文件
 * - mgit add -A            添加所有文件，并暂存已跟踪文件的删除
 */

static void add_help(void) {
    printf("usage: mgit add <file> [<file>...] | . | -A\n\n");
    printf("Add file contents to the staging area (index).\n\n");
    printf("Options:\n");
    printf("    <file>    Add specific file(s)\n");
    printf("    .         Add all files in current directory\n");
    printf("    -A        Add all files and stage deletions of tracked files\n");
}

/* 路径规范化：Windows 反斜杠 → 正斜杠（保证 Index/tree 路径一致） */
static void path_normalize(char *path) {
    for (char *p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* 添加单个文件到 index */
static int add_single_file(ObjectStore *store, Index *idx, const char *file) {
    if (!file_exists(file) || file_is_dir(file)) {
        return -1;
    }

    uint8_t *data;
    size_t size;
    if (file_read_all(file, &data, &size) != 0) {
        return -1;
    }

    Hash blob_hash;
    if (object_store_write(store, OBJ_BLOB, data, size, &blob_hash) != 0) {
        free(data);
        return -1;
    }
    free(data);

    uint32_t mode = 0100644;
    if (index_add(idx, file, &blob_hash, mode) != 0) {
        return -1;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&blob_hash, hex);
    printf("add '%s' (blob %s)\n", file, hex);
    return 0;
}

/* 递归添加目录下所有文件（跳过 .git 和 .gitignore 匹配的文件） */
static int add_all_files(ObjectStore *store, Index *idx, const char *dir_path,
                         const IgnoreList *ign) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过 . .. .git */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".git") == 0) {
            continue;
        }

        char full[1024];
        if (strcmp(dir_path, ".") == 0) {
            snprintf(full, sizeof(full), "%s", entry->d_name);
        } else {
            snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);
        }

        if (file_is_dir(full)) {
            /* 被忽略的目录 → 整个跳过 */
            if (ignore_is_ignored(ign, full, 1)) continue;
            /* 递归进入子目录 */
            int sub = add_all_files(store, idx, full, ign);
            if (sub > 0) count += sub;
            continue;
        }

        if (ignore_is_ignored(ign, full, 0)) continue;

        if (add_single_file(store, idx, full) == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* -A 专用：暂存删除（工作区已不存在的已跟踪文件从 Index 移除） */
static int add_stage_deletions(Index *idx) {
    int removed = 0;
    size_t i = 0;
    while (i < idx->count) {
        if (!file_exists(idx->entries[i].name)) {
            printf("remove '%s' (deleted)\n", idx->entries[i].name);
            if (index_remove(idx, idx->entries[i].name) == 0) {
                removed++;
                continue;  /* 后续条目已前移，不递增 i */
            }
        }
        i++;
    }
    return removed;
}

static int add_run(int argc, char **argv) {
    if (argc < 2) {
        mgit_error("no files specified");
        add_help();
        return -1;
    }

    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("not a git repository");
        return -1;
    }

    Index *idx = index_open(".git");
    if (!idx) {
        object_store_close(store);
        mgit_error("cannot open index");
        return -1;
    }

    /* 加载 .gitignore */
    IgnoreList ign;
    ignore_load(&ign);

    int errors = 0;

    for (int i = 1; i < argc; i++) {
        char argbuf[1024];
        snprintf(argbuf, sizeof(argbuf), "%s", argv[i]);
        path_normalize(argbuf);
        const char *arg = argbuf;

        /* 跳过选项 */
        if (arg[0] == '-' && strcmp(arg, "-A") != 0) {
            continue;
        }

        /* . 或 -A：添加所有文件（-A 额外暂存删除） */
        if (strcmp(arg, ".") == 0 || strcmp(arg, "-A") == 0) {
            int count = add_all_files(store, idx, ".", &ign);
            if (count < 0) {
                mgit_error("failed to scan directory");
                errors++;
            }
            if (strcmp(arg, "-A") == 0) {
                add_stage_deletions(idx);
            }
            continue;
        }

        /* 目录参数：递归添加（子目录支持） */
        if (file_exists(arg) && file_is_dir(arg)) {
            int count = add_all_files(store, idx, arg, &ign);
            if (count < 0) {
                mgit_error("failed to scan directory '%s'", arg);
                errors++;
            }
            continue;
        }

        /* 指定文件：被忽略的文件拒绝添加 */
        if (ignore_is_ignored(&ign, arg, 0)) {
            printf("skipped ignored file '%s'\n", arg);
            continue;
        }
        if (add_single_file(store, idx, arg) != 0) {
            mgit_error("pathspec '%s' did not match any files", arg);
            errors++;
        }
    }

    /* 保存 Index */
    if (idx->dirty) {
        if (index_write(idx) != 0) {
            mgit_error("failed to write index");
            errors++;
        }
    }

    index_close(idx);
    object_store_close(store);
    return errors > 0 ? -1 : 0;
}

Command cmd_add = {
    .name = "add",
    .description = "Add file contents to the staging area",
    .run = add_run,
    .help = add_help
};
