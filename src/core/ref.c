#include "ref.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

RefManager *ref_manager_open(const char *git_dir) {
    RefManager *mgr = (RefManager *)malloc(sizeof(RefManager));
    if (!mgr) return NULL;

    mgr->git_dir = (char *)malloc(strlen(git_dir) + 1);
    if (!mgr->git_dir) {
        free(mgr);
        return NULL;
    }
    strcpy(mgr->git_dir, git_dir);
    return mgr;
}

void ref_manager_close(RefManager *mgr) {
    if (mgr) {
        free(mgr->git_dir);
        free(mgr);
    }
}

/*
 * 解析 HEAD
 * 
 * HEAD 文件格式：
 * 1. 符号引用: "ref: refs/heads/master\n"
 * 2. 直接引用: "abc123def456...\n" (detached HEAD)
 */
int ref_resolve_head(RefManager *mgr, Hash *out) {
    char path[1024];
    path_join(path, sizeof(path), mgr->git_dir, "HEAD");

    char buf[1024];
    if (file_read_line(path, buf, sizeof(buf)) != 0) {
        mgit_error("cannot read HEAD");
        return -1;
    }

    /* 检查是否是符号引用 */
    if (strncmp(buf, "ref: ", 5) == 0) {
        /* 符号引用: ref: refs/heads/master */
        return ref_resolve(mgr, buf + 5, out);
    } else {
        /* 直接引用: commit hash */
        return hex_to_hash(buf, out);
    }
}

/*
 * 静默版解析 HEAD（失败时不打印错误）
 * 用于探测性调用，如首次 commit 前检查是否有历史
 */
int ref_resolve_head_quiet(RefManager *mgr, Hash *out) {
    char path[1024];
    path_join(path, sizeof(path), mgr->git_dir, "HEAD");

    char buf[1024];
    if (file_read_line(path, buf, sizeof(buf)) != 0) {
        return -1;
    }

    if (strncmp(buf, "ref: ", 5) == 0) {
        return ref_resolve_quiet(mgr, buf + 5, out);
    } else {
        return hex_to_hash(buf, out);
    }
}

int ref_get_head_branch(RefManager *mgr, char *buf, int size) {
    char path[1024];
    path_join(path, sizeof(path), mgr->git_dir, "HEAD");

    char line[1024];
    if (file_read_line(path, line, sizeof(line)) != 0) {
        mgit_error("cannot read HEAD");
        return -1;
    }

    if (strncmp(line, "ref: ", 5) == 0) {
        /* 返回完整引用路径 */
        snprintf(buf, size, "%s", line + 5);
        return 0;
    }

    /* detached HEAD */
    snprintf(buf, size, "HEAD");
    return 0;
}

/*
 * 解析引用（内部实现，quiet 控制是否报错）
 * 
 * 支持多种格式：
 * - "HEAD" -> 解析 HEAD
 * - "master" -> 解析 refs/heads/master
 * - "refs/heads/master" -> 直接解析
 */
/* fallback lookup in packed-refs (git gc packs loose refs here) */
static int packed_refs_find(RefManager *mgr, const char *name, Hash *out) {
    char path[1024];
    path_join(path, sizeof(path), mgr->git_dir, "packed-refs");

    uint8_t *data;
    size_t size;
    if (file_read_all(path, &data, &size) != 0) return -1;

    char full_ref[1024];
    snprintf(full_ref, sizeof(full_ref), "refs/heads/%s", name);

    /* 同名 tag 候选（分支优先，与真实 git 一致） */
    char tag_ref[1024];
    snprintf(tag_ref, sizeof(tag_ref), "refs/tags/%s", name);

    char hex[41];
    int rc = -1;
    int found_tag = 0;
    size_t pos = 0;
    while (pos < size) {
        size_t eol = pos;
        while (eol < size && data[eol] != '\n') eol++;
        size_t len = eol - pos;
        if (len > 0 && data[pos + len - 1] == '\r') len--;
        const char *line = (const char *)data + pos;
        if (len >= 42 && line[40] == ' ' && line[0] != '#' && line[0] != '^') {
            size_t rlen = len - 41;
            const char *rname = line + 41;
            int match_branch = (rlen == strlen(name) && strncmp(rname, name, rlen) == 0) ||
                               (rlen == strlen(full_ref) && strncmp(rname, full_ref, rlen) == 0);
            int match_tag = !match_branch &&
                            (rlen == strlen(tag_ref) && strncmp(rname, tag_ref, rlen) == 0);
            if (match_branch || (match_tag && !found_tag)) {
                memcpy(hex, line, 40);
                hex[40] = 0;
                if (hex_to_hash(hex, out) == 0) {
                    if (match_branch) {
                        rc = 0;
                        break;
                    }
                    rc = 0;
                    found_tag = 1;  /* 记下 tag，继续找是否有同名分支 */
                }
            }
        }
        pos = eol + 1;
    }
    free(data);
    return rc;
}

