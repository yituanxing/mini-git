#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/graph.h"
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
 * mgit merge <branch>
 * 
 * 将指定分支合并到当前分支
 * 
 * 支持:
 * - Fast-Forward（快进合并）
 * - 三路合并（无冲突时自动合并）
 * - 文件级冲突检测与标记
 * 
 * 这是理解 Git 合并机制的核心命令：
 * - 找到共同祖先（merge base）
 * - 比较 base/ours/theirs 三棵树
 * - 逐文件决定保留哪一方
 */

static void merge_help(void) {
    printf("usage: mgit merge <branch>\n\n");
    printf("Join two or more development histories together.\n\n");
    printf("Merge the specified branch into the current branch.\n");
    printf("If possible, a fast-forward merge will be performed.\n");
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

/* 写冲突文件到工作区 */
static int write_conflict_file(const char *filename,
                               const uint8_t *ours_data, size_t ours_size,
                               const uint8_t *theirs_data, size_t theirs_size,
                               const char *ours_name, const char *theirs_name) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return -1;

    fprintf(fp, "<<<<<<< %s\n", ours_name);
    if (ours_data && ours_size > 0) {
        fwrite(ours_data, 1, ours_size, fp);
        if (ours_data[ours_size - 1] != '\n') fputc('\n', fp);
    }
    fprintf(fp, "=======\n");
    if (theirs_data && theirs_size > 0) {
        fwrite(theirs_data, 1, theirs_size, fp);
        if (theirs_data[theirs_size - 1] != '\n') fputc('\n', fp);
    }
    fprintf(fp, ">>>>>>> %s\n", theirs_name);

    fclose(fp);
    return 0;
}

