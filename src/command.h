#ifndef MGIT_COMMAND_H
#define MGIT_COMMAND_H

#include "core/object.h"
#include "core/ref.h"
#include "core/index.h"

/*
 * 命令接口定义
 * 
 * 每个命令是一个独立的模块，实现这个接口
 * 新增命令只需：
 * 1. 创建 cmd_xxx.c 实现这个接口
 * 2. 在 main.c 的命令表中注册
 */
typedef struct {
    const char *name;           /* 命令名，如 "init" */
    const char *description;    /* 简短描述 */
    int (*run)(int argc, char **argv);  /* 执行函数 */
    void (*help)(void);         /* 帮助信息 */
} Command;

/* 命令声明 */
extern Command cmd_init;
extern Command cmd_hash_object;
extern Command cmd_cat_file;
extern Command cmd_write_tree;
extern Command cmd_commit_tree;
extern Command cmd_ls_tree;
extern Command cmd_log;
extern Command cmd_add;
extern Command cmd_commit;
extern Command cmd_status;
extern Command cmd_branch;
extern Command cmd_checkout;
extern Command cmd_reset;
extern Command cmd_tag;
extern Command cmd_diff;
extern Command cmd_merge;
extern Command cmd_stash;
extern Command cmd_reflog;
extern Command cmd_revert;
extern Command cmd_remote;
extern Command cmd_push;
extern Command cmd_fetch;
extern Command cmd_pull;
extern Command cmd_clone;
extern Command cmd_cherry_pick;
extern Command cmd_rebase;
extern Command cmd_gc;
extern Command cmd_count_objects;

/*
 * cherry-pick 核心（实现在 cmd_cherry_pick.c，供 rebase 复用）
 * @return 0 成功提交；1 有冲突未提交；2 无变更未提交；-1 致命错误
 */
int cherry_pick_commit(ObjectStore *store, RefManager *refs, Index *idx,
                       const Hash *target_hash, Hash *new_commit_out);

#endif /* MGIT_COMMAND_H */
