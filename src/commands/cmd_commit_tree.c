#include "../command.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../core/ref.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit commit-tree <tree> [-p <parent>] [-m <message>]
 * 
 * 创建一个 commit 对象
 * 
 * 这是理解 commit 结构的关键命令：
 * - commit 指向一个 tree（快照）
 * - commit 可以有 parent（形成链表）
 * - commit 包含作者、时间、消息等信息
 */

static void commit_tree_help(void) {
    printf("usage: mgit commit-tree <tree> [-p <parent>] [-m <message>]\n\n");
    printf("Create a commit object from a tree.\n\n");
    printf("Options:\n");
    printf("    -p <parent>     Parent commit hash (can be specified multiple times)\n");
    printf("    -m <message>    Commit message\n");
}

static int commit_tree_run(int argc, char **argv) {
    const char *tree_str = NULL;
    const char *parent_str = NULL;
    const char *message = "mgit commit";

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            parent_str = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[++i];
        } else if (argv[i][0] != '-') {
            tree_str = argv[i];
        }
    }

    if (!tree_str) {
        mgit_error("no tree specified");
        commit_tree_help();
        return -1;
    }

    /* 解析 tree 哈希 */
    Hash tree_hash;
    if (hex_to_hash(tree_str, &tree_hash) != 0) {
        mgit_error("invalid tree hash: %s", tree_str);
        return -1;
    }

    /* 解析 parent 哈希（如果有） */
    Hash parent_hash;
    Hash *parent_ptr = NULL;
    if (parent_str) {
        if (hex_to_hash(parent_str, &parent_hash) != 0) {
            mgit_error("invalid parent hash: %s", parent_str);
            return -1;
        }
        parent_ptr = &parent_hash;
    }

    /* 打开对象存储 */
    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 创建 commit */
    Hash commit_hash;
    if (commit_create(store, &tree_hash, parent_ptr, message, &commit_hash) != 0) {
        object_store_close(store);
        return -1;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&commit_hash, hex);
    printf("%s\n", hex);

    object_store_close(store);
    return 0;
}

Command cmd_commit_tree = {
    .name = "commit-tree",
    .description = "Create a commit object from a tree",
    .run = commit_tree_run,
    .help = commit_tree_help
};
