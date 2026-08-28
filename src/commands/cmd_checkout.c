#include "../command.h"
#include "../core/ref.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/index.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * mgit checkout <branch|commit>
 * mgit checkout -b <new-branch>
 *
 * 切换分支、检出某个 commit（detached HEAD），或创建并切换到新分支。
 *
 * HEAD 的两个关键状态：
 * - 分支状态：HEAD = "ref: refs/heads/master"
 * - detached：HEAD 直接保存 commit hash
 * 
 * 切换分支时会：
 * 1. 检查未提交的修改是否会被覆盖（安全检查）
 * 2. 删除目标分支不包含的已跟踪文件
 * 3. 恢复目标分支的文件内容
 * 4. 同步 Index 到目标分支的 tree
 */

static void checkout_help(void) {
    printf("usage: mgit checkout <branch-or-commit>\n");
    printf("       mgit checkout -b <new-branch>\n\n");
    printf("Switch branches, detach at a commit, or create a new branch.\n\n");
    printf("Options:\n");
    printf("    -b <name>    Create and switch to new branch\n");
}

/*
 * 查找 tree 中指定路径的 blob hash（迭代实现，避免深递归爆栈）
 */
static int tree_find_hash(ObjectStore *store, const Tree *root, const char *target, Hash *out) {
    /* 显式栈：每项是一个已解析的 tree */
    Tree stack[64];
    int depth = 0;

    /* 复制根 tree 到栈（浅拷贝，条目共享） */
    stack[0] = *root;
    depth = 1;

    /* 逐路径分量向下查找 */
    const char *p = target;
    while (depth > 0) {
        /* 取当前路径分量 */
        char component[512];
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen >= sizeof(component)) {
            for (int d = 1; d < depth; d++) tree_free(&stack[d]);
            return -1;
        }
        memcpy(component, p, clen);
        component[clen] = 0;

        Tree *cur = &stack[depth - 1];
        int found = 0;

        for (size_t i = 0; i < cur->count; i++) {
            const TreeEntry *e = &cur->entries[i];
            if (strcmp(e->name, component) != 0) continue;

            if (!slash) {
                /* 最后一个分量：必须是 blob */
                if (e->type == TREE_ENTRY_BLOB) {
                    *out = e->hash;
                    /* 释放栈中解析出的子 tree */
                    for (int d = 1; d < depth; d++) {
                        tree_free(&stack[d]);
                    }
                    return 0;
                }
                for (int d = 1; d < depth; d++) tree_free(&stack[d]);
                return -1;
            } else {
                /* 还有后续分量：必须是子 tree */
                if (e->type != TREE_ENTRY_TREE) {
                    for (int d = 1; d < depth; d++) tree_free(&stack[d]);
                    return -1;
                }
                if (depth >= 64) {
                    for (int d = 1; d < depth; d++) tree_free(&stack[d]);
                    return -1;
                }

                Object obj;
                if (object_store_read(store, &e->hash, &obj) != 0) {
                    for (int d = 1; d < depth; d++) tree_free(&stack[d]);
                    return -1;
                }
                if (obj.type != OBJ_TREE) {
                    object_free(&obj);
                    for (int d = 1; d < depth; d++) tree_free(&stack[d]);
                    return -1;
                }

                memset(&stack[depth], 0, sizeof(Tree));
                tree_parse(obj.data, obj.size, &stack[depth]);
                object_free(&obj);
                depth++;
                found = 1;
                break;
            }
        }

        if (!found) {
            for (int d = 1; d < depth; d++) {
                tree_free(&stack[d]);
            }
            return -1;
        }

        p = slash + 1;
    }

    return -1;
}

/*
 * 切换分支的工作区同步
 * 
 * 1. 安全检查：当前 Index 跟踪的文件若有未提交修改，
 *    且目标分支该文件内容不同 → 拒绝切换
 * 2. 用目标分支的 tree 同步工作区和 Index
 */
