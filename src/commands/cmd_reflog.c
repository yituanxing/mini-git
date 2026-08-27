#include "../command.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../core/ref.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * mgit reflog [show]
 * 
 * 显示 HEAD 的移动历史
 * 
 * 原理：
 * - 每次 HEAD 变化时，在 .git/logs/HEAD 追加一行
 * - 格式: <old-hash> <new-hash> <author> <timestamp> <tz> <action>: <message>
 * - 这就是 Git 的"后悔药"：即使 reset --hard 了，reflog 还能找回
 */

static void reflog_help(void) {
    printf("usage: mgit reflog [show]\n\n");
    printf("Show the history of HEAD movements.\n\n");
    printf("This is Git's safety net - even after 'reset --hard',\n");
    printf("you can recover lost commits using reflog.\n");
}

/* 读取 reflog 条目 */
typedef struct {
    char old_hex[HASH_HEX_SIZE];
    char new_hex[HASH_HEX_SIZE];
    char action[256];
    time_t timestamp;
    char tz[8];
} ReflogEntry;

static int reflog_read(ReflogEntry *entries, int max_count) {
    char log_path[256];
    snprintf(log_path, sizeof(log_path), ".git/logs/HEAD");

    FILE *fp = fopen(log_path, "r");
    if (!fp) return 0;

    int count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp) && count < max_count) {
        /* 跳过畸行：合法行至少 old(40) + 空格 + new(40) + 空格 */
        if (strlen(line) < 82) continue;

        ReflogEntry *e = &entries[count];

        /* 解析: old_hex new_hex name <email> timestamp tz\taction */
        char *p = line;

        /* old hash */
        memcpy(e->old_hex, p, 40);
        e->old_hex[40] = 0;
        p += 41;

        /* new hash */
        memcpy(e->new_hex, p, 40);
        e->new_hex[40] = 0;
        p += 41;

        /* 跳过 "name <email> " */
        char *tab = strchr(p, '\t');
        if (tab) {
            /* 提取 timestamp 和 tz（在 tab 前面） */
            /* 格式: "mgit user <mgit@example.com> 1234567890 +0800" */
            char *last_space = tab - 1;
            while (last_space > p && *last_space != ' ') last_space--;
            /* tz（在 last_space 和 tab 之间，只复制到 tab 为止） */
            size_t tzlen = (size_t)(tab - (last_space + 1));
            if (tzlen >= sizeof(e->tz)) tzlen = sizeof(e->tz) - 1;
            memcpy(e->tz, last_space + 1, tzlen);
            e->tz[tzlen] = 0;
            char *nl;

            /* timestamp */
            char *ts_start = last_space - 1;
            while (ts_start > p && *(ts_start-1) != ' ') ts_start--;
            e->timestamp = (time_t)strtoul(ts_start, NULL, 10);

            /* action (tab 后面的部分) */
            strncpy(e->action, tab + 1, sizeof(e->action) - 1);
            e->action[sizeof(e->action) - 1] = 0;
            nl = strchr(e->action, '\n');
            if (nl) *nl = 0;
            nl = strchr(e->action, '\r');
            if (nl) *nl = 0;

            count++;
        }
    }
    fclose(fp);
    return count;
}

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

static int reflog_run(int argc, char **argv) {
    (void)argc;
    (void)argv;

    ReflogEntry entries[256];
    int count = reflog_read(entries, 256);

    if (count == 0) {
        printf("(no reflog entries yet)\n");
        printf("Note: reflog records HEAD movements. Make some commits to see entries.\n");
        return 0;
    }

    /* 倒序显示（最新的在前） */
    for (int i = count - 1; i >= 0; i--) {
        ReflogEntry *e = &entries[i];
        char time_buf[64];
        format_time(e->timestamp, e->tz, time_buf, sizeof(time_buf));

        printf("%.7s -> %.7s %s %s\n",
               e->old_hex, e->new_hex, time_buf, e->action);
    }

    return 0;
}

Command cmd_reflog = {
    .name = "reflog",
    .description = "Show the history of HEAD movements",
    .run = reflog_run,
    .help = reflog_help
};
