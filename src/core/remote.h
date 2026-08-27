#ifndef MGIT_REMOTE_H
#define MGIT_REMOTE_H

#include "../base/hash.h"
#include "object.h"
#include <stddef.h>

/*
 * 远程仓库支持
 *
 * remote 配置存储在 .git/config，格式与 Git 兼容：
 *   [remote "origin"]
 *       path = C:/path/to/repo
 *
 * 对象传输：远程仓库就是一个本地目录（含 .git 子目录），
 * 对象按内容寻址，直接复制松散对象文件即可。
 */

/* ---- remote 配置（.git/config） ---- */

/* 获取 remote 路径；不存在返回 -1 */
int remote_config_get(const char *git_dir, const char *name,
                      char *path_out, size_t size);

/* 添加/更新 remote */
int remote_config_set(const char *git_dir, const char *name, const char *path);

/* 删除 remote；不存在返回 -1 */
int remote_config_remove(const char *git_dir, const char *name);

/*
 * 列出所有 remote
 * @return remote 数量
 */
int remote_config_list(const char *git_dir,
                       char names[][64], char paths[][512], int max_count);

/* ---- 对象传输 ---- */

/*
 * 复制单个对象（目标已存在则跳过）
 */
int remote_copy_object(ObjectStore *src, ObjectStore *dst, const Hash *hash);

/*
 * 将 commit 可达的所有对象（commit/tree/blob）从 src 传输到 dst
 * @return 0 成功，-1 失败
 */
int remote_send_reachable(const char *src_git_dir, const char *dst_git_dir,
                          const Hash *commit_hash);

#endif /* MGIT_REMOTE_H */
