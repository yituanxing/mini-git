#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/ref.h"
#include "../core/index.h"
#include "../core/linemerge.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * mgit cherry-pick <commit>
 *
 * 把指定 commit 的改动"摘"到当前分支，生成一个新 commit
 *
 * 原理：
 * - 计算该 commit 相对其 parent 的变更（diff）
 * - 将变更应用到当前 Index/工作区
 * - 用原 commit 的提交信息创建新 commit
 *
 * 冲突处理：
 * - 当前 Index 中同一文件已有不同修改 → 写冲突标记文件
 * - 有冲突时不自动提交，与真实 Git 行为一致
 *
 * cherry_pick_commit() 被 rebase 复用（rebase = 批量 cherry-pick）
 */

static void cherry_pick_help(void) {
    printf("usage: mgit cherry-pick <commit>\n\n");
    printf("Apply the changes introduced by an existing commit.\n");
    printf("A new commit is created on the current branch.\n");
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
    memset(&obj, 0, sizeof(obj));
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
static void write_worktree_file(ObjectStore *store, const char *name, const Hash *blob) {
    uint8_t *data;
    size_t size;
    if (read_blob(store, blob, &data, &size) == 0) {
        ensure_parent_dir(name);
        file_write_all(name, data, size);
        free(data);
    }
}

/* 写冲突文件（ours 来自 Index，theirs 来自被 pick 的 commit） */
static void write_conflict(ObjectStore *store, const char *path,
                           const Hash *ours_blob, const Hash *theirs_blob,
                           const char *theirs_label) {
    uint8_t *ours_data = NULL, *theirs_data = NULL;
    size_t ours_size = 0, theirs_size = 0;
    if (ours_blob) read_blob(store, ours_blob, &ours_data, &ours_size);
    if (theirs_blob) read_blob(store, theirs_blob, &theirs_data, &theirs_size);

    ensure_parent_dir(path);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fprintf(fp, "<<<<<<< HEAD\n");
        if (ours_data && ours_size > 0) {
            fwrite(ours_data, 1, ours_size, fp);
            if (ours_data[ours_size - 1] != '\n') fputc('\n', fp);
        }
        fprintf(fp, "=======\n");
        if (theirs_data && theirs_size > 0) {
            fwrite(theirs_data, 1, theirs_size, fp);
            if (theirs_data[theirs_size - 1] != '\n') fputc('\n', fp);
        }
        fprintf(fp, ">>>>>>> %s\n", theirs_label);
        fclose(fp);
    }
    free(ours_data);
    free(theirs_data);
}

/* 解析短哈希或引用名为完整 commit hash */
static int resolve_commit(ObjectStore *store, RefManager *refs, const char *str, Hash *out) {
    size_t len = strlen(str);
    if (len == 40 && hex_to_hash(str, out) == 0) return 0;
    if (len >= 4 && len < 40) {
        if (object_find_by_prefix(store, str, OBJ_COMMIT, out) == 0) return 0;
    }
    if (ref_resolve_quiet(refs, str, out) == 0) return 0;
    return -1;
}

/*
 * cherry-pick 核心：应用 + 提交（供 rebase 复用）
 *
 * @return 0  成功提交
 *         1  有冲突，未提交（工作区已写冲突标记）
 *         2  无实际变更，未提交
 *        -1  致命错误
 */
