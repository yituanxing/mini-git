#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../core/index.h"
#include "../base/file.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * mgit write-tree
 *
 * 从暂存区（Index）创建 tree 对象（与真实 git 一致：
 * 树的内容由 index 决定，而不是扫描工作区）。
 * 递归子目录，嵌套目录生成嵌套 tree。
 */

static void write_tree_help(void) {
    printf("usage: mgit write-tree\n\n");
    printf("Create a tree object from the current index.\n");
}

static int write_tree_run(int argc, char **argv) {
    (void)argc;
    (void)argv;

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

    if (idx->count == 0) {
        index_close(idx);
        object_store_close(store);
        mgit_error("no files in index to add to tree");
        return -1;
    }

    /* 从 index 逐层构建嵌套 tree（与 commit 用的是同一个函数） */
    Hash tree_hash;
    if (index_write_tree(idx, store, &tree_hash) != 0) {
        index_close(idx);
        object_store_close(store);
        return -1;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&tree_hash, hex);
    printf("%s\n", hex);

    index_close(idx);
    object_store_close(store);
    return 0;
}

Command cmd_write_tree = {
    .name = "write-tree",
    .description = "Create a tree object from the current directory",
    .run = write_tree_run,
    .help = write_tree_help
};
