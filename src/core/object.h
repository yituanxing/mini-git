#ifndef MGIT_OBJECT_H
#define MGIT_OBJECT_H

#include "../base/hash.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Git 对象类型
 * 
 * Git 有四种对象类型：
 * - blob: 文件内容
 * - tree: 目录结构（类似文件系统的目录）
 * - commit: 提交信息
 * - tag: 标签
 */
typedef enum {
    OBJ_NONE = 0,
    OBJ_BLOB = 1,
    OBJ_TREE = 2,
    OBJ_COMMIT = 3,
    OBJ_TAG = 4
} ObjectType;

/* 对象类型名称 */
const char *object_type_name(ObjectType type);
ObjectType object_type_from_name(const char *name);

/*
 * Git 对象
 * 
 * 磁盘格式: <type> <size>\0<content>
 * 然后用 zlib 压缩存储
 */
typedef struct {
    ObjectType type;
    size_t size;
    uint8_t *data;      /* 原始内容（不含头部） */
    Hash hash;          /* 对象的 SHA-1 */
} Object;

/*
 * 对象存储上下文
 */
typedef struct {
    char *objects_dir;  /* .git/objects 目录路径 */
} ObjectStore;

/* 打开/关闭对象存储 */
ObjectStore *object_store_open(const char *git_dir);
void object_store_close(ObjectStore *store);

/*
 * 计算对象的哈希值（不写入）
 * 
 * 哈希计算基于: <type> <size>\0<content>
 * 
 * @param type  对象类型
 * @param data  对象内容
 * @param size  内容大小
 * @param out   输出：哈希值
 */
void object_hash(ObjectType type, const void *data, size_t size, Hash *out);

/*
 * 写入对象到存储
 * 
 * @param store 对象存储
 * @param type  对象类型
 * @param data  对象内容
 * @param size  内容大小
 * @param out   输出：对象的哈希值
 * @return      0 成功，-1 失败
 */
int object_store_write(ObjectStore *store, ObjectType type,
                       const void *data, size_t size, Hash *out);

/*
 * 读取对象
 * 
 * @param store 对象存储
 * @param hash  对象哈希
 * @param obj   输出：对象（调用者需用 object_free 释放）
 * @return      0 成功，-1 失败（对象不存在等）
 */
int object_store_read(ObjectStore *store, const Hash *hash, Object *obj);

/*
 * 检查对象是否存在
 */
int object_exists(ObjectStore *store, const Hash *hash);

/*
 * 按十六进制前缀查找对象（扫描 loose + pack，能找到不可达对象）
 * @param prefix  十六进制前缀（4-40 字符，大小写均可）
 * @param type    限定对象类型；OBJ_NONE 表示不限
 * @return        0 唯一命中，-1 未找到/太短，-2 前缀歧义
 */
int object_find_by_prefix(ObjectStore *store, const char *prefix,
                          ObjectType type, Hash *out);

/* 释放对象资源 */
void object_free(Object *obj);

/*
 * 获取对象的文件路径
 * @param store     对象存储
 * @param hash      对象哈希
 * @param buf       输出缓冲区
 * @param size      缓冲区大小
 */
void object_path(ObjectStore *store, const Hash *hash, char *buf, size_t size);

#endif /* MGIT_OBJECT_H */
