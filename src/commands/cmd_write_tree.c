#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../base/file.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/*
 * mgit write-tree
 * 
 * 将当前目录的文件结构创建为 tree 对象
 * 
 * 简化实现：只处理当前目录的文件
 * 不递归处理子目录（后续可以扩展）
 */

static void write_tree_help(void) {
    printf("usage: mgit write-tree\n\n");
    printf("Create a tree object from the current directory.\n");
}

static int write_tree_run(int argc, char **argv) {
    (void)argc;
    (void)argv;

    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("not a git repository");
        return -1;
    }

    Tree *tree = tree_new();
    if (!tree) {
        object_store_close(store);
        return -1;
    }

    /* 遍历当前目录 */
    DIR *dir = opendir(".");
    if (!dir) {
        mgit_error("cannot open current directory");
        tree_free(tree);
        object_store_close(store);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过 . 和 .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        /* 跳过 .git */
        if (strcmp(entry->d_name, ".git") == 0) {
            continue;
        }

        /* 读取文件内容 */
        uint8_t *data;
        size_t size;
        if (file_read_all(entry->d_name, &data, &size) != 0) {
            /* 可能是目录，跳过 */
            continue;
        }

        /* 写入 blob 对象 */
        Hash blob_hash;
        if (object_store_write(store, OBJ_BLOB, data, size, &blob_hash) != 0) {
            free(data);
            continue;
        }
        free(data);

        /* 添加到 tree */
        tree_add_entry(tree, "100644", entry->d_name, &blob_hash);
    }
    closedir(dir);

    /* 检查是否有文件 */
    if (tree->count == 0) {
        mgit_error("no files to add to tree");
        tree_free(tree);
        object_store_close(store);
        return -1;
    }

    /* 写入 tree 对象 */
    Hash tree_hash;
    if (tree_write(store, tree, &tree_hash) != 0) {
        tree_free(tree);
        object_store_close(store);
        return -1;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&tree_hash, hex);
    printf("%s\n", hex);

    tree_free(tree);
    object_store_close(store);
    return 0;
}

Command cmd_write_tree = {
    .name = "write-tree",
    .description = "Create a tree object from the current directory",
    .run = write_tree_run,
    .help = write_tree_help
};