static int sync_worktree(ObjectStore *store, Index *idx, const Hash *target_commit) {
    /* 读取目标分支的 tree */
    Tree target_tree = {0};
    if (commit_read_tree(store, target_commit, &target_tree) != 0) {
        mgit_error("cannot read target branch tree");
        return -1;
    }

    /* 1. 安全检查：本地修改会被覆盖？ */
    for (size_t i = 0; i < idx->count; i++) {
        IndexEntry *ie = &idx->entries[i];

        Hash target_hash;
        int in_target = (tree_find_hash(store, &target_tree, ie->name, &target_hash) == 0);

        if (in_target && hash_equal(&ie->hash, &target_hash)) {
            continue;  /* 目标分支内容相同，切换无影响 */
        }

        /* 内容不同（或目标分支没有此文件）：检查工作区是否干净 */
        uint8_t *data;
        size_t size;
        if (file_read_all(ie->name, &data, &size) != 0) {
            continue;  /* 文件已不存在（用户手动删除），允许切换 */
        }

        Hash worktree_hash;
        object_hash(OBJ_BLOB, data, size, &worktree_hash);
        free(data);

        if (!hash_equal(&worktree_hash, &ie->hash)) {
            mgit_error("Your local changes to '%s' would be overwritten by checkout.\n"
                       "Please commit your changes or stash them before you switch branches.",
                       ie->name);
            tree_free(&target_tree);
            return -1;
        }
    }

    /* 2. 同步工作区和 Index（删除多余文件 + 恢复目标分支文件） */
    tree_restore_worktree(store, idx, &target_tree);
    tree_free(&target_tree);
    index_write(idx);
    return 0;
}

/* 解析用于 detached HEAD 的 commit-ish：引用名或 4..40 位 commit 前缀。 */
static int resolve_commitish(ObjectStore *store, RefManager *refs,
                             const char *name, Hash *out) {
    if (ref_resolve_quiet(refs, name, out) == 0) {
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, out, &obj) == 0) {
            int ok = (obj.type == OBJ_COMMIT);
            object_free(&obj);
            if (ok) return 0;
        }
    }

    size_t len = strlen(name);
    if (len >= 4 && len <= 40 &&
        object_find_by_prefix(store, name, OBJ_COMMIT, out) == 0) {
        return 0;
    }
    return -1;
}

