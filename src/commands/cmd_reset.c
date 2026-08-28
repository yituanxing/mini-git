#include "../command.h"
#include "../core/ref.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../core/tree.h"
#include "../core/index.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit reset [--soft|--mixed|--hard] <commit>
 *
 * reset 是理解 Git "三棵树"最直接的命令：
 *
 *                 HEAD        Index       Working Tree
 *   --soft         reset       keep        keep
 *   --mixed        reset       reset       keep    (默认)
 *   --hard         reset       reset       reset
 *
 * reset 不会修改旧 commit 对象；它移动引用，并按模式决定是否同步
 * Index / 工作区。旧 commit 失去引用后仍可通过 reflog 找回。
 */

typedef enum {
    RESET_SOFT,
    RESET_MIXED,
    RESET_HARD
} ResetMode;

static void reset_help(void) {
    printf("usage: mgit reset [--soft | --mixed | --hard] <commit-hash>\n\n");
    printf("Reset current HEAD to a specific commit.\n\n");
    printf("Modes:\n");
    printf("    --soft     Move HEAD only; keep Index and working tree\n");
    printf("    --mixed    Move HEAD and reset Index; keep working tree (default)\n");
    printf("    --hard     Move HEAD and reset Index + working tree\n");
}

/* 读取目标 commit 的 tree，并按模式同步 Index / 工作区。 */
static int reset_state(ObjectStore *store, Index *idx, const Hash *commit_hash,
                       ResetMode mode) {
    Object obj;
    memset(&obj, 0, sizeof(obj));
    if (object_store_read(store, commit_hash, &obj) != 0) return -1;
    if (obj.type != OBJ_COMMIT) { object_free(&obj); return -1; }

    Commit c;
    memset(&c, 0, sizeof(c));
    commit_parse(obj.data, obj.size, &c);
    object_free(&obj);

    Object tree_obj;
    memset(&tree_obj, 0, sizeof(tree_obj));
    if (object_store_read(store, &c.tree, &tree_obj) != 0 ||
        tree_obj.type != OBJ_TREE) {
        object_free(&tree_obj);
        commit_free(&c);
        return -1;
    }

    Tree tree;
    memset(&tree, 0, sizeof(tree));
    tree_parse(tree_obj.data, tree_obj.size, &tree);
    object_free(&tree_obj);
    commit_free(&c);

    int ret;
    if (mode == RESET_HARD) {
        ret = tree_restore_worktree(store, idx, &tree);
    } else {
        ret = tree_rebuild_index(store, idx, &tree);
    }
    tree_free(&tree);

    if (ret != 0) return -1;
    return index_write(idx);
}

/* 解析 commit：完整哈希 / 短哈希（扫描对象库，含不可达对象）/ 引用名 */
static int resolve_target(ObjectStore *store, RefManager *refs, const char *str, Hash *out) {
    size_t len = strlen(str);

    /* 完整哈希 */
    if (len == 40 && hex_to_hash(str, out) == 0) {
        return 0;
    }

    /* 短哈希前缀：直接扫描对象库（reset 后丢失的 commit 也能找到） */
    if (len >= 4 && len < 40) {
        if (object_find_by_prefix(store, str, OBJ_COMMIT, out) == 0) {
            return 0;
        }
    }

    /* 引用名 */
    if (ref_resolve_quiet(refs, str, out) == 0) {
        return 0;
    }

    return -1;
}

static int reset_run(int argc, char **argv) {
    const char *target = NULL;
    ResetMode mode = RESET_MIXED;  /* 与真实 Git 一致：默认 mixed */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--soft") == 0) {
            mode = RESET_SOFT;
        } else if (strcmp(argv[i], "--mixed") == 0) {
            mode = RESET_MIXED;
        } else if (strcmp(argv[i], "--hard") == 0) {
            mode = RESET_HARD;
        } else if (argv[i][0] == '-') {
            mgit_error("unknown reset option: %s", argv[i]);
            reset_help();
            return -1;
        } else if (!target) {
            target = argv[i];
        }
    }

    if (!target) {
        mgit_error("no commit specified");
        reset_help();
        return -1;
    }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 解析目标 commit（支持短哈希、完整哈希、引用名） */
    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("cannot open object store");
        ref_manager_close(refs);
        return -1;
    }

    Hash target_hash;
    if (resolve_target(store, refs, target, &target_hash) != 0) {
        mgit_error("unknown commit: %s", target);
        object_store_close(store);
        ref_manager_close(refs);
        return -1;
    }

    /* 验证对象存在且是 commit */
    {
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &target_hash, &obj) == 0) {
            if (obj.type != OBJ_COMMIT) {
                mgit_error("not a commit: %s", target);
                object_free(&obj);
                object_store_close(store);
                ref_manager_close(refs);
                return -1;
            }
            object_free(&obj);
        }
    }

    /*
     * --soft 只移动引用；
     * --mixed/--hard 在移动引用前先同步需要同步的状态，失败则不移动 HEAD。
     */
    if (mode != RESET_SOFT) {
        Index *idx = index_open(".git");
        if (!idx) {
            mgit_error("cannot open index");
            object_store_close(store);
            ref_manager_close(refs);
            return -1;
        }
        if (reset_state(store, idx, &target_hash, mode) != 0) {
            mgit_error(mode == RESET_HARD
                       ? "failed to reset index and working tree"
                       : "failed to reset index");
            index_close(idx);
            object_store_close(store);
            ref_manager_close(refs);
            return -1;
        }
        index_close(idx);
    }
    object_store_close(store);

    /* 获取当前分支 */
    char branch_ref[256];
    if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) != 0) {
        mgit_error("cannot get current branch");
        ref_manager_close(refs);
        return -1;
    }

    /* 获取旧 commit */
    Hash old_hash;
    char old_hex[HASH_HEX_SIZE] = "(none)";
    char old_hex_log[HASH_HEX_SIZE];
    memset(old_hex_log, '0', 40);
    old_hex_log[40] = 0;
    if (ref_resolve_head_quiet(refs, &old_hash) == 0) {
        hash_to_hex(&old_hash, old_hex);
        memcpy(old_hex_log, old_hex, HASH_HEX_SIZE);
    }

    /* 更新分支指针 */
    if (ref_update(refs, branch_ref, &target_hash) != 0) {
        mgit_error("failed to update branch");
        ref_manager_close(refs);
        return -1;
    }

    char target_hex[HASH_HEX_SIZE];
    hash_to_hex(&target_hash, target_hex);

    const char *branch_name = branch_ref;
    if (strncmp(branch_ref, "refs/heads/", 11) == 0) {
        branch_name = branch_ref + 11;
    }

    printf("HEAD is now at %s (was %s)\n", target_hex, old_hex);
    printf("Branch '%s' moved: %s -> %s\n", branch_name, old_hex, target_hex);
    printf("\nNote: no commits were deleted. Objects remain in .git/objects/\n");

    /* 记录 reflog（reset 后靠它找回 commit） */
    {
        char action[512];
        snprintf(action, sizeof(action), "reset: moving to %s", target);
        reflog_append(old_hex_log, target_hex, action);
    }

    ref_manager_close(refs);
    return 0;
}

Command cmd_reset = {
    .name = "reset",
    .description = "Reset current branch to a specific commit",
    .run = reset_run,
    .help = reset_help
};
