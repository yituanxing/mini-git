#ifndef MGIT_COMMIT_H
#define MGIT_COMMIT_H

#include "../base/hash.h"
#include "object.h"
#include "tree.h"
#include <time.h>

/*
 * Commit 对象
 * 
 * 文本格式：
 * tree <40-char-sha1>
 * parent <40-char-sha1>    (可选，可以有多个)
 * author <name> <email> <timestamp> <timezone>
 * committer <name> <email> <timestamp> <timezone>
 * 
 * <message>
 */

/* 作者/提交者信息 */
typedef struct {
    char *name;
    char *email;
    time_t timestamp;
    char tz[8];         /* 如 "+0800" */
} Signature;

/* Commit 对象 */
typedef struct {
    Hash tree;                      /* 指向根 tree */
    Hash *parents;                  /* 父 commit 动态数组 */
    int parent_count;               /* 父 commit 数量 */
    Signature author;
    Signature committer;
    char *message;                  /* 提交信息 */
} Commit;

/* 创建/释放 Commit */
Commit *commit_new(void);
void commit_free(Commit *commit);

/*
 * 从对象数据解析 Commit
 */
int commit_parse(const uint8_t *data, size_t size, Commit *commit);

/*
 * 将 Commit 序列化为对象数据
 */
int commit_serialize(const Commit *commit, uint8_t **data, size_t *size);

/*
 * 创建新的 Commit（单亲）
 */
int commit_create(ObjectStore *store, const Hash *tree, const Hash *parent,
                  const char *message, Hash *out);

/*
 * 创建合并 Commit（双亲）
 * @param store       对象存储
 * @param tree        根 tree 的哈希
 * @param parent1     第一个父 commit（HEAD）
 * @param parent2     第二个父 commit（被合并的分支）
 * @param message     提交信息
 * @param out         输出：commit 对象的哈希
 */
int commit_create_merge(ObjectStore *store, const Hash *tree,
                        const Hash *parent1, const Hash *parent2,
                        const char *message, Hash *out);

/*
 * 读取并解析 Commit 对象
 */
int commit_read(ObjectStore *store, const Hash *hash, Commit *commit);

/*
 * 读取 commit 的根 tree（读 commit → 读其 tree → parse）
 * 成功返回 0，tree_out 需调用者 tree_free
 */
int commit_read_tree(ObjectStore *store, const Hash *commit_hash, Tree *tree_out);

#endif /* MGIT_COMMIT_H */
