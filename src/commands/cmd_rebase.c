#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/graph.h"
#include "../core/ref.h"
#include "../core/index.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * mgit rebase <branch>
 * mgit rebase --continue
 * mgit rebase --abort
 *
 * 变基：把当前分支的提交"搬"到目标分支的最新位置上
 *
 * 原理（等价于）：
 * 1. 找到当前分支领先目标分支的提交列表
 * 2. 把 HEAD 分支指针移到目标分支（工作区同步）
 * 3. 逐个 cherry-pick 这些提交
 *
 * 状态文件（rebase 过程中）：
 * .git/rebase-orig     rebase 前的 HEAD commit（用于 --abort）
 * .git/rebase-todo     待重放的 commit 列表（每行一个哈希）
 * .git/rebase-current  当前因冲突暂停的 commit
 */

static void rebase_help(void) {
    printf("usage: mgit rebase <branch>\n");
    printf("       mgit rebase --continue\n");
    printf("       mgit rebase --abort\n\n");
    printf("Reapply commits on top of another base tip.\n");
}

#define REBASE_ORIG    ".git/rebase-orig"
#define REBASE_TODO    ".git/rebase-todo"
#define REBASE_CURRENT ".git/rebase-current"

/* ---- 状态文件读写 ---- */

static int hex_file_write(const char *path, const char *hex) {
    return file_write_line(path, hex);
}

static int hex_file_read(const char *path, char *hex, size_t size) {
    uint8_t *data;
    size_t len;
    if (file_read_all(path, &data, &len) != 0) return -1;
    size_t n = 0;
    while (n < len && data[n] != '\n' && data[n] != '\r') n++;
    if (n >= size) n = size - 1;
    memcpy(hex, data, n);
    hex[n] = 0;
    free(data);
    return 0;
}

static int todo_write(Hash *list, int count) {
    FILE *fp = fopen(REBASE_TODO, "wb");
    if (!fp) return -1;
    for (int i = 0; i < count; i++) {
        char hex[HASH_HEX_SIZE];
        hash_to_hex(&list[i], hex);
        fprintf(fp, "%s\n", hex);
    }
    fclose(fp);
    return 0;
}

static int todo_read(Hash *list, int max) {
    FILE *fp = fopen(REBASE_TODO, "rb");
    if (!fp) return 0;
    int count = 0;
    char line[64];
    while (count < max && fgets(line, sizeof(line), fp)) {
        /* 去掉换行 */
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) == 40 && hex_to_hash(line, &list[count]) == 0) {
            count++;
        }
    }
    fclose(fp);
    return count;
}

static void state_cleanup(void) {
    file_delete(REBASE_ORIG);
    file_delete(REBASE_TODO);
    file_delete(REBASE_CURRENT);
}

static int rebase_in_progress(void) {
    return file_exists(REBASE_TODO) || file_exists(REBASE_CURRENT);
}

/*
 * 收集 HEAD 有而 upstream 没有的提交（BFS）
 * 返回数量；结果按"新→旧"顺序，调用者需反转
 */
static int collect_local_commits(ObjectStore *store, const Hash *head,
                                 const Hash *upstream, Hash *out, int max) {
    int count = 0;
    Hash queue[1000];
    Hash visited[1000];
    int vcount = 0;
    int head_q = 0, tail = 0;
    queue[tail++] = *head;

    while (head_q < tail && head_q < 999) {
        Hash cur = queue[head_q++];

        /* 去重 */
        int seen = 0;
        for (int i = 0; i < vcount; i++) {
            if (hash_equal(&cur, &visited[i])) { seen = 1; break; }
        }
        if (seen) continue;
        if (vcount < 1000) visited[vcount++] = cur;

        /* 在 upstream 历史中 → 不收集，且不再向下遍历 */
        if (graph_is_ancestor(store, &cur, upstream)) continue;

        if (count < max) out[count++] = cur;

        Commit c;
        memset(&c, 0, sizeof(c));
        if (commit_read(store, &cur, &c) != 0) continue;
        for (int i = 0; i < c.parent_count; i++) {
            if (tail < 1000) queue[tail++] = c.parents[i];
        }
        commit_free(&c);
    }
    return count;
}

/* 把工作区和 Index 恢复为指定 commit 的 tree */
static int restore_to_commit(ObjectStore *store, Index *idx, const Hash *commit_hash) {
    Object obj;
    memset(&obj, 0, sizeof(obj));
    if (object_store_read(store, commit_hash, &obj) != 0 || obj.type != OBJ_COMMIT) {
        object_free(&obj);
        return -1;
    }
    Commit c;
    memset(&c, 0, sizeof(c));
    commit_parse(obj.data, obj.size, &c);
    object_free(&obj);

    Object tree_obj;
    memset(&tree_obj, 0, sizeof(tree_obj));
    if (object_store_read(store, &c.tree, &tree_obj) != 0 || tree_obj.type != OBJ_TREE) {
        object_free(&tree_obj);
        commit_free(&c);
        return -1;
    }
    Tree tree;
    memset(&tree, 0, sizeof(tree));
    tree_parse(tree_obj.data, tree_obj.size, &tree);
    object_free(&tree_obj);
    commit_free(&c);

    tree_restore_worktree(store, idx, &tree);
    tree_free(&tree);
    return index_write(idx);
}