int cherry_pick_commit(ObjectStore *store, RefManager *refs, Index *idx,
                       const Hash *target_hash, Hash *new_commit_out) {
    /* 读取目标 commit */
    Commit target;
    memset(&target, 0, sizeof(target));
    if (commit_read(store, target_hash, &target) != 0) {
        mgit_error("cannot read commit");
        return -1;
    }

    /* 读取 target tree 和 parent tree（初始提交用空 base） */
    Tree target_tree = {0}, parent_tree = {0};
    if (commit_read_tree(store, target_hash, &target_tree) != 0) {
        mgit_error("cannot read target tree");
        commit_free(&target);
        return -1;
    }
    int has_parent = 0;
    if (target.parent_count > 0) {
        if (commit_read_tree(store, &target.parents[0], &parent_tree) != 0) {
            mgit_error("cannot read parent tree");
            tree_free(&target_tree);
            commit_free(&target);
            return -1;
        }
        has_parent = 1;
    }

    TreeFlatEntry *target_f = NULL, *parent_f = NULL;
    size_t target_c = 0, parent_c = 0;
    tree_flatten(store, &target_tree, &target_f, &target_c);
    if (has_parent) {
        tree_flatten(store, &parent_tree, &parent_f, &parent_c);
    }

    char label[64];
    char full_hex[HASH_HEX_SIZE];
    hash_to_hex(target_hash, full_hex);
    snprintf(label, sizeof(label), "%.7s", full_hex);

    int conflicts = 0;
    int changes = 0;

    /* 1. 遍历 target：新增与修改 */
    for (size_t i = 0; i < target_c; i++) {
        TreeFlatEntry *te = &target_f[i];
        TreeFlatEntry *pe = tree_flat_find(parent_f, parent_c, te->path);
        IndexEntry *ie = index_find(idx, te->path);

        if (!pe) {
            /* commit 新增的文件 */
            if (!ie) {
                index_add(idx, te->path, &te->hash, 0100644);
                write_worktree_file(store, te->path, &te->hash);
                changes++;
            } else if (!hash_equal(&ie->hash, &te->hash)) {
                /* 本地已有不同内容 → add/add 冲突 */
                printf("  CONFLICT (add/add): %s\n", te->path);
                write_conflict(store, te->path, &ie->hash, &te->hash, label);
                conflicts++;
            }
        } else if (!hash_equal(&te->hash, &pe->hash)) {
            /* commit 修改的文件 */
            if (ie && hash_equal(&ie->hash, &pe->hash)) {
                /* 本地没动过 → 直接应用 */
                index_add(idx, te->path, &te->hash, 0100644);
                write_worktree_file(store, te->path, &te->hash);
                changes++;
            } else if (ie && hash_equal(&ie->hash, &te->hash)) {
                /* 本地已经是目标内容 → 跳过 */
            } else if (!ie) {
                /* 本地已删除该文件 → modify/delete 冲突 */
                printf("  CONFLICT (modify/delete): %s\n", te->path);
                conflicts++;
            } else {
                /* 本地有不同修改 → 先试行级三路合并（base=parent 版本） */
                uint8_t *base_data = NULL, *ours_data = NULL, *theirs_data = NULL;
                size_t base_size = 0, ours_size = 0, theirs_size = 0;
                read_blob(store, &pe->hash, &base_data, &base_size);
                read_blob(store, &ie->hash, &ours_data, &ours_size);
                read_blob(store, &te->hash, &theirs_data, &theirs_size);

                char *merged = NULL;
                size_t merged_size = 0;
                int lc = 0;
                int lm_ok = linemerge_3way(
                    (const char *)base_data, base_size,
                    (const char *)ours_data, ours_size,
                    (const char *)theirs_data, theirs_size,
                    "HEAD", label, &merged, &merged_size, &lc);

                ensure_parent_dir(te->path);
                if (lm_ok == 0 && lc == 0) {
                    Hash blob_hash;
                    if (object_store_write(store, OBJ_BLOB, merged, merged_size,
                                           &blob_hash) == 0) {
                        index_add(idx, te->path, &blob_hash, 0100644);
                        file_write_all(te->path, (const uint8_t *)merged, merged_size);
                        changes++;
                        printf("  merged: %s (line-level auto merge)\n", te->path);
                    }
                } else {
                    printf("  CONFLICT (content): %s\n", te->path);
                    if (lm_ok == 0 && merged) {
                        file_write_all(te->path, (const uint8_t *)merged, merged_size);
                    } else {
                        write_conflict(store, te->path, &ie->hash, &te->hash, label);
                    }
                    conflicts++;
                }
                free(merged);
                free(base_data);
                free(ours_data);
                free(theirs_data);
            }
        }
    }

    /* 2. 遍历 parent：commit 删除的文件 */
    for (size_t i = 0; i < parent_c; i++) {
        TreeFlatEntry *pe = &parent_f[i];
        if (tree_flat_find(target_f, target_c, pe->path)) continue;

        IndexEntry *ie = index_find(idx, pe->path);
        if (!ie) {
            /* 本地已没有 → 无需操作 */
        } else if (hash_equal(&ie->hash, &pe->hash)) {
            index_remove(idx, pe->path);
            file_delete(pe->path);
            changes++;
        } else {
            printf("  CONFLICT (modify/delete): %s\n", pe->path);
            conflicts++;
        }
    }

    free(target_f);
    free(parent_f);
    tree_free(&target_tree);
    tree_free(&parent_tree);

    if (conflicts > 0) {
        commit_free(&target);
        return 1;
    }
    if (changes == 0) {
        commit_free(&target);
        return 2;
    }

    /* 创建 commit：tree 来自 Index，parent 是当前 HEAD，沿用原提交信息 */
    Hash tree_hash;
    if (index_write_tree(idx, store, &tree_hash) != 0) {
        mgit_error("failed to create tree");
        commit_free(&target);
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

    const char *msg = target.message ? target.message : "cherry-picked commit";
    Hash new_commit;
    int ret = commit_create(store, &tree_hash,
                            has_head ? &head_hash : NULL, msg, &new_commit);
    if (ret != 0) {
        mgit_error("failed to create commit");
        commit_free(&target);
        return -1;
    }

    /* 更新当前分支 */
    char branch_ref[256];
    if (ref_get_head_branch(refs, branch_ref, sizeof(branch_ref)) != 0) {
        mgit_error("cannot get current branch");
        commit_free(&target);
        return -1;
    }
    if (ref_update(refs, branch_ref, &new_commit) != 0) {
        mgit_error("failed to update branch");
        commit_free(&target);
        return -1;
    }

    /* 输出与 reflog */
    char new_hex[HASH_HEX_SIZE];
    hash_to_hex(&new_commit, new_hex);
    const char *branch_name = branch_ref;
    if (strncmp(branch_ref, "refs/heads/", 11) == 0) branch_name += 11;

    char short_msg[128];
    strncpy(short_msg, msg, sizeof(short_msg) - 1);
    short_msg[sizeof(short_msg) - 1] = 0;
    char *nl = strchr(short_msg, '\n');
    if (nl) *nl = 0;
    printf("[%s %s] %s\n", branch_name, new_hex, short_msg);

    char action[512];
    snprintf(action, sizeof(action), "cherry-pick: %s", short_msg);
    reflog_append(old_hex, new_hex, action);

    if (new_commit_out) *new_commit_out = new_commit;
    commit_free(&target);
    return 0;
}

static int cherry_pick_run(int argc, char **argv) {
    if (argc < 2) {
        mgit_error("no commit specified");
        cherry_pick_help();
        return -1;
    }

    ObjectStore *store = object_store_open(".git");
    if (!store) { mgit_error("not a git repository"); return -1; }
    RefManager *refs = ref_manager_open(".git");
    if (!refs) { object_store_close(store); return -1; }
    Index *idx = index_open(".git");
    if (!idx) {
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    Hash target_hash;
    if (resolve_commit(store, refs, argv[1], &target_hash) != 0) {
        mgit_error("unknown commit: %s", argv[1]);
        index_close(idx);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    char hex7[8];
    char full[HASH_HEX_SIZE];
    hash_to_hex(&target_hash, full);
    snprintf(hex7, sizeof(hex7), "%.7s", full);

    int r = cherry_pick_commit(store, refs, idx, &target_hash, NULL);

    if (r == 1) {
        printf("hint: after resolving conflicts, run 'mgit add' and "
               "'mgit cherry-pick --continue'\n");
    } else if (r == 2) {
        printf("The cherry-pick of %s is empty (no changes to apply).\n", hex7);
    }

    /* 保存 Index（冲突场景下也保留当前状态） */
    if (idx->dirty) index_write(idx);

    index_close(idx);
    ref_manager_close(refs);
    object_store_close(store);
    return (r == 0 || r == 2) ? 0 : -1;
}

Command cmd_cherry_pick = {
    .name = "cherry-pick",
    .description = "Apply a commit onto the current branch",
    .run = cherry_pick_run,
    .help = cherry_pick_help
};