/* 三路合并（基于扁平化文件列表，支持嵌套目录） */
static int three_way_merge(ObjectStore *store, Index *idx,
                           Tree *base, Tree *ours, Tree *theirs,
                           const char *ours_branch, const char *theirs_branch,
                           int *conflict_count) {
    *conflict_count = 0;
    int changes = 0;

    /* 扁平化三棵树 */
    TreeFlatEntry *base_f = NULL, *ours_f = NULL, *theirs_f = NULL;
    size_t base_c = 0, ours_c = 0, theirs_c = 0;
    tree_flatten(store, base, &base_f, &base_c);
    tree_flatten(store, ours, &ours_f, &ours_c);
    tree_flatten(store, theirs, &theirs_f, &theirs_c);

    /* 1. 遍历 ours 的每个文件 */
    for (size_t i = 0; i < ours_c; i++) {
        TreeFlatEntry *ours_e = &ours_f[i];
        TreeFlatEntry *base_e = tree_flat_find(base_f, base_c, ours_e->path);
        TreeFlatEntry *theirs_e = tree_flat_find(theirs_f, theirs_c, ours_e->path);

        if (theirs_e && hash_equal(&ours_e->hash, &theirs_e->hash)) {
            /* 两边一样，无需操作 */
            continue;
        }

        if (!theirs_e && base_e && hash_equal(&ours_e->hash, &base_e->hash)) {
            /* theirs 删除了，ours 没改 → 删除（含工作区文件） */
            index_remove(idx, ours_e->path);
            file_delete(ours_e->path);
            changes++;
            printf("  deleted:  %s\n", ours_e->path);
            continue;
        }

        if (!theirs_e && base_e && !hash_equal(&ours_e->hash, &base_e->hash)) {
            /* theirs 删除了，但 ours 有修改 → 冲突 */
            printf("  CONFLICT (delete/modify): %s\n", ours_e->path);
            (*conflict_count)++;
            continue;
        }

        if (!theirs_e && !base_e) {
            /* ours 新增的文件，theirs 没有 → 保留 ours */
            continue;
        }

        if (theirs_e && !base_e) {
            /* 两边都新增了同名文件 */
            if (!hash_equal(&ours_e->hash, &theirs_e->hash)) {
                /* 内容不同 → 冲突 */
                printf("  CONFLICT (add/add): %s\n", ours_e->path);
                /* 写冲突文件 */
                uint8_t *ours_data = NULL, *theirs_data = NULL;
                size_t ours_size = 0, theirs_size = 0;
                read_blob(store, &ours_e->hash, &ours_data, &ours_size);
                read_blob(store, &theirs_e->hash, &theirs_data, &theirs_size);
                ensure_parent_dir(ours_e->path);
                write_conflict_file(ours_e->path, ours_data, ours_size,
                                    theirs_data, theirs_size,
                                    ours_branch, theirs_branch);
                free(ours_data);
                free(theirs_data);
                (*conflict_count)++;
            }
            continue;
        }

        if (theirs_e && base_e) {
            int ours_changed = !hash_equal(&ours_e->hash, &base_e->hash);
            int theirs_changed = !hash_equal(&theirs_e->hash, &base_e->hash);

            if (ours_changed && !theirs_changed) {
                /* 只有 ours 改了 → 保留 ours */
                continue;
            }
            if (!ours_changed && theirs_changed) {
                /* 只有 theirs 改了 → 取 theirs */
                index_add(idx, ours_e->path, &theirs_e->hash, 0100644);
                uint8_t *data;
                size_t size;
                if (read_blob(store, &theirs_e->hash, &data, &size) == 0) {
                    ensure_parent_dir(ours_e->path);
                    FILE *fp = fopen(ours_e->path, "wb");
                    if (fp) { fwrite(data, 1, size, fp); fclose(fp); }
                    free(data);
                }
                changes++;
                printf("  updated:  %s\n", ours_e->path);
                continue;
            }
            if (ours_changed && theirs_changed) {
                /* 两边都改了 → 先试行级三路合并 */
                uint8_t *base_data = NULL, *ours_data = NULL, *theirs_data = NULL;
                size_t base_size = 0, ours_size = 0, theirs_size = 0;
                read_blob(store, &base_e->hash, &base_data, &base_size);
                read_blob(store, &ours_e->hash, &ours_data, &ours_size);
                read_blob(store, &theirs_e->hash, &theirs_data, &theirs_size);

                char *merged = NULL;
                size_t merged_size = 0;
                int lc = 0;
                int lm_ok = linemerge_3way(
                    (const char *)base_data, base_size,
                    (const char *)ours_data, ours_size,
                    (const char *)theirs_data, theirs_size,
                    ours_branch, theirs_branch,
                    &merged, &merged_size, &lc);

                ensure_parent_dir(ours_e->path);
                if (lm_ok == 0 && lc == 0) {
                    /* 行级自动合并成功 → 写 blob + index + 工作区 */
                    Hash blob_hash;
                    if (object_store_write(store, OBJ_BLOB, merged, merged_size,
                                           &blob_hash) == 0) {
                        index_add(idx, ours_e->path, &blob_hash, 0100644);
                        FILE *fp = fopen(ours_e->path, "wb");
                        if (fp) { fwrite(merged, 1, merged_size, fp); fclose(fp); }
                        changes++;
                        printf("  merged:   %s (line-level auto merge)\n", ours_e->path);
                    }
                } else {
                    /* 行级合并失败或仍有冲突块 → 写带标记的合并结果 */
                    printf("  CONFLICT (content): %s\n", ours_e->path);
                    if (lm_ok == 0 && merged) {
                        FILE *fp = fopen(ours_e->path, "wb");
                        if (fp) { fwrite(merged, 1, merged_size, fp); fclose(fp); }
                    } else {
                        /* 降级：整文件冲突 */
                        write_conflict_file(ours_e->path, ours_data, ours_size,
                                            theirs_data, theirs_size,
                                            ours_branch, theirs_branch);
                    }
                    (*conflict_count)++;
                }
                free(merged);
                free(base_data);
                free(ours_data);
                free(theirs_data);
            }
        }
    }

    /* 2. 遍历 theirs 中 ours 没有的文件（新增） */
    for (size_t i = 0; i < theirs_c; i++) {
        TreeFlatEntry *theirs_e = &theirs_f[i];
        TreeFlatEntry *ours_e = tree_flat_find(ours_f, ours_c, theirs_e->path);
        if (ours_e) continue;  /* 已在上面处理 */

        TreeFlatEntry *base_e = tree_flat_find(base_f, base_c, theirs_e->path);
        if (!base_e || !hash_equal(&theirs_e->hash, &base_e->hash)) {
            /* theirs 新增的文件 → 添加到 index */
            index_add(idx, theirs_e->path, &theirs_e->hash, 0100644);
            /* 写入工作区 */
            uint8_t *data;
            size_t size;
            if (read_blob(store, &theirs_e->hash, &data, &size) == 0) {
                ensure_parent_dir(theirs_e->path);
                FILE *fp = fopen(theirs_e->path, "wb");
                if (fp) { fwrite(data, 1, size, fp); fclose(fp); }
                free(data);
            }
            changes++;
            printf("  new file: %s\n", theirs_e->path);
        }
    }

    free(base_f);
    free(ours_f);
    free(theirs_f);
    return changes;
}

