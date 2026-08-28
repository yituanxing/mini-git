#include "../command.h"
#include "../core/ref.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit branch [<name> | -d <name>]
 * 
 * 分支管理
 * 
 * 用法：
 * - mgit branch           列出所有分支
 * - mgit branch <name>    创建新分支
 * - mgit branch -d <name> 删除分支
 */

static void branch_help(void) {
    printf("usage: mgit branch [<name> | -d <name>]\n\n");
    printf("List, create, or delete branches.\n\n");
    printf("Options:\n");
    printf("    (no args)    List all branches\n");
    printf("    <name>       Create a new branch\n");
    printf("    -d <name>    Delete a branch\n");
}

/* target 是否已经包含在 start 的提交历史中。 */
static int branch_is_merged(ObjectStore *store, const Hash *target, const Hash *start) {
    if (hash_equal(target, start)) return 1;

    Hash queue[1000];
    int head = 0, tail = 0;
    queue[tail++] = *start;

    while (head < tail && head < 1000) {
        Hash cur = queue[head++];
        Commit commit;
        memset(&commit, 0, sizeof(commit));
        if (commit_read(store, &cur, &commit) != 0) continue;

        for (int i = 0; i < commit.parent_count; i++) {
            if (hash_equal(target, &commit.parents[i])) {
                commit_free(&commit);
                return 1;
            }
            if (tail < 1000) queue[tail++] = commit.parents[i];
        }
        commit_free(&commit);
    }
    return 0;
}

static int branch_run(int argc, char **argv) {
    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 无参数：列出分支 */
    if (argc < 2) {
        char current[256];
        int has_current = (ref_get_head_branch(refs, current, sizeof(current)) == 0);

        /* 列出所有分支 */
        char branches[64][256];
        int count = ref_list_branches(refs, branches, 64);

        if (count == 0) {
            printf("(no branches)\n");
        } else {
            for (int i = 0; i < count; i++) {
                const char *name = branches[i];
                if (strncmp(name, "refs/heads/", 11) == 0) {
                    name = name + 11;
                }
                /* 标记当前分支 */
                if (has_current && strcmp(branches[i], current) == 0) {
                    printf("* %s\n", name);
                } else {
                    printf("  %s\n", name);
                }
            }
        }

        ref_manager_close(refs);
        return 0;
    }

    /* 删除分支 */
    if (strcmp(argv[1], "-d") == 0) {
        if (argc < 3) {
            mgit_error("branch name required");
            branch_help();
            ref_manager_close(refs);
            return -1;
        }
        const char *name = argv[2];

        /* 不能删除当前分支 */
        char current[256];
        if (ref_get_head_branch(refs, current, sizeof(current)) == 0) {
            const char *cur_name = current;
            if (strncmp(current, "refs/heads/", 11) == 0) {
                cur_name = current + 11;
            }
            if (strcmp(cur_name, name) == 0) {
                mgit_error("cannot delete current branch '%s'", name);
                ref_manager_close(refs);
                return -1;
            }
        }

        /*
         * -d 是安全删除：分支 tip 必须已经在当前 HEAD 的历史里。
         * mgit 不实现 upstream 配置，因此用 HEAD 作为合并判断基准。
         */
        Hash branch_hash, head_hash;
        if (ref_resolve_quiet(refs, name, &branch_hash) != 0 ||
            ref_resolve_head_quiet(refs, &head_hash) != 0) {
            mgit_error("cannot determine merge status of branch '%s'", name);
            ref_manager_close(refs);
            return -1;
        }

        ObjectStore *store = object_store_open(".git");
        if (!store) {
            ref_manager_close(refs);
            return -1;
        }
        int merged = branch_is_merged(store, &branch_hash, &head_hash);
        object_store_close(store);

        if (!merged) {
            mgit_error("branch '%s' is not fully merged into HEAD", name);
            mgit_error("refusing safe delete");
            ref_manager_close(refs);
            return -1;
        }

        if (ref_delete_branch(refs, name) != 0) {
            mgit_error("failed to delete branch '%s'", name);
            ref_manager_close(refs);
            return -1;
        }
        printf("Deleted branch %s\n", name);
        ref_manager_close(refs);
        return 0;
    }

    /* 创建分支 */
    const char *name = argv[1];

    /* 拒绝覆盖已存在的分支（与真实 git 一致） */
    {
        Hash existing;
        if (ref_resolve_quiet(refs, name, &existing) == 0) {
            mgit_error("a branch named '%s' already exists", name);
            ref_manager_close(refs);
            return -1;
        }
    }

    /* 获取当前 HEAD 的 commit */
    Hash head_hash;
    if (ref_resolve_head(refs, &head_hash) != 0) {
        mgit_error("cannot create branch: HEAD is not set");
        mgit_error("make a commit first, then try again");
        ref_manager_close(refs);
        return -1;
    }

    if (ref_create_branch(refs, name, &head_hash) != 0) {
        mgit_error("failed to create branch '%s'", name);
        ref_manager_close(refs);
        return -1;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&head_hash, hex);
    printf("Created branch %s at %s\n", name, hex);

    ref_manager_close(refs);
    return 0;
}

Command cmd_branch = {
    .name = "branch",
    .description = "List, create, or delete branches",
    .run = branch_run,
    .help = branch_help
};