static int checkout_run(int argc, char **argv) {
    if (argc < 2) {
        mgit_error("no branch specified");
        checkout_help();
        return -1;
    }

    int create_new = 0;
    const char *branch_name = NULL;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            create_new = 1;
            branch_name = argv[++i];
            break;
        } else if (argv[i][0] != '-') {
            branch_name = argv[i];
            break;
        }
    }

    if (!branch_name) {
        mgit_error("no branch specified");
        checkout_help();
        return -1;
    }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        mgit_error("not a git repository");
        return -1;
    }

    char ref_path[256];
    snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", branch_name);

    if (create_new) {
        /* -b: 创建新分支（指向当前 HEAD 的 commit） */
        Hash head_hash;
        if (ref_resolve_head_quiet(refs, &head_hash) != 0) {
            mgit_error("cannot create branch: no commits yet");
            ref_manager_close(refs);
            return -1;
        }

        /* 旧分支名（供 reflog 描述） */
        char from_branch[256];
        if (ref_get_head_branch(refs, from_branch, sizeof(from_branch)) == 0 &&
            strncmp(from_branch, "refs/heads/", 11) == 0) {
            memmove(from_branch, from_branch + 11, strlen(from_branch + 11) + 1);
        } else {
            snprintf(from_branch, sizeof(from_branch), "HEAD");
        }

        /* 检查分支是否已存在（静默探测） */
        Hash existing;
        if (ref_resolve_quiet(refs, ref_path, &existing) == 0) {
            mgit_error("branch '%s' already exists", branch_name);
            ref_manager_close(refs);
            return -1;
        }

        /* 创建分支 */
        if (ref_create_branch(refs, branch_name, &head_hash) != 0) {
            mgit_error("failed to create branch '%s'", branch_name);
            ref_manager_close(refs);
            return -1;
        }

        /* 切换 HEAD（工作区不变，因为新分支指向当前 commit） */
        if (ref_set_head(refs, branch_name) != 0) {
            mgit_error("failed to switch to branch '%s'", branch_name);
            ref_manager_close(refs);
            return -1;
        }
        printf("Switched to a new branch '%s'\n", branch_name);

        /* 记录 reflog（HEAD commit 不变，但当前分支变了） */
        {
            char hex[HASH_HEX_SIZE];
            hash_to_hex(&head_hash, hex);
            char action[512];
            snprintf(action, sizeof(action), "checkout: moving from %s to %s",
                     from_branch, branch_name);
            reflog_append(hex, hex, action);
        }

        ref_manager_close(refs);
        return 0;
    }

    /*
     * 优先按本地分支解析；如果不是分支，再按 commit-ish 解析。
     * 后一种情况进入 detached HEAD。
     */
    Hash target_hash;
    int target_is_branch =
        (ref_resolve_quiet(refs, ref_path, &target_hash) == 0);

    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("cannot open object store");
        ref_manager_close(refs);
        return -1;
    }

    if (!target_is_branch &&
        resolve_commitish(store, refs, branch_name, &target_hash) != 0) {
        mgit_error("branch or commit '%s' not found", branch_name);
        object_store_close(store);
        ref_manager_close(refs);
        return -1;
    }

    /* 记录切换前的位置；分支切换到自己时直接返回。 */
    char current[256];
    char from_branch[256];
    snprintf(from_branch, sizeof(from_branch), "HEAD");
    if (ref_get_head_branch(refs, current, sizeof(current)) == 0) {
        if (target_is_branch && strcmp(current, ref_path) == 0) {
            printf("Already on '%s'\n", branch_name);
            object_store_close(store);
            ref_manager_close(refs);
            return 0;
        }
        if (strncmp(current, "refs/heads/", 11) == 0) {
            snprintf(from_branch, sizeof(from_branch), "%s", current + 11);
        }
    }

    Hash old_head_hash;
    char old_hex[HASH_HEX_SIZE];
    memset(old_hex, '0', 40);
    old_hex[40] = 0;
    if (ref_resolve_head_quiet(refs, &old_head_hash) == 0) {
        hash_to_hex(&old_head_hash, old_hex);
    }

    /* 两种 checkout 都用同一套安全检查并同步工作区 + Index。 */
    Index *idx = index_open(".git");
    if (!idx) {
        mgit_error("cannot open index");
        object_store_close(store);
        ref_manager_close(refs);
        return -1;
    }

    if (sync_worktree(store, idx, &target_hash) != 0) {
        index_close(idx);
        object_store_close(store);
        ref_manager_close(refs);
        return -1;
    }

    index_close(idx);
    object_store_close(store);

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&target_hash, hex);

    if (target_is_branch) {
        if (ref_set_head(refs, branch_name) != 0) {
            mgit_error("failed to switch to branch '%s'", branch_name);
            ref_manager_close(refs);
            return -1;
        }
        printf("Switched to branch '%s' (%s)\n", branch_name, hex);
    } else {
        if (ref_set_head_detached(refs, &target_hash) != 0) {
            mgit_error("failed to detach HEAD at %s", branch_name);
            ref_manager_close(refs);
            return -1;
        }
        printf("HEAD is now at %.7s (detached)\n", hex);
    }

    {
        char action[512];
        snprintf(action, sizeof(action), "checkout: moving from %s to %s",
                 from_branch, target_is_branch ? branch_name : hex);
        reflog_append(old_hex, hex, action);
    }

    ref_manager_close(refs);
    return 0;
}

Command cmd_checkout = {
    .name = "checkout",
    .description = "Switch branches or create a new branch",
    .run = checkout_run,
    .help = checkout_help
};
