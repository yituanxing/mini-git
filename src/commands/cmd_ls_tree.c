#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/commit.h"
#include "../core/ref.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit ls-tree <tree>
 * 
 * 列出 tree 对象的内容
 * 
 * 这是理解 tree 结构的关键命令：
 * - 可以看到 tree 包含哪些文件/子目录
 * - 每个条目的 mode、类型、hash、文件名
 */

static void ls_tree_help(void) {
    printf("usage: mgit ls-tree <tree|commit|branch|HEAD>\n\n");
    printf("List the contents of a tree object.\n");
}

static int ls_tree_run(int argc, char **argv) {
    const char *tree_str = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            tree_str = argv[i];
            break;
        }
    }

    if (!tree_str) {
        mgit_error("no tree specified");
        ls_tree_help();
        return -1;
    }

    /* 解析参数：HEAD / 分支名 / 哈希 */
    Hash hash;
    int resolved = 0;
    if (strcmp(tree_str, "HEAD") == 0) {
        RefManager *refs = ref_manager_open(".git");
        if (refs) {
            resolved = (ref_resolve_head(refs, &hash) == 0);
            ref_manager_close(refs);
        }
    } else if (hex_to_hash(tree_str, &hash) == 0) {
        resolved = 1;
    } else {
        RefManager *refs = ref_manager_open(".git");
        if (refs) {
            resolved = (ref_resolve_quiet(refs, tree_str, &hash) == 0);
            ref_manager_close(refs);
        }
        /* 短哈希前缀（含 pack 内对象） */
        if (!resolved) {
            size_t tlen = strlen(tree_str);
            if (tlen >= 4 && tlen < 40) {
                ObjectStore *tmp = object_store_open(".git");
                if (tmp) {
                    resolved = (object_find_by_prefix(tmp, tree_str, OBJ_NONE, &hash) == 0);
                    object_store_close(tmp);
                }
            }
        }
    }
    if (!resolved) {
        mgit_error("invalid hash: %s", tree_str);
        return -1;
    }

    /* 打开对象存储 */
    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 读取对象；若是 commit 则取其 tree */
    Hash tree_hash = hash;
    Object first;
    if (object_store_read(store, &hash, &first) != 0) {
        object_store_close(store);
        return -1;
    }
    if (first.type == OBJ_COMMIT) {
        Commit commit;
        memset(&commit, 0, sizeof(commit));
        commit_parse(first.data, first.size, &commit);
        tree_hash = commit.tree;
        commit_free(&commit);
    }
    object_free(&first);

    /* 读取 tree 对象 */
    Object obj;
    if (object_store_read(store, &tree_hash, &obj) != 0) {
        object_store_close(store);
        return -1;
    }

    if (obj.type != OBJ_TREE) {
        mgit_error("object is not a tree");
        object_free(&obj);
        object_store_close(store);
        return -1;
    }

    /* 解析 tree */
    Tree tree;
    memset(&tree, 0, sizeof(tree));
    if (tree_parse(obj.data, obj.size, &tree) != 0) {
        object_free(&obj);
        object_store_close(store);
        return -1;
    }

    /* 输出 */
    for (size_t i = 0; i < tree.count; i++) {
        char hex[HASH_HEX_SIZE];
        hash_to_hex(&tree.entries[i].hash, hex);
        
        const char *type = tree.entries[i].type == TREE_ENTRY_TREE ? "tree" : "blob";
        printf("%s %s %s\t%s\n", 
               tree.entries[i].mode,
               type,
               hex,
               tree.entries[i].name);
    }

    tree_free(&tree);
    object_free(&obj);
    object_store_close(store);
    return 0;
}

Command cmd_ls_tree = {
    .name = "ls-tree",
    .description = "List the contents of a tree object",
    .run = ls_tree_run,
    .help = ls_tree_help
};
