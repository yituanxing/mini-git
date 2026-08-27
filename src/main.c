#include "command.h"
#include <stdio.h>
#include <string.h>

/*
 * 命令注册表
 * 
 * 新增命令只需在这里添加一行
 */
static Command *commands[] = {
    &cmd_init,
    &cmd_hash_object,
    &cmd_cat_file,
    &cmd_write_tree,
    &cmd_commit_tree,
    &cmd_ls_tree,
    &cmd_log,
    &cmd_add,
    &cmd_commit,
    &cmd_status,
    &cmd_branch,
    &cmd_checkout,
    &cmd_reset,
    &cmd_tag,
    &cmd_diff,
    &cmd_merge,
    &cmd_stash,
    &cmd_reflog,
    &cmd_revert,
    &cmd_remote,
    &cmd_push,
    &cmd_fetch,
    &cmd_pull,
    &cmd_clone,
    &cmd_cherry_pick,
    &cmd_rebase,
    &cmd_gc,
    &cmd_count_objects,
    NULL
};

/* 查找命令 */
static Command *find_command(const char *name) {
    for (int i = 0; commands[i]; i++) {
        if (strcmp(commands[i]->name, name) == 0) {
            return commands[i];
        }
    }
    return NULL;
}

/* 打印使用帮助 */
static void print_usage(void) {
    printf("usage: mgit <command> [<args>]\n\n");
    printf("Available commands:\n");
    for (int i = 0; commands[i]; i++) {
        printf("    %-15s %s\n", commands[i]->name, commands[i]->description);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *cmd_name = argv[1];

    /* 特殊处理 help */
    if (strcmp(cmd_name, "help") == 0 || strcmp(cmd_name, "-h") == 0 ||
        strcmp(cmd_name, "--help") == 0) {
        if (argc >= 3) {
            Command *cmd = find_command(argv[2]);
            if (cmd && cmd->help) {
                cmd->help();
                return 0;
            }
            fprintf(stderr, "unknown command: %s\n", argv[2]);
            return 1;
        }
        print_usage();
        return 0;
    }

    /* 查找并执行命令 */
    Command *cmd = find_command(cmd_name);
    if (!cmd) {
        fprintf(stderr, "mgit: '%s' is not a mgit command.\n", cmd_name);
        fprintf(stderr, "See 'mgit help'\n");
        return 1;
    }

    /* 执行命令，传递剩余参数 */
    return cmd->run(argc - 1, argv + 1);
}
