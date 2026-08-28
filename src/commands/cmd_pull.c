#include "../command.h"
#include "../core/ref.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

/*
 * mgit pull [<remote>] [<branch>]
 *
 * Git 的概念是 pull = fetch + integrate。
 * 真实 Git 的 integrate 可以选择 merge / rebase / fast-forward-only 等策略；
 * mgit 为教学简化，只选择最直观的 merge 路线：
 *
 * 1. fetch：从远程下载对象，更新 refs/remotes/<remote>/<branch> 跟踪分支
 * 2. merge：把该远程跟踪分支合并进当前分支
 *
 * 默认 remote 为 origin，默认 branch 为当前分支。
 */

static void pull_help(void) {
    printf("usage: mgit pull [<remote>] [<branch>]\n\n");
    printf("Fetch from a remote and merge into the current branch.\n");
    printf("Defaults: remote 'origin', branch = current branch.\n");
}

static int pull_run(int argc, char **argv) {
    const char *remote_name = "origin";
    const char *branch = NULL;

    /* 解析位置参数：第一个是 remote，第二个是 branch */
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (pos == 0) remote_name = argv[i];
        else if (pos == 1) branch = argv[i];
        pos++;
    }

    if (!file_is_dir(".git")) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 未指定 branch 时取当前分支名 */
    char branch_buf[256];
    if (!branch) {
        RefManager *refs = ref_manager_open(".git");
        if (!refs) return -1;

        char head_ref[256];
        if (ref_get_head_branch(refs, head_ref, sizeof(head_ref)) != 0 ||
            strcmp(head_ref, "HEAD") == 0) {
            ref_manager_close(refs);
            mgit_error("pull needs a current branch (detached HEAD is not supported)");
            return -1;
        }
        ref_manager_close(refs);

        /* head_ref 形如 refs/heads/xxx，取短名 */
        if (strncmp(head_ref, "refs/heads/", 11) == 0) {
            snprintf(branch_buf, sizeof(branch_buf), "%s", head_ref + 11);
        } else {
            snprintf(branch_buf, sizeof(branch_buf), "%s", head_ref);
        }
        branch = branch_buf;
    }

    /* 第 1 步：fetch（复用 fetch 命令，更新所有跟踪分支） */
    char *fargv[] = { "fetch", (char *)remote_name };
    if (cmd_fetch.run(2, fargv) != 0) {
        return -1;
    }

    /* 第 2 步：确认跟踪分支存在 */
    char tracking_ref[512];
    snprintf(tracking_ref, sizeof(tracking_ref),
             "refs/remotes/%s/%s", remote_name, branch);

    RefManager *refs = ref_manager_open(".git");
    if (!refs) return -1;

    Hash tracking_hash;
    if (ref_resolve_quiet(refs, tracking_ref, &tracking_hash) != 0) {
        ref_manager_close(refs);
        mgit_error("no tracking branch %s (does it exist on the remote?)",
                   tracking_ref);
        return -1;
    }
    ref_manager_close(refs);

    /* 第 3 步：merge（复用 merge 命令，支持完整引用路径） */
    char *margv[] = { "merge", tracking_ref };
    return cmd_merge.run(2, margv);
}

Command cmd_pull = {
    .name = "pull",
    .description = "Fetch from a remote and merge into current branch",
    .run = pull_run,
    .help = pull_help
};
