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

/*
 * mgit revert <commit>
 * 
 * 创建一个新 commit，撤销指定 commit 的改动
 * 
 * 与 reset 的区别：
 * - reset: 改写历史（移动 HEAD 指针回去）
 * - revert: 安全回滚（新增一个反向 commit，不改写历史）
 * 
 * 原理：
 * - 读取要 revert 的 commit 及其 parent 的 tree
 * - 比较两者的差异，反向应用到当前 Index
 */

static void revert_help(void) {
    printf("usage: mgit revert <commit>\n\n");
    printf("Create a new commit that undoes the changes of a previous commit.\n\n");
    printf("Unlike 'reset', revert does NOT rewrite history.\n");
    printf("It creates a new commit that reverses the specified commit's changes.\n");
}

/* 确保文件的父目录存在（子目录支持） */
static void ensure_parent_dir(const char *path) {
    char dir[1024];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        if (dlen < sizeof(dir)) {
            memcpy(dir, path, dlen);
            dir[dlen] = 0;
            file_mkdir_p(dir);
        }
    }
}

/* 读取 blob 内容到 malloc 缓冲区 */
static int read_blob(ObjectStore *store, const Hash *hash, uint8_t **data, size_t *size) {
    Object obj;
    if (object_store_read(store, hash, &obj) != 0) return -1;
    if (obj.type != OBJ_BLOB) { object_free(&obj); return -1; }
    *data = (uint8_t *)malloc(obj.size);
    if (!*data) { object_free(&obj); return -1; }
    memcpy(*data, obj.data, obj.size);
    *size = obj.size;
    object_free(&obj);
    return 0;
}

/* 将 blob 内容写到工作区文件 */
static void restore_worktree_file(ObjectStore *store, const char *name, const Hash *blob) {
    uint8_t *data;
    size_t size;
    if (read_blob(store, blob, &data, &size) == 0) {
        ensure_parent_dir(name);
        file_write_all(name, data, size);
        free(data);
    }
}

/* 解析短哈希或引用名为完整 commit hash */
static int resolve_commit(ObjectStore *store, RefManager *refs, const char *str, Hash *out) {
    size_t len = strlen(str);

    /* 尝试作为完整 40 字符哈希 */
    if (len == 40 && hex_to_hash(str, out) == 0) {
        return 0;
    }

    /* 尝试作为短哈希前缀：扫描对象库（含不可达对象） */
    if (len >= 4 && len < 40) {
        if (object_find_by_prefix(store, str, OBJ_COMMIT, out) == 0) {
            return 0;
        }
    }

    /* 尝试作为引用名（静默探测） */
    if (ref_resolve_quiet(refs, str, out) == 0) {
        return 0;
    }

    return -1;
}

