#include "object.h"
#include "pack.h"
#include "../base/hash.h"
#include "../base/zlib_util.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* 对象类型名称映射 */
static const char *type_names[] = {
    [OBJ_NONE]   = "none",
    [OBJ_BLOB]   = "blob",
    [OBJ_TREE]   = "tree",
    [OBJ_COMMIT] = "commit",
    [OBJ_TAG]    = "tag"
};

const char *object_type_name(ObjectType type) {
    if (type < OBJ_BLOB || type > OBJ_TAG) return "unknown";
    return type_names[type];
}

ObjectType object_type_from_name(const char *name) {
    if (strcmp(name, "blob") == 0) return OBJ_BLOB;
    if (strcmp(name, "tree") == 0) return OBJ_TREE;
    if (strcmp(name, "commit") == 0) return OBJ_COMMIT;
    if (strcmp(name, "tag") == 0) return OBJ_TAG;
    return OBJ_NONE;
}

ObjectStore *object_store_open(const char *git_dir) {
    ObjectStore *store = (ObjectStore *)malloc(sizeof(ObjectStore));
    if (!store) return NULL;

    store->objects_dir = (char *)malloc(strlen(git_dir) + 10);
    if (!store->objects_dir) {
        free(store);
        return NULL;
    }

    path_join(store->objects_dir, strlen(git_dir) + 10, git_dir, "objects");
    
    /* 在 Windows 上，将 / 转换为 \ */
#ifdef _WIN32
    for (char *c = store->objects_dir; *c; c++) {
        if (*c == '/') *c = '\\';
    }
#endif
    
    return store;
}

void object_store_close(ObjectStore *store) {
    if (store) {
        free(store->objects_dir);
        free(store);
    }
}

/*
 * 计算对象的哈希值
 * 
 * Git 对象的哈希基于: "<type> <size>\0<content>"
 * 例如: "blob 11\0hello world"
 */
void object_hash(ObjectType type, const void *data, size_t size, Hash *out) {
    SHA1_CTX ctx;
    char header[32];
    int header_len;

    /* 构建头部: "<type> <size>\0" */
    header_len = snprintf(header, sizeof(header), "%s %lu",
                          object_type_name(type), (unsigned long)size);
    header_len++;  /* 包含 NUL 字节 */

    /* 计算哈希: header + content */
    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t *)header, header_len);
    sha1_update(&ctx, (const uint8_t *)data, size);
    sha1_final(&ctx, out);
}

static int object_hash_matches(const Object *obj, const Hash *expected) {
    Hash actual;
    object_hash(obj->type, obj->data, obj->size, &actual);
    return hash_equal(&actual, expected);
}


/*
 * 获取对象的存储路径
 * 
 * Git 将对象存储在 .git/objects/ab/cdef...
 * 前2位做目录名，剩余做文件名
 */
void object_path(ObjectStore *store, const Hash *hash, char *buf, size_t size) {
    char hex[HASH_HEX_SIZE];
    hash_to_hex(hash, hex);
    
    /* 构建路径: objects/ab/cdef... */
    char subdir[3] = { hex[0], hex[1], 0 };
    char path1[1024];
    path_join(path1, sizeof(path1), store->objects_dir, subdir);
    path_join(buf, size, path1, hex + 2);
}

/*
 * 写入对象
 * 
 * 1. 计算哈希
 * 2. 构建完整数据: "<type> <size>\0<content>"
 * 3. zlib 压缩
 * 4. 写入 .git/objects/ab/cdef...
 */
