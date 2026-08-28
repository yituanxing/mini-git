#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/ref.h"
#include "../core/index.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * mgit stash [push|pop|list|drop]
 * 
 * 临时保存工作区修改
 * 
 * 教学语义：
 * - stash 保存已跟踪文件在 Working Tree + Index 中的本地修改
 * - push 后 Working Tree 和 Index 回到 HEAD
 * - 默认不包含 untracked（真实 Git 需 -u 才包含）
 *
 * 实现简化：
 * - mgit 用普通 commit 保存快照，并把多个 hash 逐行写在 .git/refs/stash
 * - 真实 Git 的 refs/stash 是一个 ref，旧条目放在该 ref 的 reflog 中，
 *   stash commit 的内部结构也更丰富；这里不复刻这些细节。
 */

static void stash_help(void) {
    printf("usage: mgit stash [push|pop|list|drop]\n\n");
    printf("Stash changes in the working directory.\n\n");
    printf("Commands:\n");
    printf("    push    Save current changes and reset working directory\n");
    printf("    pop     Restore the most recent stash\n");
    printf("    list    Show all stashes\n");
    printf("    drop    Remove the most recent stash\n");
}

/* 获取 stash 文件路径 */
static void stash_path(char *buf, size_t size) {
    snprintf(buf, size, ".git/refs/stash");
}

