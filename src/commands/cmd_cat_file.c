#include "../command.h"
#include "../core/object.h"
#include "../core/tree.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit cat-file [-p] [-t] [-s] <object>
 * 
 * 读取并显示对象内容
 * 
 * 选项：
 *   -p    根据对象类型美化输出
 *   -t    只输出对象类型
 *   -s    只输出对象大小
 * 
 * 这是理解 Git 对象存储的关键命令：
 * - 可以看到对象内部结构
 * - 验证对象是否正确存储
 */

static void cat_file_help(void) {
    printf("usage: mgit cat-file [-p] [-t] [-s] <object>\n\n");
    printf("Display the content of a Git object.\n\n");
    printf("Options:\n");
    printf("    -p    Pretty-print based on object type\n");
    printf("    -t    Print only the object type\n");
    printf("    -s    Print only the object size\n");
}

static int cat_file_run(int argc, char **argv) {
    int show_type = 0;
    int show_size = 0;
    int pretty = 0;
    const char *hash_str = NULL;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            show_type = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            show_size = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            pretty = 1;
        } else if (argv[i][0] != '-') {
            hash_str = argv[i];
        } else {
            mgit_error("unknown option: %s", argv[i]);
            return -1;
        }
    }

    if (!hash_str) {
        mgit_error("no object specified");
        cat_file_help();
        return -1;
    }

    /* 解析哈希（支持完整哈希与短哈希前缀） */
    Hash hash;
    size_t hlen = strlen(hash_str);
    int resolved = 0;
    if (hlen == 40 && hex_to_hash(hash_str, &hash) == 0) {
        resolved = 1;
    } else if (hlen >= 4 && hlen < 40) {
        ObjectStore *tmp = object_store_open(".git");
        if (tmp) {
            resolved = (object_find_by_prefix(tmp, hash_str, OBJ_NONE, &hash) == 0);
            object_store_close(tmp);
        }
    }
    if (!resolved) {
        mgit_error("invalid hash: %s", hash_str);
        return -1;
    }

    /* 打开对象存储 */
    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 读取对象 */
    Object obj;
    if (object_store_read(store, &hash, &obj) != 0) {
        object_store_close(store);
        return -1;
    }
    object_store_close(store);

    /* 输出 */
    if (show_type) {
        printf("%s\n", object_type_name(obj.type));
    } else if (show_size) {
        printf("%lu\n", (unsigned long)obj.size);
    } else if (pretty) {
        /* 根据类型美化输出 */
        if (obj.type == OBJ_BLOB) {
            /* blob: 直接输出内容 */
            fwrite(obj.data, 1, obj.size, stdout);
        } else if (obj.type == OBJ_COMMIT) {
            /* commit: 格式化输出 */
            printf("commit %s\n", hash_str);
            fwrite(obj.data, 1, obj.size, stdout);
        } else if (obj.type == OBJ_TREE) {
            /* tree: 按真实 git 的格式逐条列出（<mode> <type> <hash>\t<name>） */
            Tree tree;
            memset(&tree, 0, sizeof(tree));
            if (tree_parse(obj.data, obj.size, &tree) != 0) {
                object_free(&obj);
                return -1;
            }
            for (size_t i = 0; i < tree.count; i++) {
                const TreeEntry *e = &tree.entries[i];
                char hex[HASH_SIZE * 2 + 1];
                hash_to_hex(&e->hash, hex);
                /* 存储时目录 mode 无前导 0（"40000"），
                 * 真实 git 显示时补成 6 位（"040000"） */
                const char *mode = e->mode;
                if (e->type == TREE_ENTRY_TREE && strcmp(e->mode, "40000") == 0) {
                    mode = "040000";
                }
                printf("%s %s %s\t%s\n", mode,
                       e->type == TREE_ENTRY_TREE ? "tree" : "blob",
                       hex, e->name);
            }
            tree_free(&tree);
        } else {
            fwrite(obj.data, 1, obj.size, stdout);
        }
    } else {
        /* 默认：直接输出内容 */
        fwrite(obj.data, 1, obj.size, stdout);
    }

    object_free(&obj);
    return 0;
}

Command cmd_cat_file = {
    .name = "cat-file",
    .description = "Display the content of a Git object",
    .run = cat_file_run,
    .help = cat_file_help
};