int object_store_write(ObjectStore *store, ObjectType type,
                       const void *data, size_t size, Hash *out) {
    /* 计算哈希 */
    object_hash(type, data, size, out);

    /* 检查对象是否已存在 */
    char path[1024];
    object_path(store, out, path, sizeof(path));
    if (file_exists(path)) {
        return 0;  /* 对象已存在，无需重复写入 */
    }

    /* 构建完整数据 */
    char header[32];
    int header_len = snprintf(header, sizeof(header), "%s %lu",
                              object_type_name(type), (unsigned long)size);
    header_len++;  /* 包含 NUL */

    size_t full_size = header_len + size;
    uint8_t *full_data = (uint8_t *)malloc(full_size);
    if (!full_data) return -1;

    memcpy(full_data, header, header_len);
    memcpy(full_data + header_len, data, size);

    /* zlib 压缩 */
    size_t comp_size = full_size + full_size / 100 + 20;
    uint8_t *comp_data = (uint8_t *)malloc(comp_size);
    if (!comp_data) {
        free(full_data);
        return -1;
    }

    if (zlib_compress(full_data, full_size, comp_data, &comp_size) != 0) {
        free(full_data);
        free(comp_data);
        return -1;
    }
    free(full_data);

    /* 创建目录 */
    char hex[HASH_HEX_SIZE];
    hash_to_hex(out, hex);

    char dir2[1024];
#ifdef _WIN32
    snprintf(dir2, sizeof(dir2), "%s\\%c%c", store->objects_dir, hex[0], hex[1]);
#else
    snprintf(dir2, sizeof(dir2), "%s/%c%c", store->objects_dir, hex[0], hex[1]);
#endif
    if (file_mkdir_p(dir2) != 0) {
        free(comp_data);
        return -1;
    }

    /* 写入文件 */
    int ret = file_write_all(path, comp_data, comp_size);
    free(comp_data);
    return ret;
}

/*
 * 读取对象
 * 
 * 1. 根据哈希获取文件路径
 * 2. 读取文件
 * 3. zlib 解压
 * 4. 解析头部: "<type> <size>\0"
 * 5. 提取类型、大小和内容
 */
int object_store_read(ObjectStore *store, const Hash *hash, Object *obj) {
    char path[1024];
    object_path(store, hash, path, sizeof(path));

    /* 松散对象不存在时，回退到 packfile 查找 */
    if (!file_exists(path)) {
        if (pack_read_object(store, hash, obj) == 0) {
            if (!object_hash_matches(obj, hash)) {
                char hex[HASH_HEX_SIZE];
                hash_to_hex(hash, hex);
                object_free(obj);
                mgit_error("object hash mismatch: %s", hex);
                return -1;
            }
            obj->hash = *hash;
            return 0;
        }
        char hex[HASH_HEX_SIZE];
        hash_to_hex(hash, hex);
        mgit_error("object not found: %s", hex);
        return -1;
    }

    /* 读取压缩数据 */
    uint8_t *comp_data;
    size_t comp_size;
    if (file_read_all(path, &comp_data, &comp_size) != 0) {
        mgit_error("object not found: %s", path);
        return -1;
    }

    /* 解压 */
    uint8_t *full_data;
    size_t full_size;
    if (zlib_decompress_alloc(comp_data, comp_size, &full_data, &full_size) != 0) {
        free(comp_data);
        mgit_error("failed to decompress object");
        return -1;
    }
    free(comp_data);

    /* 解析头部: "<type> <size>\0<content>" */
    char *null_pos = (char *)memchr(full_data, 0, full_size);
    if (!null_pos) {
        free(full_data);
        mgit_error("invalid object format");
        return -1;
    }

    char *header = (char *)full_data;
    size_t header_len = null_pos - (char *)full_data + 1;

    /* 解析类型 */
    char *space = strchr(header, ' ');
    if (!space) {
        free(full_data);
        mgit_error("invalid object header");
        return -1;
    }
    *space = 0;
    obj->type = object_type_from_name(header);
    if (obj->type == OBJ_NONE) {
        free(full_data);
        mgit_error("unknown object type: %s", header);
        return -1;
    }

    /* 解析大小 */
    obj->size = (size_t)atol(space + 1);

    /* 提取内容 */
    size_t content_size = full_size - header_len;
    if (content_size != obj->size) {
        free(full_data);
        mgit_error("object size mismatch");
        return -1;
    }

    obj->data = (uint8_t *)malloc(content_size + 1);
    if (!obj->data) {
        free(full_data);
        return -1;
    }
    memcpy(obj->data, full_data + header_len, content_size);
    obj->data[content_size] = 0;  /* 方便当文本处理 */

    /* Git 是内容寻址存储：路径里的 OID 必须等于对象内容重新计算出的 OID。 */
    if (!object_hash_matches(obj, hash)) {
        char hex[HASH_HEX_SIZE];
        hash_to_hex(hash, hex);
        free(full_data);
        object_free(obj);
        mgit_error("object hash mismatch: %s", hex);
        return -1;
    }
    obj->hash = *hash;

    free(full_data);
    return 0;
}