/* 读取 stash 列表（每行一个 hash） */
static int stash_read_list(Hash *list, int max_count) {
    char path[256];
    stash_path(path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;  /* 没有 stash */

    int count = 0;
    char line[128];
    while (fgets(line, sizeof(line), fp) && count < max_count) {
        /* 去掉换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = 0;
        }
        if (len == 40) {
            hex_to_hash(line, &list[count]);
            count++;
        }
    }
    fclose(fp);
    return count;
}

/* 保存 stash 列表 */
static int stash_write_list(Hash *list, int count) {
    char path[256];
    stash_path(path, sizeof(path));

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    for (int i = 0; i < count; i++) {
        char hex[HASH_HEX_SIZE];
        hash_to_hex(&list[i], hex);
        fprintf(fp, "%s\n", hex);
    }
    fclose(fp);
    return 0;
}

/* 把已跟踪文件的 Working Tree 修改/删除折叠进 Index；不碰 untracked。 */
static void auto_stage_tracked_changes(Index *idx, ObjectStore *store) {
    size_t i = 0;
    while (i < idx->count) {
        IndexEntry *entry = &idx->entries[i];

        if (!file_exists(entry->name)) {
            char path[1024];
            snprintf(path, sizeof(path), "%s", entry->name);
            index_remove(idx, path);
            continue;  /* 后续条目已前移 */
        }

        uint8_t *data = NULL;
        size_t size = 0;
        if (file_read_all(entry->name, &data, &size) != 0) {
            i++;
            continue;
        }

        Hash current_hash;
        object_hash(OBJ_BLOB, data, size, &current_hash);
        if (!hash_equal(&current_hash, &entry->hash)) {
            Hash new_hash;
            if (object_store_write(store, OBJ_BLOB, data, size, &new_hash) == 0) {
                entry->hash = new_hash;
                idx->dirty = 1;
            }
        }
        free(data);
        i++;
    }
}

static int stash_push(int argc, char **argv) {
    const char *message = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[++i];
        }
    }

    ObjectStore *store = object_store_open(".git");
    if (!store) { mgit_error("not a git repository"); return -1; }

    Index *idx = index_open(".git");
    if (!idx) { object_store_close(store); return -1; }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) { index_close(idx); object_store_close(store); return -1; }

    /* 1. 把已跟踪的 Working Tree 修改/删除并入要保存的快照。 */
    auto_stage_tracked_changes(idx, store);

    /* 2. 从当前 Index 创建 stash tree。 */
    Hash tree_hash;
    if (index_write_tree(idx, store, &tree_hash) != 0) {
        mgit_error("failed to create tree from index");
        goto error;
    }

    /* 3. 创建 stash commit（保存当前 HEAD 作为 parent） */
    Hash parent_hash;
    Hash *parent_ptr = NULL;
    if (ref_resolve_head_quiet(refs, &parent_hash) == 0) {
        parent_ptr = &parent_hash;
    }

    /*
     * 干净仓库不能产生 stash：比较将要保存的 tree 与 HEAD tree。
     * untracked 不在 Index 中，因此也自然不会单独触发 stash。
     */
    if (parent_ptr) {
        Commit head_commit;
        memset(&head_commit, 0, sizeof(head_commit));
        if (commit_read(store, &parent_hash, &head_commit) == 0) {
            int clean = hash_equal(&tree_hash, &head_commit.tree);
            commit_free(&head_commit);
            if (clean) {
                printf("No local changes to save.\n");
                ref_manager_close(refs);
                index_close(idx);
                object_store_close(store);
                return 0;
            }
        }
    }

    char msg[256];
    if (message) {
        snprintf(msg, sizeof(msg), "WIP: %s", message);
    } else {
        snprintf(msg, sizeof(msg), "WIP on mgit stash");
    }

    Hash stash_commit;
    if (commit_create(store, &tree_hash, parent_ptr, msg, &stash_commit) != 0) {
        mgit_error("failed to create stash commit");
        goto error;
    }

    /* 4. 压入 stash 栈 */
    Hash stash_list[64];
    int count = stash_read_list(stash_list, 64);

    /* 新的 stash 放在最前面 */
    if (count < 64) {
        for (int i = count; i > 0; i--) {
            stash_list[i] = stash_list[i - 1];
        }
        stash_list[0] = stash_commit;
        count++;
    }
    stash_write_list(stash_list, count);

    /* 5. 重置 Index 和工作区为 HEAD 的 tree */
    if (parent_ptr) {
        /* 读取 HEAD commit 的 tree，同步工作区 + index */
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &parent_hash, &obj) == 0) {
            if (obj.type == OBJ_COMMIT) {
                Commit commit;
                memset(&commit, 0, sizeof(commit));
                commit_parse(obj.data, obj.size, &commit);

                Object tree_obj;
                memset(&tree_obj, 0, sizeof(tree_obj));
                if (object_store_read(store, &commit.tree, &tree_obj) == 0) {
                    if (tree_obj.type == OBJ_TREE) {
                        Tree tree = {0};
                        tree_parse(tree_obj.data, tree_obj.size, &tree);
                        tree_restore_worktree(store, idx, &tree);
                        tree_free(&tree);
                    }
                    object_free(&tree_obj);
                }
                commit_free(&commit);
            }
            object_free(&obj);
        }
    } else {
        /* 没有 HEAD：删除所有已跟踪文件，清空 index */
        for (size_t i = 0; i < idx->count; i++) {
            file_delete(idx->entries[i].name);
            free(idx->entries[i].name);
        }
        idx->count = 0;
        idx->dirty = 1;
    }

    index_write(idx);

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&stash_commit, hex);
    printf("Saved working directory to stash (%.7s)\n", hex);

    ref_manager_close(refs);
    index_close(idx);
    object_store_close(store);
    return 0;

error:
    ref_manager_close(refs);
    index_close(idx);
    object_store_close(store);
    return -1;
}