static int ref_resolve_impl(RefManager *mgr, const char *name, Hash *out, int quiet) {
    char path[1024];
    char line[1024];

    if (strcmp(name, "HEAD") == 0) {
        /* 解析 HEAD */
        path_join(path, sizeof(path), mgr->git_dir, "HEAD");
        if (file_read_line(path, line, sizeof(line)) != 0) {
            if (!quiet) mgit_error("cannot read HEAD");
            return -1;
        }

        if (strncmp(line, "ref: ", 5) == 0) {
            return ref_resolve_impl(mgr, line + 5, out, quiet);
        }
        return hex_to_hash(line, out);
    }

    /* 尝试直接路径 */
    path_join(path, sizeof(path), mgr->git_dir, name);
    if (file_exists(path)) {
        if (file_read_line(path, line, sizeof(line)) != 0) {
            if (!quiet) mgit_error("cannot read ref: %s", name);
            return -1;
        }
        return hex_to_hash(line, out);
    }

    /* 尝试 refs/heads/ 前缀 */
    char full_ref[1024];
    snprintf(full_ref, sizeof(full_ref), "refs/heads/%s", name);
    path_join(path, sizeof(path), mgr->git_dir, full_ref);
    if (file_exists(path)) {
        if (file_read_line(path, line, sizeof(line)) != 0) {
            if (!quiet) mgit_error("cannot read ref: %s", full_ref);
            return -1;
        }
        return hex_to_hash(line, out);
    }

    /* 尝试 refs/tags/ 前缀（分支优先，与真实 git 一致） */
    char tag_ref[1024];
    snprintf(tag_ref, sizeof(tag_ref), "refs/tags/%s", name);
    path_join(path, sizeof(path), mgr->git_dir, tag_ref);
    if (file_exists(path)) {
        if (file_read_line(path, line, sizeof(line)) != 0) {
            if (!quiet) mgit_error("cannot read ref: %s", tag_ref);
            return -1;
        }
        return hex_to_hash(line, out);
    }

    /* fallback: packed-refs (written by git gc) */
    if (packed_refs_find(mgr, name, out) == 0) return 0;

    if (!quiet) mgit_error("unknown ref: %s", name);
    return -1;
}

int ref_resolve(RefManager *mgr, const char *name, Hash *out) {
    return ref_resolve_impl(mgr, name, out, 0);
}

int ref_resolve_quiet(RefManager *mgr, const char *name, Hash *out) {
    return ref_resolve_impl(mgr, name, out, 1);
}

int ref_update(RefManager *mgr, const char *name, const Hash *hash) {
    char path[1024];
    path_join(path, sizeof(path), mgr->git_dir, name);

    char hex[HASH_HEX_SIZE];
    hash_to_hex(hash, hex);

    return file_write_line(path, hex);
}

int ref_create_branch(RefManager *mgr, const char *name, const Hash *hash) {
    char ref_path[1024];
    snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", name);
    return ref_update(mgr, ref_path, hash);
}

int ref_delete_branch(RefManager *mgr, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/refs/heads/%s", mgr->git_dir, name);

    if (!file_exists(path)) {
        mgit_error("branch '%s' not found", name);
        return -1;
    }

    return file_delete(path);
}

int ref_set_head(RefManager *mgr, const char *name) {
    char path[1024];
    path_join(path, sizeof(path), mgr->git_dir, "HEAD");

    char content[1024];
    snprintf(content, sizeof(content), "ref: refs/heads/%s", name);

    return file_write_line(path, content);
}

int ref_set_head_detached(RefManager *mgr, const Hash *hash) {
    char path[1024];
    path_join(path, sizeof(path), mgr->git_dir, "HEAD");

    char hex[HASH_HEX_SIZE];
    hash_to_hex(hash, hex);
    return file_write_line(path, hex);
}

/*
 * 列出所有分支
 * 遍历 refs/heads/ 目录下的所有文件
 */
int ref_list_branches(RefManager *mgr, char branches[][256], int max_count) {
    char refs_dir[1024];
    path_join(refs_dir, sizeof(refs_dir), mgr->git_dir, "refs/heads");

    if (!file_is_dir(refs_dir)) {
        return 0;
    }

    /* 使用 dirent 遍历目录 */
#ifdef _WIN32
    /* Windows: 使用 FindFirstFile/FindNextFile */
    WIN32_FIND_DATAA ffd;
    char search_path[1024];
    snprintf(search_path, sizeof(search_path), "%s/*", refs_dir);
    
    HANDLE hFind = FindFirstFileA(search_path, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return 0;
    }

    int count = 0;
    int truncated = 0;
    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (count >= max_count) { truncated = 1; break; }
        
        snprintf(branches[count], 256, "refs/heads/%s", ffd.cFileName);
        count++;
    } while (FindNextFileA(hFind, &ffd) != 0);
    
    FindClose(hFind);
    return truncated ? -2 : count;
#else
    /* POSIX: 使用 opendir/readdir */
    DIR *dir = opendir(refs_dir);
    if (!dir) return 0;

    int count = 0;
    int truncated = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count >= max_count) { truncated = 1; break; }
        
        snprintf(branches[count], 256, "refs/heads/%s", entry->d_name);
        count++;
    }

    closedir(dir);
    return truncated ? -2 : count;
#endif
}

/* 记录 HEAD 移动（追加到 .git/logs/HEAD，供各命令在 HEAD 变更后调用） */
int reflog_append(const char *old_hex, const char *new_hex, const char *action) {
    char log_path[256];
    snprintf(log_path, sizeof(log_path), ".git/logs/HEAD");

    /* 确保 logs 目录存在 */
    file_mkdir_p(".git/logs");

    /* 获取时间 */
    time_t now = time(NULL);

#ifdef _WIN32
    TIME_ZONE_INFORMATION tzi;
    GetTimeZoneInformation(&tzi);
    int bias = tzi.Bias;
    char sign = bias > 0 ? '-' : '+';
    bias = abs(bias);
    char tz[8];
    snprintf(tz, sizeof(tz), "%c%02d%02d", sign, bias / 60, bias % 60);
#else
    char tz[8];
    struct tm *tm = localtime(&now);
    strftime(tz, sizeof(tz), "%z", tm);
#endif

    FILE *fp = fopen(log_path, "a");
    if (!fp) return -1;

    fprintf(fp, "%s %s mgit user <mgit@example.com> %lu %s\t%s\n",
            old_hex, new_hex, (unsigned long)now, tz, action);
    fclose(fp);
    return 0;
}
