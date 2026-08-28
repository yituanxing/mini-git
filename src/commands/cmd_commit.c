#include "../command.h"
#include "../core/object.h"
#include "../core/index.h"
#include "../core/ref.h"
#include "../core/commit.h"
#include "../core/tree.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * mgit commit -m <message> [-a] [--amend]
 * 
 * 创建一个提交
 * 
 * 支持:
 * - -m <msg>       提交信息
 * - -a             自动暂存已跟踪文件的修改
 * - --amend        修改上一次提交（替换）
 */

static void commit_help(void) {
    printf("usage: mgit commit -m <message> [-a] [--amend]\n\n");
    printf("Record changes to the repository.\n\n");
    printf("Options:\n");
    printf("    -m <message>    Commit message\n");
    printf("    -a              Auto-stage modified tracked files\n");
    printf("    --amend         Replace the last commit\n");
}

/* -a: 自动暂存所有已跟踪文件的修改和删除；不添加未跟踪文件。 */
static void auto_stage_all(Index *idx, ObjectStore *store) {
    size_t i = 0;
    while (i < idx->count) {
        IndexEntry *entry = &idx->entries[i];

        /* 已跟踪文件从工作区删除：等价于自动 git rm。 */
        if (!file_exists(entry->name)) {
            char path[1024];
            snprintf(path, sizeof(path), "%s", entry->name);
            index_remove(idx, path);
            /* index_remove 会把后续条目前移，所以这里不递增 i。 */
            continue;
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
            /* 文件已修改，写入新 blob 并更新 Index。 */
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

static int commit_run(int argc, char **argv) {
    const char *message = NULL;
    int auto_stage = 0;
    int amend = 0;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0) {
            auto_stage = 1;
        } else if (strcmp(argv[i], "--amend") == 0) {
            amend = 1;
        }
    }

    if (!message) {
        mgit_error("no commit message specified");
        commit_help();
        return -1;
    }

    /* 打开各个组件 */
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

    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        index_close(idx);
        object_store_close(store);
        mgit_error("cannot open ref manager");
        return -1;
    }

    /* -a: 自动暂存已跟踪文件的修改 */
    if (auto_stage) {
        auto_stage_all(idx, store);
    }

    if (idx->count == 0) {
        mgit_error("nothing to commit (empty index)");
        ref_manager_close(refs);
        index_close(idx);
        object_store_close(store);
        return -1;
    }

    /* 1. 从 Index 创建 tree */
    Hash tree_hash;
    if (index_write_tree(idx, store, &tree_hash) != 0) {
        mgit_error("failed to create tree from index");
        goto error;
    }

    /* 2. 获取父 commit */
    Hash parent_hash;
    Hash *parent_ptr = NULL;
    int in_merge = 0;
    Hash merge_parent;

    if (amend) {
        /* --amend: 使用上一次 commit 的 parent */
        Hash head_hash;
        if (ref_resolve_head(refs, &head_hash) != 0) {
            mgit_error("nothing to amend (no commits yet)");
            goto error;
        }
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &head_hash, &obj) != 0) {
            mgit_error("cannot read HEAD commit");
            goto error;
        }
        if (obj.type != OBJ_COMMIT) {
            mgit_error("cannot read HEAD commit");
            object_free(&obj);
            goto error;
        }
        Commit old;
        memset(&old, 0, sizeof(old));
        commit_parse(obj.data, obj.size, &old);
        object_free(&obj);

        if (old.parent_count > 0) {
            parent_hash = old.parents[0];
            parent_ptr = &parent_hash;
        }
        commit_free(&old);
        printf("amending previous commit\n");
    } else {
        if (ref_resolve_head_quiet(refs, &parent_hash) == 0) {
            parent_ptr = &parent_hash;
        }
        /* 存在 MERGE_HEAD => 正在解决合并冲突，本次提交应为双亲 merge commit */
        char merge_hex[HASH_HEX_SIZE + 16];
        if (parent_ptr != NULL &&
            file_read_line(".git/MERGE_HEAD", merge_hex, sizeof(merge_hex)) == 0 &&
            hex_to_hash(merge_hex, &merge_parent) == 0) {
            in_merge = 1;
        }
    }

    /* 3. 创建 commit 对象 */
    Hash commit_hash;
    if (in_merge) {
        if (commit_create_merge(store, &tree_hash, &parent_hash, &merge_parent,
                                message, &commit_hash) != 0) {
            mgit_error("failed to create merge commit");
            goto error;
        }
    } else if (commit_create(store, &tree_hash, parent_ptr, message, &commit_hash) != 0) {
        mgit_error("failed to create commit");
        goto error;
    }

    char commit_hex[HASH_HEX_SIZE];
    hash_to_hex(&commit_hash, commit_hex);

    /* 记录旧 HEAD（供 reflog）：首次提交时为 40 个 0 */
    Hash old_head_hash;
    char old_head_hex[HASH_HEX_SIZE];
    if (ref_resolve_head_quiet(refs, &old_head_hash) == 0) {
        hash_to_hex(&old_head_hash, old_head_hex);
    } else {
        memset(old_head_hex, '0', 40);
        old_head_hex[40] = 0;
    }

    /* 4. 更新当前分支引用 */
    char branch_ref[256];
    if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) != 0) {
        mgit_error("cannot get current branch");
        goto error;
    }

    if (ref_update(refs, branch_ref, &commit_hash) != 0) {
        mgit_error("failed to update branch: %s", branch_ref);
        goto error;
    }

    /*
     * MERGE_HEAD belongs to the merge transaction.  Remove it only after
     * the branch ref was updated successfully; otherwise a failed commit
     * must remain resumable.
     */
    if (in_merge && file_delete(".git/MERGE_HEAD") != 0) {
        mgit_warning("merge commit created but failed to remove MERGE_HEAD");
    }

    const char *branch_name = branch_ref;
    if (strncmp(branch_ref, "refs/heads/", 11) == 0) {
        branch_name = branch_ref + 11;
    }
    printf("[%s %s] %s\n", branch_name, commit_hex, message);

    /* 记录 reflog */
    {
        char action[512];
        snprintf(action, sizeof(action), "%s: %s",
                 amend ? "commit (amend)" : (in_merge ? "commit (merge)" : "commit"),
                 message);
        reflog_append(old_head_hex, commit_hex, action);
    }

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

Command cmd_commit = {
    .name = "commit",
    .description = "Record changes to the repository",
    .run = commit_run,
    .help = commit_help
};