/*
 * 处理 todo 列表：逐个 cherry-pick
 * 冲突时保存状态并返回 1
 */
static int process_todo(ObjectStore *store, RefManager *refs, Index *idx) {
    Hash todo[1000];
    int count = todo_read(todo, 1000);

    int pos = 0;
    while (pos < count) {
        Hash pick = todo[pos];
        int r = cherry_pick_commit(store, refs, idx, &pick, NULL);

        if (r == 1) {
            /* 冲突：保存剩余 todo（含当前）与当前 commit */
            todo_write(&todo[pos], count - pos);
            char hex[HASH_HEX_SIZE];
            hash_to_hex(&pick, hex);
            hex_file_write(REBASE_CURRENT, hex);
            if (idx->dirty) index_write(idx);
            printf("\nCONFLICT while rebasing.\n");
            printf("hint: fix conflicts, run 'mgit add', then 'mgit rebase --continue'\n");
            printf("hint: or run 'mgit rebase --abort' to cancel\n");
            return 1;
        } else if (r == 2) {
            /* 空提交 → 跳过 */
            char hex[HASH_HEX_SIZE];
            hash_to_hex(&pick, hex);
            printf("skipping empty commit %.7s\n", hex);
        } else if (r != 0) {
            return -1;
        }

        if (idx->dirty) index_write(idx);
        pos++;
        /* 更新剩余 todo */
        todo_write(&todo[pos], count - pos);
    }

    return 0;
}

/* --continue：先提交用户解决冲突后的 Index，再继续 todo */
static int rebase_continue(ObjectStore *store, RefManager *refs, Index *idx) {
    char cur_hex[HASH_HEX_SIZE];
    if (hex_file_read(REBASE_CURRENT, cur_hex, sizeof(cur_hex)) == 0 &&
        strlen(cur_hex) == 40) {
        Hash cur;
        hex_to_hash(cur_hex, &cur);

        /* 用原 commit 的信息提交当前 Index */
        Commit orig;
        memset(&orig, 0, sizeof(orig));
        const char *msg = "rebased commit";
        if (commit_read(store, &cur, &orig) == 0 && orig.message) {
            msg = orig.message;
        }

        Hash tree_hash;
        if (index_write_tree(idx, store, &tree_hash) != 0) {
            commit_free(&orig);
            mgit_error("failed to create tree");
            return -1;
        }
        Hash head_hash;
        char old_hex[HASH_HEX_SIZE];
        int has_head = (ref_resolve_head_quiet(refs, &head_hash) == 0);
        if (has_head) {
            hash_to_hex(&head_hash, old_hex);
        } else {
            memset(old_hex, '0', 40);
            old_hex[40] = 0;
        }

        Hash new_commit;
        if (commit_create(store, &tree_hash, has_head ? &head_hash : NULL,
                          msg, &new_commit) != 0) {
            commit_free(&orig);
            mgit_error("failed to create commit");
            return -1;
        }

        char branch_ref[256];
        branch_ref[0] = 0;
        if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) == 0) {
            ref_update(refs, branch_ref, &new_commit);
        }

        char new_hex[HASH_HEX_SIZE];
        hash_to_hex(&new_commit, new_hex);
        const char *bn = branch_ref;
        if (strncmp(branch_ref, "refs/heads/", 11) == 0) bn += 11;

        char short_msg[128];
        strncpy(short_msg, msg, sizeof(short_msg) - 1);
        short_msg[sizeof(short_msg) - 1] = 0;
        char *nl = strchr(short_msg, '\n');
        if (nl) *nl = 0;
        printf("[%s %s] %s\n", bn, new_hex, short_msg);

        char action[512];
        snprintf(action, sizeof(action), "rebase: %s", short_msg);
        reflog_append(old_hex, new_hex, action);

        commit_free(&orig);
        file_delete(REBASE_CURRENT);
        if (idx->dirty) index_write(idx);

        /* 当前冲突提交已处理，从 todo 中移除，避免重复 pick */
        Hash todo[1000];
        int tcount = todo_read(todo, 1000);
        if (tcount > 0 && hash_equal(&todo[0], &cur)) {
            todo_write(&todo[1], tcount - 1);
        }
    }

    return process_todo(store, refs, idx);
}