int object_exists(ObjectStore *store, const Hash *hash) {
    char path[1024];
    object_path(store, hash, path, sizeof(path));
    if (file_exists(path)) return 1;
    return pack_contains(store, hash);
}

void object_free(Object *obj) {
    if (obj) {
        free(obj->data);
        obj->data = NULL;
    }
}

/*
 * 按十六进制前缀查找对象
 *
 * 直接扫描 .git/objects 目录结构（xx/yyyyyy...），
 * 能找到任何存在的对象——包括 reset 后"不可达"的对象，
 * 这是 reflog 找回丢失 commit 的关键能力。
 *
 * @param prefix  十六进制前缀（1-40 字符）
 * @param type    限定对象类型；OBJ_NONE 表示不限
 * @return        0 找到，-1 未找到
 */
int object_find_by_prefix(ObjectStore *store, const char *prefix,
                          ObjectType type, Hash *out) {
    size_t plen = strlen(prefix);
    if (plen < 1 || plen > 40) return -1;

    DIR *root = opendir(store->objects_dir);
    if (!root) return -1;

    int found = -1;
    struct dirent *d;
    while ((d = readdir(root)) != NULL && found != 0) {
        const char *name = d->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strlen(name) != 2) continue;  /* 对象目录是 2 位十六进制 */

        /* 前缀的第一部分必须匹配目录名 */
        if (plen >= 2) {
            if (strncmp(name, prefix, 2) != 0) continue;
        } else {
            if (name[0] != prefix[0]) continue;
        }

        char subdir[1024];
        snprintf(subdir, sizeof(subdir), "%s/%s", store->objects_dir, name);
        DIR *sub = opendir(subdir);
        if (!sub) continue;

        struct dirent *f;
        while ((f = readdir(sub)) != NULL) {
            const char *fname = f->d_name;
            if (strlen(fname) != 38) continue;  /* 对象文件名是 38 位 */

            char hex[HASH_HEX_SIZE];
            snprintf(hex, sizeof(hex), "%s%s", name, fname);
            if (strncmp(hex, prefix, plen) != 0) continue;

            Hash cand;
            if (hex_to_hash(hex, &cand) != 0) continue;

            /* 类型校验（需要读对象） */
            if (type != OBJ_NONE) {
                Object obj;
                memset(&obj, 0, sizeof(obj));
                if (object_store_read(store, &cand, &obj) != 0) continue;
                int ok = (obj.type == type);
                object_free(&obj);
                if (!ok) continue;
            }

            *out = cand;
            found = 0;
            break;
        }
        closedir(sub);
    }
    closedir(root);
    if (found == 0) return 0;

    /* 松散对象未命中：回退查 packfile（gc 后对象已打包） */
    Hash cand;
    if (pack_find_by_prefix(store, prefix, plen, &cand) != 0) return -1;

    /* 类型校验（object_store_read 已支持从 pack 读取） */
    if (type != OBJ_NONE) {
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &cand, &obj) != 0) return -1;
        int ok = (obj.type == type);
        object_free(&obj);
        if (!ok) return -1;
    }

    *out = cand;
    return 0;
}
