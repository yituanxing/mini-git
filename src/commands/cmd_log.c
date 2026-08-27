#include "../command.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../core/ref.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * mgit log [--oneline] [-n <count>] [<branch>]
 * 
 * 显示提交历史
 * 
 * 支持:
 * - --oneline    简洁模式（一行一个 commit）
 * - -n <count>   限制显示数量
 * - <branch>     查看指定分支的历史
 */

static void log_help(void) {
    printf("usage: mgit log [--oneline] [-n <count>] [<branch>]\n\n");
    printf("Show the commit history.\n\n");
    printf("Options:\n");
    printf("    --oneline    Show one commit per line\n");
    printf("    -n <count>   Limit number of commits shown\n");
    printf("    <branch>     Show history for specific branch\n");
}

/* 格式化时间（应用时区偏移） */
static void format_time(time_t t, const char *tz, char *buf, size_t size) {
    int tz_offset_sec = 0;
    if (tz && strlen(tz) >= 5) {
        int sign = (tz[0] == '-') ? -1 : 1;
        int hours = (tz[1] - '0') * 10 + (tz[2] - '0');
        int mins = (tz[3] - '0') * 10 + (tz[4] - '0');
        tz_offset_sec = sign * (hours * 3600 + mins * 60);
    }
    time_t adjusted = t + tz_offset_sec;
    struct tm *tm = gmtime(&adjusted);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm);
    size_t len = strlen(buf);
    if (tz && strlen(tz) >= 5) {
        snprintf(buf + len, size - len, " %s", tz);
    }
}

/* 检查 hash 是否已在 visited 列表中 */
static int hash_in_list(const Hash *list, int count, const Hash *h) {
    for (int i = 0; i < count; i++) {
        if (hash_equal(&list[i], h)) return 1;
    }
    return 0;
}

/* 打印单个 commit */
static void print_commit(ObjectStore *store, const Hash *hash, int oneline) {
    Commit commit;
    memset(&commit, 0, sizeof(commit));
    if (commit_read(store, hash, &commit) != 0) return;

    char hex[HASH_HEX_SIZE];
    hash_to_hex(hash, hex);

    if (oneline) {
        /* 简洁模式：abcdef0 commit message */
        printf("%.7s %s\n", hex, commit.message ? commit.message : "");
    } else {
        char time_buf[64];
        format_time(commit.author.timestamp, commit.author.tz, time_buf, sizeof(time_buf));

        printf("commit %s\n", hex);
        if (commit.parent_count > 1) {
            printf("Merge:");
            for (int i = 0; i < commit.parent_count; i++) {
                char ph[HASH_HEX_SIZE];
                hash_to_hex(&commit.parents[i], ph);
                printf(" %.7s", ph);
            }
            printf("\n");
        }
        printf("Author: %s <%s>\n",
               commit.author.name ? commit.author.name : "Unknown",
               commit.author.email ? commit.author.email : "unknown");
        printf("Date:   %s\n", time_buf);
        printf("\n");
        if (commit.message) {
            printf("    %s\n", commit.message);
        }
        printf("\n");
    }

    commit_free(&commit);
}

static int log_run(int argc, char **argv) {
    int oneline = 0;
    int max_count = 0;  /* 0 = 无限制 */
    const char *branch = NULL;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--oneline") == 0) {
            oneline = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_count = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            branch = argv[i];
        }
    }

    /* 打开对象存储和引用管理器 */
    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("not a git repository");
        return -1;
    }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        object_store_close(store);
        mgit_error("cannot open ref manager");
        return -1;
    }

    /* 确定起始 commit */
    Hash current;
    if (branch) {
        /* 指定引用：通用解析（分支/tag/HEAD/packed-refs），失败再当哈希解析 */
        if (ref_resolve_quiet(refs, branch, &current) != 0) {
            if (hex_to_hash(branch, &current) != 0) {
                mgit_error("unknown branch or commit: %s", branch);
                ref_manager_close(refs);
                object_store_close(store);
                return -1;
            }
        }
    } else {
        if (ref_resolve_head(refs, &current) != 0) {
            mgit_error("no commits yet");
            ref_manager_close(refs);
            object_store_close(store);
            return -1;
        }
    }

    /*
     * BFS 遍历所有可达的 commit（包括 merge 的第二父）
     * visited 去重，queue 控制遍历顺序
     */
    int qcap = 256, vcap = 256;
    Hash *queue = (Hash *)malloc(sizeof(Hash) * qcap);
    Hash *visited = (Hash *)malloc(sizeof(Hash) * vcap);
    if (!queue || !visited) {
        free(queue);
        free(visited);
        ref_manager_close(refs);
        object_store_close(store);
        mgit_error("out of memory");
        return -1;
    }

    int q_head = 0, q_tail = 0;
    int visited_count = 0;
    int shown = 0;

    queue[q_tail++] = current;

    while (q_head < q_tail) {
        if (max_count > 0 && shown >= max_count) break;

        Hash cur = queue[q_head++];

        /* 去重：同一个 commit 可能被多个父引用 */
        if (hash_in_list(visited, visited_count, &cur)) continue;
        if (hash_is_zero(&cur)) continue;

        /* visited 独立扩容（与 queue 分开，避免互相截断） */
        if (visited_count >= vcap) {
            vcap *= 2;
            Hash *nv = (Hash *)realloc(visited, sizeof(Hash) * vcap);
            if (!nv) break;
            visited = nv;
        }
        visited[visited_count++] = cur;

        /* 读取并打印（同时需要父 commit 信息，所以直接读一次） */
        Commit commit;
        memset(&commit, 0, sizeof(commit));
        if (commit_read(store, &cur, &commit) != 0) continue;

        print_commit(store, &cur, oneline);
        shown++;

        /* 所有父 commit 入队（queue 独立扩容） */
        for (int i = 0; i < commit.parent_count; i++) {
            if (hash_in_list(visited, visited_count, &commit.parents[i])) continue;
            if (q_tail >= qcap) {
                qcap *= 2;
                Hash *nq = (Hash *)realloc(queue, sizeof(Hash) * qcap);
                if (!nq) break;
                queue = nq;
            }
            queue[q_tail++] = commit.parents[i];
        }

        commit_free(&commit);
    }

    free(queue);
    free(visited);

    ref_manager_close(refs);
    object_store_close(store);
    return 0;
}

Command cmd_log = {
    .name = "log",
    .description = "Show the commit history",
    .run = log_run,
    .help = log_help
};
