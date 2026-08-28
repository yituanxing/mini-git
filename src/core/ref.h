#ifndef MGIT_REF_H
#define MGIT_REF_H

#include "../base/hash.h"

/*
 * 引用管理
 * 
 * Git 的引用系统：
 * - HEAD: 通常是指向当前分支的符号引用；detached 时直接存 commit hash
 * - refs/heads/xxx: 分支，存储 commit hash
 * - refs/tags/xxx: 标签，存储 commit hash
 * 
 * 引用的本质就是一个指向 commit 的指针
 */

typedef struct {
    char *git_dir;      /* .git 目录路径 */
} RefManager;

/* 打开/关闭引用管理器 */
RefManager *ref_manager_open(const char *git_dir);
void ref_manager_close(RefManager *mgr);

/*
 * 解析 HEAD，获取当前分支指向的 commit hash
 * 
 * HEAD 可能是：
 * 1. 符号引用: "ref: refs/heads/master" -> 解析分支文件
 * 2. 直接引用: 直接是 commit hash (detached HEAD)
 */
int ref_resolve_head(RefManager *mgr, Hash *out);

/*
 * 获取 HEAD 指向的分支名
 * 例如: "refs/heads/master" 或 "master"
 */
int ref_get_head_branch(RefManager *mgr, char *buf, int size);

/*
 * 解析任意引用
 * @param name  引用名，如 "HEAD", "master", "refs/heads/master"
 * @param out   输出：解析后的 commit hash
 */
int ref_resolve(RefManager *mgr, const char *name, Hash *out);

/*
 * 静默版解析：失败时不打印错误
 * 用于探测性调用（检查分支/引用是否存在）
 */
int ref_resolve_quiet(RefManager *mgr, const char *name, Hash *out);

/* 静默版解析 HEAD */
int ref_resolve_head_quiet(RefManager *mgr, Hash *out);

/*
 * 更新引用
 * @param name  引用名，如 "refs/heads/master"
 * @param hash  新的 commit hash
 */
int ref_update(RefManager *mgr, const char *name, const Hash *hash);

/*
 * 创建分支
 * @param name  分支名
 * @param hash  分支指向的 commit
 */
int ref_create_branch(RefManager *mgr, const char *name, const Hash *hash);

/*
 * 删除分支
 */
int ref_delete_branch(RefManager *mgr, const char *name);

/*
 * 设置 HEAD 指向的分支
 * @param name  分支名（不含 refs/heads/ 前缀）
 */
int ref_set_head(RefManager *mgr, const char *name);

/* 设置 detached HEAD：HEAD 文件直接保存 commit hash。 */
int ref_set_head_detached(RefManager *mgr, const Hash *hash);

/*
 * 列出所有分支
 * @param branches  输出数组，存储分支引用路径
 * @param max_count 最大分支数
 * @return 分支数量
 */
int ref_list_branches(RefManager *mgr, char branches[][256], int max_count);

/*
 * 记录 HEAD 移动日志（追加到 .git/logs/HEAD）
 * 供各命令在 HEAD 变更后调用
 */
int reflog_append(const char *old_hex, const char *new_hex, const char *action);

#endif /* MGIT_REF_H */
