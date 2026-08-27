#ifndef MGIT_TREE_H
#define MGIT_TREE_H

#include "../base/hash.h"
#include "object.h"
#include "index.h"
#include <stddef.h>

/*
 * Tree 对象
 * 
 * Tree 表示目录结构，类似文件系统的目录。
 * 
 * 磁盘格式（二进制）：
 * 每个条目: "<mode> <name>\0<20-byte-sha1>"
 * 
 * 例如：
 * "100644 README.md\0<20-byte-hash>"
 * "040000 src\0<20-byte-hash>"
 * 
 * mode:
 * - 100644: 普通文件
 * - 100755: 可执行文件
 * - 040000: 目录（指向另一个 tree）
 */

/* Tree 条目类型 */
typedef enum {
    TREE_ENTRY_BLOB = 0,
    TREE_ENTRY_TREE = 1
} TreeEntryType;

/* Tree 条目 */
typedef struct {
    char mode[8];           /* 文件模式，如 "100644" */
    char *name;             /* 文件名 */
    Hash hash;              /* 指向 blob 或 tree */
    TreeEntryType type;     /* 条目类型 */
} TreeEntry;

/* Tree 对象 */
typedef struct {
    TreeEntry *entries;     /* 条目数组 */
    size_t count;           /* 条目数量 */
    size_t capacity;        /* 容量 */
} Tree;

/* 创建/释放 Tree */
Tree *tree_new(void);
void tree_free(Tree *tree);

/*
 * 从对象数据解析 Tree
 * @param data  对象的原始数据
 * @param size  数据大小
 * @param tree  输出：解析后的 Tree
 */
int tree_parse(const uint8_t *data, size_t size, Tree *tree);

/*
 * 将 Tree 序列化为对象数据
 * @param tree  Tree 对象
 * @param data  输出：序列化数据（调用者释放）
 * @param size  输出：数据大小
 */
int tree_serialize(const Tree *tree, uint8_t **data, size_t *size);

/*
 * 添加条目到 Tree
 * @param tree  Tree 对象
 * @param mode  文件模式
 * @param name  文件名
 * @param hash  指向的对象哈希
 */
int tree_add_entry(Tree *tree, const char *mode, const char *name, const Hash *hash);

/*
 * 将 Tree 写入对象存储，返回 Tree 对象的哈希
 */
int tree_write(ObjectStore *store, Tree *tree, Hash *out);

/*
 * 用 tree 内容同步工作区和 Index（checkout/stash/merge 共用）
 * 
 * - 删除 Index 中有但 tree 中没有的文件（含工作区文件）
 * - 写出 tree 中所有文件，重建 Index
 * - 不会保存 Index，调用者需自行 index_write
 */
int tree_restore_worktree(ObjectStore *store, Index *idx, const Tree *tree);

/*
 * 仅用 tree 重建 Index（不碰工作区文件）
 * 用于 stash pop 后让恢复的修改显示为“未暂存”
 */
int tree_rebuild_index(ObjectStore *store, Index *idx, const Tree *tree);

/* 扁平化后的文件条目（完整路径 + blob 哈希） */
typedef struct {
    char path[1024];
    Hash hash;
} TreeFlatEntry;

/*
 * 递归展开（嵌套）tree 为扁平文件列表
 * 
 * @param out    输出：malloc 的数组，调用者 free
 * @param count  输出：条目数
 * @return       0 成功，-1 失败
 */
int tree_flatten(ObjectStore *store, const Tree *tree, TreeFlatEntry **out, size_t *count);

/* 在扁平列表中查找路径 */
TreeFlatEntry *tree_flat_find(TreeFlatEntry *entries, size_t count, const char *path);

#endif /* MGIT_TREE_H */
