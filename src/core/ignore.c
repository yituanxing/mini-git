#include "ignore.h"
#include "../base/file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 通配符匹配（* ? 不跨 /） ---- */

static int wildcard_match(const char *pat, const char *str) {
    if (*pat == 0) return *str == 0;

    if (*pat == '*') {
        /* * 不跨越 / */
        if (pat[1] == 0) {
            /* 末尾 *：只要 str 剩余部分不含 / 即匹配 */
            return strchr(str, '/') == NULL;
        }
        for (const char *s = str; ; s++) {
            if (wildcard_match(pat + 1, s)) return 1;
            if (*s == 0 || *s == '/') return 0;
        }
    }

    if (*str == 0) return 0;
    if (*pat == '?' || *pat == *str) {
        return wildcard_match(pat + 1, str + 1);
    }
    return 0;
}

/* ---- 加载 .gitignore ---- */

int ignore_load(IgnoreList *list) {
    list->count = 0;

    uint8_t *data;
    size_t size;
    if (file_read_all(".gitignore", &data, &size) != 0) {
        return 0;  /* 不存在 → 空列表 */
    }

    char line[IGNORE_PATTERN_LEN];
    size_t i = 0;
    while (i < size && list->count < IGNORE_MAX_PATTERNS) {
        size_t j = i;
        while (j < size && data[j] != '\n' && data[j] != '\r') j++;
        size_t len = j - i;
        if (len >= IGNORE_PATTERN_LEN) len = IGNORE_PATTERN_LEN - 1;
        memcpy(line, data + i, len);
        line[len] = 0;

        /* 去掉行尾空白 */
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = 0;
        }

        /* 跳过空行与注释 */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != 0 && *p != '#') {
            snprintf(list->patterns[list->count], IGNORE_PATTERN_LEN, "%s", p);
            list->count++;
        }

        i = j;
        while (i < size && (data[i] == '\n' || data[i] == '\r')) i++;
    }

    free(data);
    return 0;
}

/* ---- 单条模式对单个路径的匹配 ---- */

/*
 * @return 1 匹配（忽略），-1 匹配但取反，0 不匹配
 */
static int pattern_check(const char *pattern, const char *rel_path, int is_dir) {
    char pat[IGNORE_PATTERN_LEN];
    snprintf(pat, sizeof(pat), "%s", pattern);

    int negate = 0;
    char *p = pat;
    if (*p == '!') { negate = 1; p++; }

    int dir_only = 0;
    size_t plen = strlen(p);
    if (plen > 0 && p[plen - 1] == '/') {
        dir_only = 1;
        p[plen - 1] = 0;
        plen--;
    }

    if (dir_only && !is_dir) return 0;

    int anchored = 0;
    if (*p == '/') {
        anchored = 1;
        p++;
    } else if (strchr(p, '/') != NULL) {
        anchored = 1;  /* 含中间 / → 锚定 */
    }

    int matched = 0;
    if (anchored) {
        matched = wildcard_match(p, rel_path);
    } else {
        /* 任意层级：匹配路径的每个组成成分 */
        const char *seg = rel_path;
        while (1) {
            const char *slash = strchr(seg, '/');
            size_t seglen = slash ? (size_t)(slash - seg) : strlen(seg);
            char comp[IGNORE_PATTERN_LEN];
            if (seglen < sizeof(comp)) {
                memcpy(comp, seg, seglen);
                comp[seglen] = 0;
                /* 只对最后一段与目标类型相关的判断：
                 * 中间段视为目录名匹配 */
                if (wildcard_match(p, comp)) {
                    if (slash == NULL) {
                        matched = 1;  /* 最后一段：直接命中 */
                    } else {
                        matched = 1;  /* 中间目录命中 → 其下全部忽略 */
                    }
                    break;
                }
            }
            if (!slash) break;
            seg = slash + 1;
        }
    }

    if (!matched) return 0;
    return negate ? -1 : 1;
}

/* 对单个路径按顺序取最后命中的规则 */
static int path_decision(const IgnoreList *list, const char *rel_path, int is_dir) {
    int decision = 0;
    for (int i = 0; i < list->count; i++) {
        int r = pattern_check(list->patterns[i], rel_path, is_dir);
        if (r == 1) decision = 1;
        else if (r == -1) decision = -1;
    }
    return decision;
}

int ignore_is_ignored(const IgnoreList *list, const char *rel_path, int is_dir) {
    if (list->count == 0) return 0;
    /* .gitignore 自身永不忽略 */
    if (strcmp(rel_path, ".gitignore") == 0) return 0;

    /* 先检查祖先目录：父目录被忽略 → 直接忽略（! 不可恢复） */
    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s", rel_path);
    char *slash = strchr(prefix, '/');
    while (slash) {
        *slash = 0;
        if (path_decision(list, prefix, 1) == 1) return 1;
        *slash = '/';
        slash = strchr(slash + 1, '/');
    }

    return path_decision(list, rel_path, is_dir) == 1;
}