static int stash_pop(void) {
    ObjectStore *store = object_store_open(".git");
    if (!store) { mgit_error("not a git repository"); return -1; }

    Index *idx = index_open(".git");
    if (!idx) { object_store_close(store); return -1; }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) { index_close(idx); object_store_close(store); return -1; }

    /* 读取 stash 列表 */
    Hash stash_list[64];
    int count = stash_read_list(stash_list, 64);

    if (count == 0) {
        printf("No stash found.\n");
        ref_manager_close(refs);
        index_close(idx);
        object_store_close(store);
        return 0;
    }

    /* 取最前面的 stash */
    Hash stash_commit = stash_list[0];

    /* 读取 stash commit 的 tree */
    Object obj;
    memset(&obj, 0, sizeof(obj));
    if (object_store_read(store, &stash_commit, &obj) != 0 || obj.type != OBJ_COMMIT) {
        mgit_error("cannot read stash commit");
        object_free(&obj);
        goto error;
    }

    Commit commit;
    memset(&commit, 0, sizeof(commit));
    commit_parse(obj.data, obj.size, &commit);
    object_free(&obj);

    Object tree_obj;
    memset(&tree_obj, 0, sizeof(tree_obj));
    if (object_store_read(store, &commit.tree, &tree_obj) != 0 || tree_obj.type != OBJ_TREE) {
        mgit_error("cannot read stash tree");
        object_free(&tree_obj);
        commit_free(&commit);
        goto error;
    }

    Tree tree = {0};
    tree_parse(tree_obj.data, tree_obj.size, &tree);
    object_free(&tree_obj);

    /* 恢复工作区和 Index */
    tree_restore_worktree(store, idx, &tree);

    /* 让恢复的修改显示为“未暂存”：Index 重新对齐 HEAD 的 tree */
    Hash head_hash;
    if (ref_resolve_head_quiet(refs, &head_hash) == 0) {
        Object head_obj;
        if (object_store_read(store, &head_hash, &head_obj) == 0 && head_obj.type == OBJ_COMMIT) {
            Commit head_commit;
            memset(&head_commit, 0, sizeof(head_commit));
            commit_parse(head_obj.data, head_obj.size, &head_commit);
            object_free(&head_obj);

            Object head_tree_obj;
            if (object_store_read(store, &head_commit.tree, &head_tree_obj) == 0 &&
                head_tree_obj.type == OBJ_TREE) {
                Tree head_tree = {0};
                tree_parse(head_tree_obj.data, head_tree_obj.size, &head_tree);
                tree_rebuild_index(store, idx, &head_tree);
                tree_free(&head_tree);
                object_free(&head_tree_obj);
            }
            commit_free(&head_commit);
        }
    }

    index_write(idx);

    tree_free(&tree);
    commit_free(&commit);

    /* 从 stash 列表中移除 */
    for (int i = 0; i < count - 1; i++) {
        stash_list[i] = stash_list[i + 1];
    }
    count--;
    stash_write_list(stash_list, count);

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&stash_commit, hex);
    printf("Restored stash (%.7s)\n", hex);

    ref_manager_close(refs);
    index_close(idx);
    object_store_close(store);
    return 0;

error:
    ref_manager_close(refs);
    index_close(idx);
    object_store_close(store);
    return -1;
}

static int stash_list(void) {
    Hash stash_list[64];
    int count = stash_read_list(stash_list, 64);

    if (count == 0) {
        printf("(no stashes)\n");
        return 0;
    }

    for (int i = 0; i < count; i++) {
        char hex[HASH_HEX_SIZE];
        hash_to_hex(&stash_list[i], hex);
        printf("stash@{%d}: %.7s\n", i, hex);
    }
    return 0;
}

static int stash_drop(void) {
    Hash stash_list[64];
    int count = stash_read_list(stash_list, 64);

    if (count == 0) {
        printf("(no stashes to drop)\n");
        return 0;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&stash_list[0], hex);

    for (int i = 0; i < count - 1; i++) {
        stash_list[i] = stash_list[i + 1];
    }
    count--;
    stash_write_list(stash_list, count);

    printf("Dropped stash (%.7s)\n", hex);
    return 0;
}

static int stash_run(int argc, char **argv) {
    const char *subcmd = "push";  /* 默认是 push */

    if (argc >= 2 && argv[1][0] != '-') {
        subcmd = argv[1];
    }

    if (strcmp(subcmd, "push") == 0) {
        return stash_push(argc, argv);
    } else if (strcmp(subcmd, "pop") == 0) {
        return stash_pop();
    } else if (strcmp(subcmd, "list") == 0) {
        return stash_list();
    } else if (strcmp(subcmd, "drop") == 0) {
        return stash_drop();
    } else {
        mgit_error("unknown stash command: %s", subcmd);
        stash_help();
        return -1;
    }
}

Command cmd_stash = {
    .name = "stash",
    .description = "Stash changes in the working directory",
    .run = stash_run,
    .help = stash_help
};