static int rebase_abort(RefManager *refs, ObjectStore *store, Index *idx) {
    char orig_hex[HASH_HEX_SIZE];
    if (hex_file_read(REBASE_ORIG, orig_hex, sizeof(orig_hex)) != 0) {
        mgit_error("no rebase in progress");
        return -1;
    }
    Hash orig;
    if (hex_to_hash(orig_hex, &orig) != 0) {
        mgit_error("corrupted rebase state");
        state_cleanup();
        return -1;
    }

    /* 恢复分支指针与工作区 */
    char branch_ref[256];
    if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) == 0) {
        ref_update(refs, branch_ref, &orig);
    }
    restore_to_commit(store, idx, &orig);

    char head_hex[HASH_HEX_SIZE];
    Hash head_now;
    if (ref_resolve_head_quiet(refs, &head_now) == 0) {
        hash_to_hex(&head_now, head_hex);
    } else {
        memset(head_hex, '0', 40);
        head_hex[40] = 0;
    }
    reflog_append(head_hex, orig_hex, "rebase: abort");

    state_cleanup();
    printf("Rebase aborted. Back to %.7s.\n", orig_hex);
    return 0;
}

static int rebase_run(int argc, char **argv) {
    ObjectStore *store = object_store_open(".git");
    if (!store) { mgit_error("not a git repository"); return -1; }
    RefManager *refs = ref_manager_open(".git");
    Index *idx = index_open(".git");
    if (!refs || !idx) {
        if (idx) index_close(idx);
        if (refs) ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* --continue */
    if (argc >= 2 && strcmp(argv[1], "--continue") == 0) {
        if (!rebase_in_progress()) {
            mgit_error("no rebase in progress");
            index_close(idx); ref_manager_close(refs); object_store_close(store);
            return -1;
        }
        int r = rebase_continue(store, refs, idx);
        if (r == 0) {
            char branch_ref[256];
            if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) == 0) {
                printf("Successfully rebased and updated %s.\n", branch_ref);
            }
            state_cleanup();
        }
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return (r == 0) ? 0 : -1;
    }

    /* --abort */
    if (argc >= 2 && strcmp(argv[1], "--abort") == 0) {
        int r = rebase_abort(refs, store, idx);
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return r;
    }

    /* mgit rebase <branch> */
    if (argc < 2) {
        mgit_error("no branch specified");
        rebase_help();
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return -1;
    }
    if (rebase_in_progress()) {
        mgit_error("rebase already in progress (use --continue or --abort)");
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return -1;
    }

    /* 解析目标分支 */
    Hash upstream;
    if (ref_resolve_quiet(refs, argv[1], &upstream) != 0) {
        mgit_error("unknown branch: %s", argv[1]);
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return -1;
    }

    Hash head_hash;
    if (ref_resolve_head(refs, &head_hash) != 0) {
        mgit_error("no commits yet");
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return -1;
    }

    /* 已经是最新（HEAD 等于 upstream，或 upstream 已是 HEAD 的祖先）→ 无需变基 */
    if (hash_equal(&head_hash, &upstream) ||
        graph_is_ancestor(store, &upstream, &head_hash)) {
        printf("Current branch is up to date.\n");
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return 0;
    }

    /* 收集待重放的提交（新→旧），然后反转为旧→新 */
    Hash picks[1000];
    int pcount = collect_local_commits(store, &head_hash, &upstream, picks, 1000);

    if (pcount == 0) {
        /* HEAD 是 upstream 的祖先 → 直接快进 */
        printf("Fast-forwarding to %s.\n", argv[1]);
    }

    /* 记录原 HEAD，移动分支指针到 upstream，同步工作区 */
    char old_hex[HASH_HEX_SIZE];
    hash_to_hex(&head_hash, old_hex);
    hex_file_write(REBASE_ORIG, old_hex);

    char branch_ref[256];
    if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) != 0) {
        mgit_error("cannot get current branch");
        state_cleanup();
        index_close(idx); ref_manager_close(refs); object_store_close(store);
        return -1;
    }
    ref_update(refs, branch_ref, &upstream);
    char up_hex[HASH_HEX_SIZE];
    hash_to_hex(&upstream, up_hex);
    char action[512];
    snprintf(action, sizeof(action), "rebase: checkout %s", argv[1]);
    reflog_append(old_hex, up_hex, action);

    restore_to_commit(store, idx, &upstream);

    /* 反转为旧→新 */
    for (int i = 0; i < pcount / 2; i++) {
        Hash tmp = picks[i];
        picks[i] = picks[pcount - 1 - i];
        picks[pcount - 1 - i] = tmp;
    }
    todo_write(picks, pcount);

    int r = process_todo(store, refs, idx);
    if (r == 0) {
        printf("Successfully rebased and updated %s.\n", branch_ref);
        state_cleanup();
    }

    index_close(idx);
    ref_manager_close(refs);
    object_store_close(store);
    return (r == 0) ? 0 : -1;
}

Command cmd_rebase = {
    .name = "rebase",
    .description = "Reapply commits on top of another base",
    .run = rebase_run,
    .help = rebase_help
};
