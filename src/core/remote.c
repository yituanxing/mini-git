#include "remote.h"
#include "commit.h"
#include "tree.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * remote 配置与对象传输实现
 *
 * 配置格式（.git/config，与真实 git 一致，键名为 url）：
 *   [remote "origin"]
 *       url = C:/path/to/repo
 *
 * 历史版本曾用 path = ；读取时两种都认，写出统一用 url。
 *
 * 传输原理：Git 对象按内容寻址，松散对象文件可以直接按字节复制。
 */

#define CONFIG_MAX 8192

static void config_path(const char *git_dir, char *buf, size_t size) {
    snprintf(buf, size, "%s/config", git_dir);
}

/* 读取 config 到缓冲区；文件不存在返回 -1 */
static int config_read(const char *git_dir, char *buf, size_t size) {
    char path[1024];
    config_path(git_dir, path, sizeof(path));
    uint8_t *data;
    size_t len;
    if (file_read_all(path, &data, &len) != 0) {
        buf[0] = 0;
        return -1;
    }
    if (len >= size) len = size - 1;
    memcpy(buf, data, len);
    buf[len] = 0;
    free(data);
    return 0;
}

static int config_write(const char *git_dir, const char *buf) {
    char path[1024];
    config_path(git_dir, path, sizeof(path));
    return file_write_all(path, (const uint8_t *)buf, strlen(buf));
}

/* 匹配 "[remote "name"]" 段头，提取 name；返回 1 表示匹配 */
static int match_remote_section(const char *line, char *name_out, size_t nsz) {
    if (strncmp(line, "[remote \"", 9) != 0) return 0;
    const char *p = line + 9;
    const char *q = strchr(p, '"');
    if (!q) return 0;
    size_t n = (size_t)(q - p);
    if (n >= nsz) n = nsz - 1;
    memcpy(name_out, p, n);
    name_out[n] = 0;
    return 1;
}

/* 解析 "\turl = value"（或旧格式 "path ="）行；返回 1 表示命中 */
static int parse_path_line(const char *line, char *val, size_t vsz) {
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "url", 3) == 0) {
        line += 3;
    } else if (strncmp(line, "path", 4) == 0) {
        line += 4;
    } else {
        return 0;
    }
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '=') return 0;
    line++;
    while (*line == ' ' || *line == '\t') line++;
    size_t n = 0;
    while (line[n] && line[n] != '\n' && line[n] != '\r') n++;
    if (n >= vsz) n = vsz - 1;
    memcpy(val, line, n);
    val[n] = 0;
    return 1;
}

int remote_config_get(const char *git_dir, const char *name,
                      char *path_out, size_t size) {
    char buf[CONFIG_MAX];
    config_read(git_dir, buf, sizeof(buf));

    int in_target = 0;
    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[600];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;

        char sec[64];
        if (line[0] == '[') {
            in_target = match_remote_section(line, sec, sizeof(sec)) &&
                        strcmp(sec, name) == 0;
        } else if (in_target) {
            char val[512];
            if (parse_path_line(line, val, sizeof(val))) {
                snprintf(path_out, size, "%s", val);
                return 0;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return -1;
}

int remote_config_set(const char *git_dir, const char *name, const char *path) {
    /* 反斜杠 → 正斜杠：git config 把反斜杠当转义符（\U \D 非法），
     * 真实 git 对 Windows 路径也一律写 C:/... 形式 */
    char norm[512];
    size_t i;
    for (i = 0; path[i] && i < sizeof(norm) - 1; i++) {
        norm[i] = (path[i] == '\\') ? '/' : path[i];
    }
    norm[i] = 0;
    path = norm;

    char buf[CONFIG_MAX];
    config_read(git_dir, buf, sizeof(buf));

    char out[CONFIG_MAX];
    size_t o = 0;
    out[0] = 0;

    int in_target = 0;
    int replaced = 0;
    int saw_target = 0;

    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[600];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;

        if (line[0] == '[') {
            /* 离开目标段时若还没替换，补一行 path */
            if (in_target && !replaced) {
                o += (size_t)snprintf(out + o, sizeof(out) - o,
                                      "\turl = %s\n", path);
                replaced = 1;
            }
            char sec[64];
            in_target = match_remote_section(line, sec, sizeof(sec)) &&
                        strcmp(sec, name) == 0;
            if (in_target) saw_target = 1;
        } else if (in_target) {
            char val[512];
            if (parse_path_line(line, val, sizeof(val))) {
                /* 替换旧值 */
                o += (size_t)snprintf(out + o, sizeof(out) - o,
                                      "\turl = %s\n", path);
                replaced = 1;
                p = nl ? nl + 1 : NULL;
                continue;
            }
        }

        o += (size_t)snprintf(out + o, sizeof(out) - o, "%s\n", line);
        p = nl ? nl + 1 : NULL;
    }

    if (in_target && !replaced) {
        o += (size_t)snprintf(out + o, sizeof(out) - o,
                              "\turl = %s\n", path);
        replaced = 1;
    }

    if (!saw_target) {
        /* 追加新段 */
        if (o > 0 && out[o - 1] != '\n') {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "\n");
        }
        snprintf(out + o, sizeof(out) - o,
                 "[remote \"%s\"]\n\turl = %s\n", name, path);
    }

    return config_write(git_dir, out);
}

