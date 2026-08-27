#include "../command.h"
#include "../core/remote.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit remote
 * mgit remote -v
 * mgit remote add <name> <path>
 * mgit remote remove <name>
 *
 * 管理远程仓库配置（存储在 .git/config）
 * 远程仓库用本地目录模拟（目录中包含 .git）
 */

static void remote_help(void) {
    printf("usage: mgit remote [-v]\n");
    printf("       mgit remote add <name> <path>\n");
    printf("       mgit remote remove <name>\n\n");
    printf("Manage set of tracked repositories.\n");
}

static int remote_run(int argc, char **argv) {
    const char *git_dir = ".git";

    /* mgit remote [-v] */
    if (argc < 2 || strcmp(argv[1], "-v") == 0) {
        int verbose = (argc >= 2 && strcmp(argv[1], "-v") == 0);
        char names[32][64];
        char paths[32][512];
        int count = remote_config_list(git_dir, names, paths, 32);
        for (int i = 0; i < count; i++) {
            if (verbose) {
                printf("%s\t%s\n", names[i], paths[i]);
            } else {
                printf("%s\n", names[i]);
            }
        }
        return 0;
    }

    /* mgit remote add <name> <path> */
    if (strcmp(argv[1], "add") == 0) {
        if (argc < 4) {
            mgit_error("usage: mgit remote add <name> <path>");
            return -1;
        }
        char existing[512];
        if (remote_config_get(git_dir, argv[2], existing, sizeof(existing)) == 0) {
            mgit_error("remote %s already exists", argv[2]);
            return -1;
        }
        if (remote_config_set(git_dir, argv[2], argv[3]) != 0) {
            mgit_error("failed to write config");
            return -1;
        }
        return 0;
    }

    /* mgit remote remove <name> */
    if (strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "rm") == 0) {
        if (argc < 3) {
            mgit_error("usage: mgit remote remove <name>");
            return -1;
        }
        if (remote_config_remove(git_dir, argv[2]) != 0) {
            mgit_error("remote '%s' not found", argv[2]);
            return -1;
        }
        return 0;
    }

    mgit_error("unknown remote subcommand: %s", argv[1]);
    remote_help();
    return -1;
}

Command cmd_remote = {
    .name = "remote",
    .description = "Manage remote repositories",
    .run = remote_run,
    .help = remote_help
};
