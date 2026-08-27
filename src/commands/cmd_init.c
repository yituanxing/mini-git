#include "../command.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit init [directory]
 * 
 * 创建 .git 目录结构：
 * .git/
 * ├── objects/        对象存储
 * │   ├── info/
 * │   └── pack/
 * ├── refs/           引用（分支、标签）
 * │   ├── heads/      分支
 * │   └── tags/       标签
 * ├── HEAD            当前分支指针
 * └── ...
 */

static void init_help(void) {
    printf("usage: mgit init [directory]\n\n");
    printf("Create an empty Git repository.\n\n");
    printf("If no directory is given, the current directory is used.\n");
}

static int init_run(int argc, char **argv) {
    const char *target = ".";
    
    if (argc >= 2) {
        target = argv[1];
    }

    char git_dir[1024];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", target);

    /* 检查是否已存在 */
    if (file_is_dir(git_dir)) {
        mgit_error("repository already exists: %s", git_dir);
        return -1;
    }

    printf("Initialized empty Git repository in %s\n", git_dir);

    /* 创建目录结构 */
    char path[1024];

    /* .git/objects */
    path_join(path, sizeof(path), git_dir, "objects");
    if (file_mkdir_p(path) != 0) {
        mgit_error("failed to create %s", path);
        return -1;
    }

    /* .git/objects/info */
    path_join(path, sizeof(path), git_dir, "objects/info");
    file_mkdir_p(path);

    /* .git/objects/pack */
    path_join(path, sizeof(path), git_dir, "objects/pack");
    file_mkdir_p(path);

    /* .git/refs */
    path_join(path, sizeof(path), git_dir, "refs");
    if (file_mkdir_p(path) != 0) {
        mgit_error("failed to create %s", path);
        return -1;
    }

    /* .git/refs/heads */
    path_join(path, sizeof(path), git_dir, "refs/heads");
    file_mkdir_p(path);

    /* .git/refs/tags */
    path_join(path, sizeof(path), git_dir, "refs/tags");
    file_mkdir_p(path);

    /* .git/HEAD - 指向默认分支 */
    path_join(path, sizeof(path), git_dir, "HEAD");
    if (file_write_line(path, "ref: refs/heads/master") != 0) {
        mgit_error("failed to create HEAD");
        return -1;
    }

    return 0;
}

Command cmd_init = {
    .name = "init",
    .description = "Create an empty Git repository",
    .run = init_run,
    .help = init_help
};