static int revert_run(int argc, char **argv) {
    if (argc < 2) {
        mgit_error("no commit specified");
        revert_help();
        return -1;
    }

    const char *target_str = argv[1];

    ObjectStore *store = object_store_open(".git");
    if (!store) { mgit_error("not a git repository"); return -1; }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) { object_store_close(store); return -1; }

    /* 解析目标 commit（支持短哈希、完整哈希、引用名） */
    Hash target_hash;
    if (resolve_commit(store, refs, target_str, &target_hash) != 0) {
        mgit_error("unknown commit: %s", target_str);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 读取目标 commit */
    Commit target;
    memset(&target, 0, sizeof(target));
    if (commit_read(store, &target_hash, &target) != 0) {
        mgit_error("cannot read commit: %s", target_str);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /*
     * merge commit 有多个 parent，revert 必须先选择 mainline。
     * 真实 Git 用 -m/--mainline；mgit 不实现该选项，因此不能猜 parents[0]。
     */
    if (target.parent_count > 1) {
        mgit_error("cannot revert a merge commit without a mainline parent");
        mgit_error("mgit does not implement revert -m");
        commit_free(&target);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    if (target.parent_count == 0) {
        mgit_error("cannot revert initial commit (no parent to compare against)");
        commit_free(&target);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 读取 target 的 tree 和其 parent 的 tree */
    Tree target_tree = {0}, parent_tree = {0};
    if (commit_read_tree(store, &target_hash, &target_tree) != 0) {
        mgit_error("cannot read target tree");
        commit_free(&target);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }
    if (commit_read_tree(store, &target.parents[0], &parent_tree) != 0) {
        mgit_error("cannot read parent tree");
        tree_free(&target_tree);
        commit_free(&target);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 计算 target 相对于 parent 的变更，然后反向应用 */
    Index *idx = index_open(".git");
    if (!idx) {
        tree_free(&target_tree);
        tree_free(&parent_tree);
        commit_free(&target);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    int changes = 0;

    /* 反向应用：
     * - target 新增的文件（parent 没有）→ 从 index 删除
     * - target 删除的文件（parent 有，target 没有）→ 恢复到 index
     * - target 修改的文件 → 恢复为 parent 的版本
     */

    /* 扁平化两棵树（支持嵌套目录） */
    TreeFlatEntry *target_f = NULL, *parent_f = NULL;
    size_t target_c = 0, parent_c = 0;
    tree_flatten(store, &target_tree, &target_f, &target_c);
    tree_flatten(store, &parent_tree, &parent_f, &parent_c);

    /* 1. 遍历 target：找新增和修改的文件 */
    for (size_t i = 0; i < target_c; i++) {
        TreeFlatEntry *te = &target_f[i];
        TreeFlatEntry *pe = tree_flat_find(parent_f, parent_c, te->path);

        if (!pe) {
            /* target 新增的 → 反向：删除（同时删除工作区文件） */
            index_remove(idx, te->path);
            file_delete(te->path);
            changes++;
            printf("  revert delete: %s\n", te->path);
        } else if (!hash_equal(&te->hash, &pe->hash)) {
            /* target 修改的 → 反向：恢复 parent 版本（含工作区） */
            index_add(idx, te->path, &pe->hash, 0100644);
            restore_worktree_file(store, te->path, &pe->hash);
            changes++;
            printf("  revert modify: %s\n", te->path);
        }
    }

    /* 2. 遍历 parent：找 target 删除的文件 */
    for (size_t i = 0; i < parent_c; i++) {
        TreeFlatEntry *pe = &parent_f[i];
        TreeFlatEntry *te = tree_flat_find(target_f, target_c, pe->path);

        if (!te) {
            /* target 删除的 → 反向：恢复（含工作区） */
            index_add(idx, pe->path, &pe->hash, 0100644);
            restore_worktree_file(store, pe->path, &pe->hash);
            changes++;
            printf("  revert restore: %s\n", pe->path);
        }
    }

    free(target_f);
    free(parent_f);
    tree_free(&target_tree);
    tree_free(&parent_tree);

    if (changes == 0) {
        printf("No changes to revert.\n");
        commit_free(&target);
        index_close(idx);
        ref_manager_close(refs);
        object_store_close(store);
        return 0;
    }

    /* 创建 revert commit 的 tree */
    Hash tree_hash;
    if (index_write_tree(idx, store, &tree_hash) != 0) {
        mgit_error("failed to create tree");
        goto error;
    }

    /* 获取当前 HEAD 作为 parent */
    Hash head_hash;
    if (ref_resolve_head(refs, &head_hash) != 0) {
        mgit_error("no commits yet");
        goto error;
    }

    /* 创建 revert commit */
    char revert_msg[256];
    if (target.message) {
        /* 提取第一行作为简短描述 */
        char short_msg[128];
        strncpy(short_msg, target.message, sizeof(short_msg) - 1);
        short_msg[sizeof(short_msg) - 1] = 0;
        char *nl = strchr(short_msg, '\n');
        if (nl) *nl = 0;
        snprintf(revert_msg, sizeof(revert_msg), "Revert \"%s\"", short_msg);
    } else {
        snprintf(revert_msg, sizeof(revert_msg), "Revert previous commit");
    }

    Hash revert_commit;
    if (commit_create(store, &tree_hash, &head_hash, revert_msg, &revert_commit) != 0) {
        mgit_error("failed to create revert commit");
        goto error;
    }

    /* 更新分支 */
    char branch_ref[256];
    if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) != 0) {
        mgit_error("cannot get current branch");
        goto error;
    }
    if (ref_update(refs, branch_ref, &revert_commit) != 0) {
        mgit_error("failed to update branch");
        goto error;
    }

    const char *branch_name = branch_ref;
    if (strncmp(branch_ref, "refs/heads/", 11) == 0) {
        branch_name = branch_ref + 11;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&revert_commit, hex);
    printf("\n[%s %s] %s\n", branch_name, hex, revert_msg);
    printf("%d file(s) reverted\n", changes);

    /* 记录 reflog */
    {
        char old_hex[HASH_HEX_SIZE];
        hash_to_hex(&head_hash, old_hex);
        char action[512];
        snprintf(action, sizeof(action), "revert: %s", revert_msg);
        reflog_append(old_hex, hex, action);
    }

    commit_free(&target);
    index_close(idx);
    ref_manager_close(refs);
    object_store_close(store);
    return 0;

error:
    commit_free(&target);
    index_close(idx);
    ref_manager_close(refs);
    object_store_close(store);
    return -1;
}

Command cmd_revert = {
    .name = "revert",
    .description = "Create a commit that undoes a previous commit",
    .run = revert_run,
    .help = revert_help
};
