#include "../command.h"
#include "../core/object.h"
#include "../core/index.h"
#include "../core/ref.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/ignore.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/*
 * mgit status
 * 
 * 显示工作区和暂存区状态
 * 
 * 展示：
 * - 当前分支
 * - 已暂存的变更（Index 中的内容）
 * - 未暂存的变更（工作区 vs Index）
 */

static void status_help(void) {
    printf("usage: mgit status\n\n");
    printf("Show the working tree status.\n");
}

/* Index 中是否存在以 "prefix/" 开头的条目 */
static int index_has_prefix(Index *idx, const char *prefix) {
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < idx->count; i++) {
        if (strncmp(idx->entries[i].name, prefix, plen) == 0 &&
            idx->entries[i].name[plen] == '/') {
            return 1;
        }
    }
    return 0;
}

/* 递归扫描目录，打印未跟踪文件（整个目录未跟踪时只显示目录名，跳过 .gitignore 匹配项） */
static void status_scan_untracked(const char *dir_path, Index *idx,
                                  const IgnoreList *ign, int *has_untracked);

static void status_scan_untracked(const char *dir_path, Index *idx,
                                  const IgnoreList *ign, int *has_untracked) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".git") == 0) {
            continue;
        }

        /* 拼相对仓库根的路径 */
        char rel[1024];
        char full[1050];
        if (strcmp(dir_path, ".") == 0) {
            snprintf(rel, sizeof(rel), "%s", entry->d_name);
            snprintf(full, sizeof(full), "%s", entry->d_name);
        } else {
            snprintf(rel, sizeof(rel), "%s/%s", dir_path, entry->d_name);
            snprintf(full, sizeof(full), "%s", rel);
        }

        if (file_is_dir(full)) {
            /* 被忽略的目录 → 不显示 */
            if (ignore_is_ignored(ign, rel, 1)) continue;
            /* 目录：若 Index 中无任何该前缀条目，整个目录未跟踪 */
            if (!index_has_prefix(idx, rel)) {
                printf("  \033[31m%s/\033[0m\n", rel);
                *has_untracked = 1;
            } else {
                status_scan_untracked(full, idx, ign, has_untracked);
            }
        } else {
            if (ignore_is_ignored(ign, rel, 0)) continue;
            if (!index_find(idx, rel)) {
                printf("  \033[31m%s\033[0m\n", rel);
                *has_untracked = 1;
            }
        }
    }
    closedir(dir);
}

static int status_run(int argc, char **argv) {
    (void)argc;
    (void)argv;

    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("not a git repository");
        return -1;
    }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        object_store_close(store);
        return -1;
    }

    Index *idx = index_open(".git");
    if (!idx) {
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 显示 HEAD 状态：符号引用属于分支；直接引用属于 detached HEAD。 */
    char branch[256];
    if (ref_get_head_branch(refs, branch, sizeof(branch)) == 0 &&
        strcmp(branch, "HEAD") != 0) {
        const char *name = branch;
        if (strncmp(branch, "refs/heads/", 11) == 0) {
            name = branch + 11;
        }
        printf("On branch %s\n", name);
    } else {
        printf("Not on any branch (detached HEAD)\n");
    }

    /* 显示已暂存的文件（比较 Index 与 HEAD 的 tree） */
    int has_staged = 0;

    /* 读取 HEAD commit 的 tree */
    Tree head_tree = {0};
    int has_head_tree = 0;
    Hash head_hash;
    if (ref_resolve_head(refs, &head_hash) == 0) {
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &head_hash, &obj) == 0) {
            if (obj.type == OBJ_COMMIT) {
                Commit commit;
                memset(&commit, 0, sizeof(commit));
                commit_parse(obj.data, obj.size, &commit);

                Object tree_obj;
                memset(&tree_obj, 0, sizeof(tree_obj));
                if (object_store_read(store, &commit.tree, &tree_obj) == 0) {
                    if (tree_obj.type == OBJ_TREE) {
                        tree_parse(tree_obj.data, tree_obj.size, &head_tree);
                        has_head_tree = 1;
                    }
                    object_free(&tree_obj);
                }
                commit_free(&commit);
            }
            object_free(&obj);
        }
    }

    /* 扁平化 HEAD tree（子目录支持） */
    TreeFlatEntry *head_flat = NULL;
    size_t head_flat_c = 0;
    if (has_head_tree) {
        tree_flatten(store, &head_tree, &head_flat, &head_flat_c);
    }

    /* 比较 Index 与 HEAD tree */
    if (idx->count > 0) {
        for (size_t i = 0; i < idx->count; i++) {
            IndexEntry *entry = &idx->entries[i];
            const char *status_str = NULL;

            if (has_head_tree) {
                /* 在 HEAD tree 扁平列表中查找同名文件 */
                TreeFlatEntry *found = tree_flat_find(head_flat, head_flat_c, entry->name);
                if (!found) {
                    status_str = "new file";
                } else if (!hash_equal(&found->hash, &entry->hash)) {
                    status_str = "modified";
                }
            } else {
                status_str = "new file";
            }

            if (status_str) {
                if (!has_staged) {
                    printf("\nChanges to be committed:\n");
                    has_staged = 1;
                }
                printf("  \033[32m%-12s\033[0m %s\n", status_str, entry->name);
            }
        }
    }

    if (has_head_tree) {
        /* 检查 HEAD tree 中有但 Index 中没有的文件（已删除） */
        for (size_t j = 0; j < head_flat_c; j++) {
            if (!index_find(idx, head_flat[j].path)) {
                if (!has_staged) {
                    printf("\nChanges to be committed:\n");
                    has_staged = 1;
                }
                printf("  \033[31m%-12s\033[0m %s\n", "deleted", head_flat[j].path);
            }
        }
        free(head_flat);
        tree_free(&head_tree);
    }

    /* 检查工作区中未暂存的变更 */
    printf("\nChanges not staged for commit:\n");
    int has_changes = 0;

    /* 检查 Index 中的文件是否被修改 */
    for (size_t i = 0; i < idx->count; i++) {
        uint8_t *data;
        size_t size;
        if (file_read_all(idx->entries[i].name, &data, &size) == 0) {
            Hash current_hash;
            object_hash(OBJ_BLOB, data, size, &current_hash);
            free(data);

            if (!hash_equal(&current_hash, &idx->entries[i].hash)) {
                printf("  \033[31m%-12s\033[0m %s\n", "modified", idx->entries[i].name);
                has_changes = 1;
            }
        } else {
            /* 已跟踪文件在工作区被删除 */
            printf("  \033[31m%-12s\033[0m %s\n", "deleted", idx->entries[i].name);
            has_changes = 1;
        }
    }

    /* 检查未跟踪的文件（递归子目录，遵守 .gitignore） */
    printf("\nUntracked files:\n");
    int has_untracked = 0;
    IgnoreList ign;
    ignore_load(&ign);
    status_scan_untracked(".", idx, &ign, &has_untracked);

    if (!has_changes && !has_untracked && !has_staged) {
        printf("\nnothing to commit, working tree clean\n");
    }

    index_close(idx);
    ref_manager_close(refs);
    object_store_close(store);
    return 0;
}

Command cmd_status = {
    .name = "status",
    .description = "Show the working tree status",
    .run = status_run,
    .help = status_help
};
