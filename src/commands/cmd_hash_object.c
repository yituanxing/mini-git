#include "../command.h"
#include "../core/object.h"
#include "../base/file.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * mgit hash-object [-w] [-t <type>] <file>
 * 
 * 计算文件内容的哈希值
 * 
 * 选项：
 *   -w          将对象写入存储
 *   -t <type>   指定对象类型（默认 blob）
 * 
 * 这是理解 Git 内容寻址的关键命令：
 * - 相同内容 = 相同哈希
 * - 不同内容 = 不同哈希
 */

static void hash_object_help(void) {
    printf("usage: mgit hash-object [-w] [-t <type>] <file>\n\n");
    printf("Compute the hash of a file's content.\n\n");
    printf("Options:\n");
    printf("    -w          Write the object to the object store\n");
    printf("    -t <type>   Object type (default: blob)\n");
}

static int hash_object_run(int argc, char **argv) {
    int write = 0;
    const char *type_name = "blob";
    const char *file = NULL;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            write = 1;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type_name = argv[++i];
        } else if (argv[i][0] != '-') {
            file = argv[i];
        } else {
            mgit_error("unknown option: %s", argv[i]);
            return -1;
        }
    }

    if (!file) {
        mgit_error("no file specified");
        hash_object_help();
        return -1;
    }

    /* 解析对象类型 */
    ObjectType type = object_type_from_name(type_name);
    if (type == OBJ_NONE) {
        mgit_error("invalid object type: %s", type_name);
        return -1;
    }

    /* 读取文件内容 */
    uint8_t *data;
    size_t size;
    if (file_read_all(file, &data, &size) != 0) {
        mgit_error("cannot read file: %s", file);
        return -1;
    }

    /* 计算哈希 */
    Hash hash;
    object_hash(type, data, size, &hash);

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&hash, hex);

    if (write) {
        /* 打开对象存储 */
        ObjectStore *store = object_store_open(".git");
        if (!store) {
            mgit_error("not a git repository");
            free(data);
            return -1;
        }

        /* 写入对象 */
        if (object_store_write(store, type, data, size, &hash) != 0) {
            mgit_error("failed to write object");
            object_store_close(store);
            free(data);
            return -1;
        }

        object_store_close(store);
    }

    /* 输出哈希 */
    printf("%s\n", hex);
    free(data);
    return 0;
}

Command cmd_hash_object = {
    .name = "hash-object",
    .description = "Compute object hash and optionally write to store",
    .run = hash_object_run,
    .help = hash_object_help
};
