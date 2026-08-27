#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/ref.h"
#include "../core/index.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * mgit diff [--cached] [<tree-hash>] [<tree-hash>]
 * 
 * 比较差异
 * 
 * 支持:
 * - mgit diff                    比较 HEAD 与其 parent
 * - mgit diff --cached           比较 Index（暂存区）与 HEAD
 * - mgit diff <tree-hash>        比较指定 tree 与 HEAD
 * - mgit diff <tree1> <tree2>    比较两棵 tree
 */

static void diff_help(void) {
    printf("usage: mgit diff [--cached] [<tree-hash>] [<tree-hash>]\n\n");
    printf("Show differences between trees or index.\n\n");
    printf("Options:\n");
    printf("    --cached    Show staged changes (index vs HEAD)\n");
}

static int diff_trees(ObjectStore *store, const Hash *hash1, const Hash *hash2) {
    Object obj1, obj2;
    Tree tree1 = {0}, tree2 = {0};

    if (object_store_read(store, hash1, &obj1) != 0) {
        mgit_error("cannot read tree object");
        return -1;
    }
    if (obj1.type != OBJ_TREE) {
        mgit_error("not a tree object");
        object_free(&obj1);
        return -1;
    }
    tree_parse(obj1.data, obj1.size, &tree1);
    object_free(&obj1);

    if (object_store_read(store, hash2, &obj2) != 0) {
        mgit_error("cannot read tree object");
        tree_free(&tree1);
        return -1;
    }
    if (obj2.type != OBJ_TREE) {
        mgit_error("not a tree object");
        object_free(&obj2);
        tree_free(&tree1);
        return -1;
    }
    tree_parse(obj2.data, obj2.size, &tree2);
    object_free(&obj2);

    int changes = 0;

    /* 扁平化两棵树（支持嵌套目录） */
    TreeFlatEntry *flat1 = NULL, *flat2 = NULL;
    size_t count1 = 0, count2 = 0;
    tree_flatten(store, &tree1, &flat1, &count1);
    tree_flatten(store, &tree2, &flat2, &count2);

    for (size_t i = 0; i < count2; i++) {
        TreeFlatEntry *e2 = &flat2[i];
        TreeFlatEntry *e1 = tree_flat_find(flat1, count1, e2->path);

        if (!e1) {
            printf("+ %s (new)\n", e2->path);
            changes++;
        } else if (!hash_equal(&e1->hash, &e2->hash)) {
            printf("~ %s (modified)\n", e2->path);
            changes++;
        }
    }

    for (size_t i = 0; i < count1; i++) {
        TreeFlatEntry *e1 = &flat1[i];
        TreeFlatEntry *e2 = tree_flat_find(flat2, count2, e1->path);

        if (!e2) {
            printf("- %s (deleted)\n", e1->path);
            changes++;
        }
    }

    if (changes == 0) {
        printf("(no differences)\n");
    }

    free(flat1);
    free(flat2);
    tree_free(&tree1);
    tree_free(&tree2);
    return 0;
}

/* --cached: 比较 Index 与 HEAD 的 tree */
static int diff_cached(ObjectStore *store, RefManager *refs) {
    Index *idx = index_open(".git");
    if (!idx) {
        mgit_error("cannot open index");
        return -1;
    }

    /* 获取 HEAD 的 tree */
    Tree head_tree = {0};
    Hash head_hash;
    int has_head = (ref_resolve_head(refs, &head_hash) == 0);

    if (has_head) {
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
                    }
                    object_free(&tree_obj);
                }
                commit_free(&commit);
            }
            object_free(&obj);
        }
    }

    int changes = 0;

    /* 扁平化 HEAD tree（支持嵌套目录） */
    TreeFlatEntry *head_f = NULL;
    size_t head_c = 0;
    tree_flatten(store, &head_tree, &head_f, &head_c);

    /* 遍历 Index，与 HEAD tree 比较 */
    for (size_t i = 0; i < idx->count; i++) {
        IndexEntry *entry = &idx->entries[i];
        TreeFlatEntry *te = tree_flat_find(head_f, head_c, entry->name);

        if (!te) {
            printf("+ %s (new file)\n", entry->name);
            changes++;
        } else if (!hash_equal(&te->hash, &entry->hash)) {
            printf("~ %s (modified)\n", entry->name);
            changes++;
        }
    }

    /* 查找 HEAD 中有但 Index 中没有的（被删除） */
    for (size_t i = 0; i < head_c; i++) {
        TreeFlatEntry *te = &head_f[i];
        int found = 0;
        for (size_t j = 0; j < idx->count; j++) {
            if (strcmp(te->path, idx->entries[j].name) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("- %s (deleted)\n", te->path);
            changes++;
        }
    }

    if (changes == 0) {
        printf("(no staged changes)\n");
    }

    free(head_f);
    tree_free(&head_tree);
    index_close(idx);
    return 0;
}