/*
 * 教学版 merge 采用保守的 pre-merge contract：
 * - Index 必须与 HEAD 一致（没有 staged changes）
 * - 已跟踪 Working Tree 必须与 Index 一致（没有 unstaged changes）
 * - 目标提交即将写入的路径不能覆盖本地 untracked 文件
 *
 * 真实 Git 对某些不重叠本地修改更宽松；mgit 宁可更严格，也不静默覆盖。
 */
static int merge_precheck(ObjectStore *store, Index *idx,
                          const Hash *ours_commit, const Hash *theirs_commit) {
    Commit ours;
    memset(&ours, 0, sizeof(ours));
    if (commit_read(store, ours_commit, &ours) != 0) return -1;

    Hash index_tree;
    if (index_write_tree(idx, store, &index_tree) != 0) {
        commit_free(&ours);
        return -1;
    }

    if (!hash_equal(&index_tree, &ours.tree)) {
        commit_free(&ours);
        mgit_error("cannot merge with staged changes");
        mgit_error("commit or reset the Index before merging");
        return -1;
    }
    commit_free(&ours);

    for (size_t i = 0; i < idx->count; i++) {
        uint8_t *data = NULL;
        size_t size = 0;
        if (file_read_all(idx->entries[i].name, &data, &size) != 0) {
            mgit_error("cannot merge with unstaged deletion: %s",
                       idx->entries[i].name);
            return -1;
        }

        Hash wt_hash;
        object_hash(OBJ_BLOB, data, size, &wt_hash);
        free(data);
        if (!hash_equal(&wt_hash, &idx->entries[i].hash)) {
            mgit_error("cannot merge with unstaged changes: %s",
                       idx->entries[i].name);
            mgit_error("commit or stash your changes before merging");
            return -1;
        }
    }

    Tree theirs_tree = {0};
    if (commit_read_tree(store, theirs_commit, &theirs_tree) != 0) return -1;

    TreeFlatEntry *flat = NULL;
    size_t count = 0;
    if (tree_flatten(store, &theirs_tree, &flat, &count) != 0) {
        tree_free(&theirs_tree);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        if (!index_find(idx, flat[i].path) && file_exists(flat[i].path)) {
            mgit_error("untracked file would be overwritten by merge: %s",
                       flat[i].path);
            free(flat);
            tree_free(&theirs_tree);
            return -1;
        }
    }

    free(flat);
    tree_free(&theirs_tree);
    return 0;
}

