#ifndef MGIT_INDEX_H
#define MGIT_INDEX_H

#include "../base/hash.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Index（暂存区）管理
 * 
 * Git Index 文件格式（二进制）：
 * 
 * 头部（12字节）：
 *   - 4字节签名: "DIRC"
 *   - 4字节版本: 2
 *   - 4字节条目数
 * 
 * 每个条目：
 *   - 32字节 stat 信息 (ctime, mtime, dev, ino, mode, uid, gid, size)
 *   - 20字节 blob hash
 *   - 2字节 flags (包含 name 长度)
 *   - 变长 name (NUL 结尾，填充到 8 字节对齐)
 */

/* Index 条目 */
typedef struct {
    uint32_t ctime_sec;
    uint32_t mtime_sec;
    uint32_t dev;
    uint32_t ino;
    uint32_t mode;      /* 文件模式，如 0100644 */
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    Hash hash;          /* blob 对象的哈希 */
    char *name;         /* 文件路径 */
} IndexEntry;

/* Index 对象 */
typedef struct {
    char *path;             /* .git/index 文件路径 */
    IndexEntry *entries;    /* 条目数组 */
    size_t count;           /* 条目数量 */
    size_t capacity;        /* 容量 */
    int dirty;              /* 是否有未保存的修改 */
} Index;

/* 打开/关闭 Index */
Index *index_open(const char *git_dir);
void index_close(Index *idx);

/* 读取 Index 文件 */
int index_read(Index *idx);

/* 保存 Index 文件 */
int index_write(Index *idx);

/*
 * 添加文件到 Index
 * @param idx       Index 对象
 * @param path      文件路径（相对于仓库根目录）
 * @param hash      blob 对象的哈希
 * @param mode      文件模式
 */
int index_add(Index *idx, const char *path, const Hash *hash, uint32_t mode);

/*
 * 从 Index 移除文件
 */
int index_remove(Index *idx, const char *path);

/*
 * 查找 Index 中的条目
 * @return 条目指针，未找到返回 NULL
 */
IndexEntry *index_find(Index *idx, const char *path);

/*
 * 将 Index 内容写入 tree 对象
 * @return tree 对象的哈希
 */
int index_write_tree(Index *idx, void *store, Hash *out);

#endif /* MGIT_INDEX_H */