int remote_config_remove(const char *git_dir, const char *name) {
    char buf[CONFIG_MAX];
    config_read(git_dir, buf, sizeof(buf));

    char out[CONFIG_MAX];
    size_t o = 0;
    out[0] = 0;

    int in_target = 0;
    int found = 0;

    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[600];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;

        if (line[0] == '[') {
            char sec[64];
            in_target = match_remote_section(line, sec, sizeof(sec)) &&
                        strcmp(sec, name) == 0;
            if (in_target) found = 1;
        }

        if (!in_target) {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%s\n", line);
        }
        p = nl ? nl + 1 : NULL;
    }

    if (!found) return -1;
    return config_write(git_dir, out);
}

int remote_config_list(const char *git_dir,
                       char names[][64], char paths[][512], int max_count) {
    char buf[CONFIG_MAX];
    config_read(git_dir, buf, sizeof(buf));

    int count = 0;
    int in_any = 0;

    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[600];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;

        if (line[0] == '[') {
            char sec[64];
            if (match_remote_section(line, sec, sizeof(sec))) {
                in_any = 1;
                if (count < max_count) {
                    snprintf(names[count], 64, "%s", sec);
                    paths[count][0] = 0;
                }
            } else {
                in_any = 0;
            }
            if (in_any && count < max_count) count++;
        } else if (in_any && count > 0) {
            char val[512];
            if (parse_path_line(line, val, sizeof(val))) {
                snprintf(paths[count - 1], 512, "%s", val);
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return count;
}

/* ---- 对象传输 ---- */

int remote_copy_object(ObjectStore *src, ObjectStore *dst, const Hash *hash) {
    if (object_exists(dst, hash)) return 0;

    char sp[1024], dp[1024];
    object_path(src, hash, sp, sizeof(sp));
    object_path(dst, hash, dp, sizeof(dp));

    uint8_t *data;
    size_t size;
    if (file_read_all(sp, &data, &size) != 0) return -1;

    /* 确保目标子目录存在（objects/xx） */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", dp);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        file_mkdir_p(dir);
    }

    int r = file_write_all(dp, data, size);
    free(data);
    return r;
}

/* 递归传输 tree 及其引用的所有对象 */
static int send_tree(ObjectStore *src, ObjectStore *dst, const Hash *tree_hash) {
    if (object_exists(dst, tree_hash)) return 0;

    Object obj;
    memset(&obj, 0, sizeof(obj));
    if (object_store_read(src, tree_hash, &obj) != 0) return -1;
    if (obj.type != OBJ_TREE) {
        object_free(&obj);
        return -1;
    }

    Tree tree;
    memset(&tree, 0, sizeof(tree));
    if (tree_parse(obj.data, obj.size, &tree) != 0) {
        object_free(&obj);
        return -1;
    }
    object_free(&obj);

    if (remote_copy_object(src, dst, tree_hash) != 0) {
        tree_free(&tree);
        return -1;
    }

    int ret = 0;
    for (size_t i = 0; i < tree.count; i++) {
        if (tree.entries[i].type == TREE_ENTRY_TREE) {
            if (send_tree(src, dst, &tree.entries[i].hash) != 0) {
                ret = -1;
                break;
            }
        } else {
            if (remote_copy_object(src, dst, &tree.entries[i].hash) != 0) {
                ret = -1;
                break;
            }
        }
    }

    tree_free(&tree);
    return ret;
}

int remote_send_reachable(const char *src_git_dir, const char *dst_git_dir,
                          const Hash *commit_hash) {
    ObjectStore *src = object_store_open(src_git_dir);
    ObjectStore *dst = object_store_open(dst_git_dir);
    if (!src || !dst) {
        if (src) object_store_close(src);
        if (dst) object_store_close(dst);
        return -1;
    }

    size_t cap = 64;
    size_t head = 0, tail = 0;
    Hash *queue = (Hash *)malloc(cap * sizeof(Hash));
    if (!queue) {
        object_store_close(src);
        object_store_close(dst);
        return -1;
    }
    queue[tail++] = *commit_hash;

    int ret = 0;
    while (head < tail) {
        Hash h = queue[head++];

        /* 目标已有该 commit，说明其历史与树都已存在 */
        if (object_exists(dst, &h)) continue;

        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(src, &h, &obj) != 0 || obj.type != OBJ_COMMIT) {
            object_free(&obj);
            ret = -1;
            break;
        }

        Commit commit;
        memset(&commit, 0, sizeof(commit));
        if (commit_parse(obj.data, obj.size, &commit) != 0) {
            object_free(&obj);
            commit_free(&commit);
            ret = -1;
            break;
        }
        object_free(&obj);

        if (remote_copy_object(src, dst, &h) != 0 ||
            send_tree(src, dst, &commit.tree) != 0) {
            commit_free(&commit);
            ret = -1;
            break;
        }

        for (int i = 0; i < commit.parent_count; i++) {
            if (object_exists(dst, &commit.parents[i])) continue;
            if (tail == cap) {
                size_t ncap = cap * 2;
                Hash *p = (Hash *)realloc(queue, ncap * sizeof(Hash));
                if (!p) {
                    ret = -1;
                    break;
                }
                queue = p;
                cap = ncap;
            }
            queue[tail++] = commit.parents[i];
        }
        commit_free(&commit);
        if (ret != 0) break;
    }

    free(queue);
    object_store_close(src);
    object_store_close(dst);
    return ret;
}