static int merge_run(int argc, char **argv) {
    if (argc < 2) {
        mgit_error("no branch specified");
        merge_help();
        return -1;
    }

    const char *theirs_branch = argv[1];

    /*
     * MERGE_HEAD is not garbage: it is the durable marker for an unfinished
     * merge.  Starting another merge would lose the second-parent identity
     * that the eventual commit needs, so reject instead of deleting it.
     */
    if (file_exists(".git/MERGE_HEAD")) {
        mgit_error("you have not concluded your previous merge");
        mgit_error("commit the resolved result or reset before merging again");
        return -1;
    }

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

    /* 获取当前分支（ours） */
    Hash ours_commit;
    if (ref_resolve_head(refs, &ours_commit) != 0) {
        mgit_error("no commits yet, nothing to merge into");
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    char ours_ref[256];
    if (ref_get_head_branch(refs, ours_ref, sizeof(ours_ref)) != 0 ||
        strcmp(ours_ref, "HEAD") == 0 ||
        strncmp(ours_ref, "refs/heads/", 11) != 0) {
        mgit_error("mgit merge requires a current branch");
        mgit_error("detached HEAD merge is intentionally not supported");
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 获取目标分支（theirs）：通用解析（分支/完整引用路径/tag），失败再当哈希 */
    Hash theirs_commit;
    if (ref_resolve_quiet(refs, theirs_branch, &theirs_commit) != 0) {
        /* 尝试作为 commit hash 解析 */
        if (hex_to_hash(theirs_branch, &theirs_commit) != 0) {
            mgit_error("branch '%s' not found", theirs_branch);
            ref_manager_close(refs);
            object_store_close(store);
            return -1;
        }
    }

    /* 检查是否已经合并（theirs 是 ours 的祖先） */
    if (graph_is_ancestor(store, &theirs_commit, &ours_commit)) {
        printf("Already up to date.\n");
        ref_manager_close(refs);
        object_store_close(store);
        return 0;
    }

    /* 当前分支已在上面验证为 refs/heads/<name>。 */
    char ours_branch[256];
    snprintf(ours_branch, sizeof(ours_branch), "%s", ours_ref + 11);

    Index *pre_idx = index_open(".git");
    if (!pre_idx) {
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }
    if (merge_precheck(store, pre_idx, &ours_commit, &theirs_commit) != 0) {
        index_close(pre_idx);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }
    index_close(pre_idx);

    /* 检查 Fast-Forward：ours 是 theirs 的祖先 */
    if (graph_is_ancestor(store, &ours_commit, &theirs_commit)) {
        /* Fast-Forward！直接移动分支指针 */
        printf("Fast-forward\n");

        char ff_ref[256];
        snprintf(ff_ref, sizeof(ff_ref), "refs/heads/%s", ours_branch);
        if (ref_update(refs, ff_ref, &theirs_commit) != 0) {
            mgit_error("failed to fast-forward");
            ref_manager_close(refs);
            object_store_close(store);
            return -1;
        }

        /* 同步工作区和 Index 到 theirs 的 tree */
        Tree ff_tree = {0};
        if (commit_read_tree(store, &theirs_commit, &ff_tree) == 0) {
            Index *idx = index_open(".git");
            if (idx) {
                tree_restore_worktree(store, idx, &ff_tree);
                index_write(idx);
                index_close(idx);
            }
            tree_free(&ff_tree);
        }

        char hex[HASH_HEX_SIZE];
        hash_to_hex(&theirs_commit, hex);
        printf("Fast-forward to %s\n", hex);

        /* 记录 reflog */
        {
            char old_hex[HASH_HEX_SIZE];
            hash_to_hex(&ours_commit, old_hex);
            char action[512];
            snprintf(action, sizeof(action), "merge %s: Fast-forward", theirs_branch);
            reflog_append(old_hex, hex, action);
        }

        ref_manager_close(refs);
        object_store_close(store);
        return 0;
    }

    /* 非 Fast-Forward：需要三路合并 */
    printf("Merging %s into %s...\n", theirs_branch, ours_branch);

    /* 1. 找 merge base */
    Hash base_commit;
    if (graph_find_merge_base(store, &ours_commit, &theirs_commit, &base_commit) != 0) {
        mgit_error("cannot find merge base (no common ancestor)");
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    char base_hex[HASH_HEX_SIZE];
    hash_to_hex(&base_commit, base_hex);
    printf("merge base: %.7s\n", base_hex);

    /* 2. 读取三棵树 */
    Tree base_tree = {0}, ours_tree = {0}, theirs_tree = {0};
    if (commit_read_tree(store, &base_commit, &base_tree) != 0) {
        mgit_error("cannot read base tree");
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }
    if (commit_read_tree(store, &ours_commit, &ours_tree) != 0) {
        mgit_error("cannot read ours tree");
        tree_free(&base_tree);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }
    if (commit_read_tree(store, &theirs_commit, &theirs_tree) != 0) {
        mgit_error("cannot read theirs tree");
        tree_free(&base_tree);
        tree_free(&ours_tree);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 3. 三路合并 */
    Index *idx = index_open(".git");
    if (!idx) {
        tree_free(&base_tree);
        tree_free(&ours_tree);
        tree_free(&theirs_tree);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    int conflict_count = 0;
    int changes = three_way_merge(store, idx, &base_tree, &ours_tree, &theirs_tree,
                                  ours_branch, theirs_branch, &conflict_count);

    tree_free(&base_tree);
    tree_free(&ours_tree);
    tree_free(&theirs_tree);

    if (conflict_count > 0) {
        printf("\nAutomatic merge failed; fix conflicts and then commit the result.\n");
        printf("(%d conflict(s) detected)\n", conflict_count);

        /* 写 MERGE_HEAD：下次 commit 时生成双亲 merge commit */
        {
            char theirs_hex[HASH_HEX_SIZE];
            hash_to_hex(&theirs_commit, theirs_hex);
            file_write_all(".git/MERGE_HEAD", (const uint8_t *)theirs_hex,
                           strlen(theirs_hex));
        }

        /* 保存 index（包含合并结果） */
        index_write(idx);
        index_close(idx);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 4. 无冲突：创建 tree 和 merge commit */
    Hash tree_hash;
    if (index_write_tree(idx, store, &tree_hash) != 0) {
        mgit_error("failed to create tree from merged index");
        index_close(idx);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 创建 merge commit（双亲） */
    char merge_msg[256];
    snprintf(merge_msg, sizeof(merge_msg), "Merge %s into %s", theirs_branch, ours_branch);

    Hash merge_commit;
    if (commit_create_merge(store, &tree_hash, &ours_commit, &theirs_commit,
                            merge_msg, &merge_commit) != 0) {
        mgit_error("failed to create merge commit");
        index_close(idx);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    /* 更新分支指针 */
    char branch_ref[256];
    snprintf(branch_ref, sizeof(branch_ref), "refs/heads/%s", ours_branch);
    if (ref_update(refs, branch_ref, &merge_commit) != 0) {
        mgit_error("failed to update branch");
        index_close(idx);
        ref_manager_close(refs);
        object_store_close(store);
        return -1;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&merge_commit, hex);
    printf("\n%s\n", merge_msg);
    printf("Merge commit: %s\n", hex);
    if (changes > 0) {
        printf("%d file(s) changed\n", changes);
    }

    /* 记录 reflog */
    {
        char old_hex[HASH_HEX_SIZE];
        hash_to_hex(&ours_commit, old_hex);
        char action[512];
        snprintf(action, sizeof(action), "merge %s: %s", theirs_branch, merge_msg);
        reflog_append(old_hex, hex, action);
    }

    index_close(idx);
    ref_manager_close(refs);
    object_store_close(store);
    return 0;
}

Command cmd_merge = {
    .name = "merge",
    .description = "Join two or more development histories together",
    .run = merge_run,
    .help = merge_help
};