static int diff_run(int argc, char **argv) {
    int cached = 0;
    const char *hash_args[2] = {NULL, NULL};
    int hash_count = 0;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cached") == 0) {
            cached = 1;
        } else if (argv[i][0] != '-' && hash_count < 2) {
            hash_args[hash_count++] = argv[i];
        }
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

    int ret = 0;

    if (cached) {
        /* --cached: 比较 Index 与 HEAD */
        ret = diff_cached(store, refs);
    } else if (hash_count == 2) {
        Hash hash1, hash2;
        if (hex_to_hash(hash_args[0], &hash1) != 0 || hex_to_hash(hash_args[1], &hash2) != 0) {
            mgit_error("invalid tree hash");
            ret = -1;
        } else {
            ret = diff_trees(store, &hash1, &hash2);
        }
    } else if (hash_count == 1) {
        Hash hash1;
        if (hex_to_hash(hash_args[0], &hash1) != 0) {
            mgit_error("invalid tree hash");
            ret = -1;
        } else {
            Hash head_hash;
            if (ref_resolve_head(refs, &head_hash) != 0) {
                mgit_error("no commits yet");
                ret = -1;
            } else {
                Object obj;
                memset(&obj, 0, sizeof(obj));
                if (object_store_read(store, &head_hash, &obj) != 0) {
                    mgit_error("cannot read HEAD commit");
                    ret = -1;
                } else if (obj.type != OBJ_COMMIT) {
                    mgit_error("cannot read HEAD commit");
                    object_free(&obj);
                    ret = -1;
                } else {
                    Commit commit;
                    memset(&commit, 0, sizeof(commit));
                    commit_parse(obj.data, obj.size, &commit);
                    object_free(&obj);
                    ret = diff_trees(store, &commit.tree, &hash1);
                    commit_free(&commit);
                }
            }
        }
    } else {
        /* 无参数：比较 HEAD 与其 parent */
        Hash head_hash;
        if (ref_resolve_head(refs, &head_hash) != 0) {
            mgit_error("no commits yet");
            ret = -1;
        } else {
            Object obj;
            memset(&obj, 0, sizeof(obj));
            if (object_store_read(store, &head_hash, &obj) != 0) {
                mgit_error("cannot read HEAD commit");
                ret = -1;
            } else if (obj.type != OBJ_COMMIT) {
                mgit_error("cannot read HEAD commit");
                object_free(&obj);
                ret = -1;
            } else {
                Commit commit;
                memset(&commit, 0, sizeof(commit));
                commit_parse(obj.data, obj.size, &commit);
                object_free(&obj);

                if (commit.parent_count == 0) {
                    printf("Initial commit - all files are new:\n");
                    Object tree_obj;
                    memset(&tree_obj, 0, sizeof(tree_obj));
                    if (object_store_read(store, &commit.tree, &tree_obj) == 0) {
                        if (tree_obj.type == OBJ_TREE) {
                            Tree tree = {0};
                            tree_parse(tree_obj.data, tree_obj.size, &tree);
                            TreeFlatEntry *flat = NULL;
                            size_t fc = 0;
                            tree_flatten(store, &tree, &flat, &fc);
                            for (size_t i = 0; i < fc; i++) {
                                printf("+ %s (new)\n", flat[i].path);
                            }
                            if (fc == 0) printf("(empty tree)\n");
                            free(flat);
                            tree_free(&tree);
                        }
                        object_free(&tree_obj);
                    }
                } else {
                    Object parent_obj;
                    memset(&parent_obj, 0, sizeof(parent_obj));
                    if (object_store_read(store, &commit.parents[0], &parent_obj) != 0) {
                        mgit_error("cannot read parent commit");
                        ret = -1;
                    } else if (parent_obj.type != OBJ_COMMIT) {
                        mgit_error("cannot read parent commit");
                        object_free(&parent_obj);
                        ret = -1;
                    } else {
                        Commit parent;
                        memset(&parent, 0, sizeof(parent));
                        commit_parse(parent_obj.data, parent_obj.size, &parent);
                        object_free(&parent_obj);

                        char hex1[HASH_HEX_SIZE], hex2[HASH_HEX_SIZE];
                        hash_to_hex(&parent.tree, hex1);
                        hash_to_hex(&commit.tree, hex2);
                        printf("diff %s..%s\n", hex1 + 7, hex2 + 7);

                        ret = diff_trees(store, &parent.tree, &commit.tree);
                        commit_free(&parent);
                    }
                }
                commit_free(&commit);
            }
        }
    }

    ref_manager_close(refs);
    object_store_close(store);
    return ret;
}

Command cmd_diff = {
    .name = "diff",
    .description = "Show differences between trees or index",
    .run = diff_run,
    .help = diff_help
};
