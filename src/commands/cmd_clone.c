#include "../command.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../core/tree.h"
#include "../core/index.h"
#include "../core/ref.h"
#include "../core/remote.h"
#include "../core/transport.h"
#include "../core/pack_index.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

/*
 * mgit clone <path-or-url> [directory]
 *
 * 克隆一个仓库：
 * - 本地目录：复制对象 + 引用（原路径）
 * - http(s) URL：走 Git Smart HTTP 协议，从真实服务器拉取
 *
 * 流程：
 * 1. 创建目标目录和 .git 结构
 * 2. 传输对象（本地复制 / 网络 pack）
 * 3. 落地分支引用，同步 HEAD
 * 4. 检出工作区（写出文件 + 重建 Index）
 * 5. 配置 remote origin
 */

static void clone_help(void) {
    printf("usage: mgit clone <path-or-url> [directory]\n\n");
    printf("Clone a repository into a new directory.\n");
    printf("Supports local paths and http(s) URLs (Git Smart HTTP).\n");
}

/* 判断是否为网络 URL */
static int is_url(const char *s) {
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

/* Build an absolute path without leaking platform APIs into Git core code. */
static char *absolute_path_dup(const char *path) {
#ifdef _WIN32
    return _fullpath(NULL, path, 0);
#else
    if (!path) return NULL;

    if (path[0] == '/') {
        size_t n = strlen(path) + 1;
        char *out = (char *)malloc(n);
        if (out) memcpy(out, path, n);
        return out;
    }

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;

    size_t n = strlen(cwd) + 1 + strlen(path) + 1;
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s/%s", cwd, path);
    return out;
#endif
}

/* 创建 .git 目录结构（同 init） */
static int create_git_dir(const char *git_dir) {
    char path[1024];

    if (file_mkdir_p(git_dir) != 0) return -1;

    const char *subs[] = {
        "objects", "objects/info", "objects/pack",
        "refs", "refs/heads", "refs/tags",
        "refs/remotes", "refs/remotes/origin"
    };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        path_join(path, sizeof(path), git_dir, subs[i]);
        if (file_mkdir_p(path) != 0) return -1;
    }

    path_join(path, sizeof(path), git_dir, "HEAD");
    return file_write_line(path, "ref: refs/heads/master");
}

/* 复制所有松散对象：src/.git/objects/xx/yyyy -> dst */
static int copy_all_objects(const char *src_git, const char *dst_git) {
    char src_objects[1024];
    snprintf(src_objects, sizeof(src_objects), "%s/objects", src_git);

    DIR *dir = opendir(src_objects);
    if (!dir) return -1;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strlen(e->d_name) != 2) continue;  /* 跳过 . .. info pack */

        char subdir[1024];
        snprintf(subdir, sizeof(subdir), "%s/%s", src_objects, e->d_name);
        if (!file_is_dir(subdir)) continue;

        DIR *sub = opendir(subdir);
        if (!sub) continue;

        struct dirent *f;
        while ((f = readdir(sub)) != NULL) {
            if (strlen(f->d_name) != 38) continue;

            char src_path[1200], dst_path[1200];
            snprintf(src_path, sizeof(src_path), "%s/%s", subdir, f->d_name);
            snprintf(dst_path, sizeof(dst_path),
                     "%s/objects/%s/%s", dst_git, e->d_name, f->d_name);

            uint8_t *data;
            size_t size;
            if (file_read_all(src_path, &data, &size) == 0) {
                char dst_dir[1024];
                snprintf(dst_dir, sizeof(dst_dir),
                         "%s/objects/%s", dst_git, e->d_name);
                file_mkdir_p(dst_dir);
                file_write_all(dst_path, data, size);
                free(data);
                count++;
            }
        }
        closedir(sub);
    }
    closedir(dir);
    return count;
}

/* 复制 packfile（.pack/.idx）：兼容 gc 后对象已打包的仓库 */
static int copy_pack_files(const char *src_git, const char *dst_git) {
    char src_pack[1024];
    snprintf(src_pack, sizeof(src_pack), "%s/objects/pack", src_git);

    DIR *dir = opendir(src_pack);
    if (!dir) return 0;  /* 没有 pack 目录不算错误 */

    char dst_pack[1024];
    snprintf(dst_pack, sizeof(dst_pack), "%s/objects/pack", dst_git);
    file_mkdir_p(dst_pack);

    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        size_t nlen = strlen(e->d_name);
        int is_pack = (nlen > 5 && strcmp(e->d_name + nlen - 5, ".pack") == 0);
        int is_idx = (nlen > 4 && strcmp(e->d_name + nlen - 4, ".idx") == 0);
        if (!is_pack && !is_idx) continue;

        char src_path[1200], dst_path[1200];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_pack, e->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_pack, e->d_name);

        uint8_t *data;
        size_t size;
        if (file_read_all(src_path, &data, &size) == 0) {
            file_write_all(dst_path, data, size);
            free(data);
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* 复制 packed-refs（git gc 把松散引用打包到此文件） */
static void copy_packed_refs(const char *src_git, const char *dst_git) {
    char src_path[1024], dst_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/packed-refs", src_git);
    snprintf(dst_path, sizeof(dst_path), "%s/packed-refs", dst_git);

    uint8_t *data;
    size_t size;
    if (file_read_all(src_path, &data, &size) == 0) {
        file_write_all(dst_path, data, size);
        free(data);
    }
}

/* 取路径的最后一段作为目录名 */
static void path_basename(const char *path, char *out, size_t size) {
    size_t len = strlen(path);
    /* 去掉尾部的 / 或 \ */
    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        len--;
    }
    size_t start = len;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\' &&
           path[start - 1] != ':') {
        start--;
    }
    size_t n = len - start;
    if (n >= size) n = size - 1;
    memcpy(out, path + start, n);
    out[n] = 0;
}

/* 检出工作区：解析 HEAD 提交，展开根 tree（本地/网络克隆共用） */
static void checkout_worktree(const char *dst_dir, RefManager *dst_refs) {
    Hash head_hash;
    int has_head = (ref_resolve_head_quiet(dst_refs, &head_hash) == 0);

    char old_cwd[1024];
    int cwd_saved = (getcwd(old_cwd, sizeof(old_cwd)) != NULL);

    if (has_head && chdir(dst_dir) == 0) {
        ObjectStore *store = object_store_open(".git");
        Index *idx = index_open(".git");
        if (store && idx) {
            Object obj;
            memset(&obj, 0, sizeof(obj));
            if (object_store_read(store, &head_hash, &obj) == 0 &&
                obj.type == OBJ_COMMIT) {
                Commit commit;
                memset(&commit, 0, sizeof(commit));
                commit_parse(obj.data, obj.size, &commit);

                Object tree_obj;
                memset(&tree_obj, 0, sizeof(tree_obj));
                if (object_store_read(store, &commit.tree, &tree_obj) == 0 &&
                    tree_obj.type == OBJ_TREE) {
                    Tree tree;
                    memset(&tree, 0, sizeof(tree));
                    tree_parse(tree_obj.data, tree_obj.size, &tree);
                    tree_restore_worktree(store, idx, &tree);
                    index_write(idx);
                    tree_free(&tree);
                    object_free(&tree_obj);
                }
                commit_free(&commit);
            }
            object_free(&obj);
        }
        if (idx) index_close(idx);
        if (store) object_store_close(store);
    }

    if (cwd_saved) chdir(old_cwd);
}

/* 取 URL 的最后一段作为目录名（去掉 .git 后缀） */
static void url_basename(const char *url, char *out, size_t size) {
    path_basename(url, out, size);
    size_t len = strlen(out);
    if (len > 4 && strcmp(out + len - 4, ".git") == 0) {
        out[len - 4] = 0;
    }
}

/*
 * 网络克隆：Git Smart HTTP 协议
 *
 * 1. GET info/refs -> 引用广告（分支/标签 + 服务端 HEAD）
 * 2. POST git-upload-pack -> side-band 封装的 packfile
 * 3. 解包为松散对象
 * 4. 落地 refs/remotes/origin/<branch>、标签、本地分支与 HEAD
 * 5. 检出工作区
 */
static int clone_from_url(const char *url, const char *dst_dir) {
    char dst_buf[512];
    if (!dst_dir) {
        url_basename(url, dst_buf, sizeof(dst_buf));
        dst_dir = dst_buf;
        if (!dst_dir[0]) {
            mgit_error("cannot derive directory name from URL");
            return -1;
        }
    }
    if (file_exists(dst_dir)) {
        mgit_error("destination path '%s' already exists", dst_dir);
        return -1;
    }

    printf("Cloning into '%s'...\n", dst_dir);

    /* 1. 引用广告 */
    RefAd ad;
    if (transport_get_refs(url, &ad) != 0) return -1;
    if (ad.count == 0) {
        mgit_error("remote repository is empty");
        ref_ad_free(&ad);
        return -1;
    }
    printf("remote: %lu refs found\n", (unsigned long)ad.count);

    /* 2. 拉取 pack */
    uint8_t *pack = NULL;
    size_t pack_size = 0;
    if (transport_fetch_pack(url, &ad, &pack, &pack_size) != 0) {
        ref_ad_free(&ad);
        return -1;
    }
    printf("remote: received %lu bytes of pack data\n",
           (unsigned long)pack_size);

    /* 3. 创建 .git 结构 + 解包 */
    char dst_git[1024];
    snprintf(dst_git, sizeof(dst_git), "%s/.git", dst_dir);
    if (create_git_dir(dst_git) != 0) {
        mgit_error("failed to create repository structure");
        free(pack);
        ref_ad_free(&ad);
        return -1;
    }

    ObjectStore *store = object_store_open(dst_git);
    if (!store) {
        free(pack);
        ref_ad_free(&ad);
        return -1;
    }
    size_t nobj = 0;
    int rc = pack_unpack(store, pack, pack_size, &nobj);
    object_store_close(store);
    free(pack);
    if (rc != 0) {
        ref_ad_free(&ad);
        return -1;
    }
    printf("Receiving objects: %lu, done.\n", (unsigned long)nobj);

    /* 4. 落地引用 */
    RefManager *refs = ref_manager_open(dst_git);
    if (!refs) {
        ref_ad_free(&ad);
        return -1;
    }

    const char *head_branch = ad.head_branch[0] ? ad.head_branch : NULL;
    Hash head_hash;
    memset(&head_hash, 0, sizeof(head_hash));

    for (size_t i = 0; i < ad.count; i++) {
        RemoteRef *rr = &ad.refs[i];
        if (strncmp(rr->name, "refs/heads/", 11) == 0) {
            /* 分支 -> refs/remotes/origin/<branch> */
            char tracking[512];
            snprintf(tracking, sizeof(tracking), "refs/remotes/origin/%s",
                     rr->name + 11);
            ref_update(refs, tracking, &rr->hash);

            if (!head_branch) head_branch = rr->name + 11;  /* 兜底：第一个分支 */
        } else if (strncmp(rr->name, "refs/tags/", 10) == 0) {
            /* 标签原样落地（与真实 git clone 一致） */
            ref_update(refs, rr->name, &rr->hash);
        }
    }

    /* HEAD 分支：本地分支引用 + HEAD 指向 */
    if (head_branch) {
        char tracking[512], full[512];
        snprintf(tracking, sizeof(tracking), "refs/remotes/origin/%s",
                 head_branch);
        snprintf(full, sizeof(full), "refs/heads/%s", head_branch);
        if (ref_resolve_quiet(refs, tracking, &head_hash) == 0) {
            ref_update(refs, full, &head_hash);  /* 本地分支指向同一提交 */
            ref_set_head(refs, head_branch);
        } else {
            head_branch = NULL;  /* 服务端 HEAD 指向的分支不在广告里 */
        }
    }
    checkout_worktree(dst_dir, refs);
    ref_manager_close(refs);

    char checked_out[256] = {0};
    if (head_branch) snprintf(checked_out, sizeof(checked_out), "%s",
                              head_branch);
    ref_ad_free(&ad);

    /* 5. 配置 remote origin */
    remote_config_set(dst_git, "origin", url);

    if (checked_out[0]) {
        printf("Checked out branch '%s'.\n", checked_out);
    }
    return 0;
}

static int clone_run(int argc, char **argv) {
    const char *src_path = NULL;
    const char *dst_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (!src_path) src_path = argv[i];
        else if (!dst_dir) dst_dir = argv[i];
    }

    if (!src_path) {
        mgit_error("you must specify a repository to clone");
        clone_help();
        return -1;
    }

    /* 网络 URL：走 Smart HTTP 协议 */
    if (is_url(src_path)) {
        return clone_from_url(src_path, dst_dir);
    }

    char src_git[1024];
    snprintf(src_git, sizeof(src_git), "%s/.git", src_path);
    if (!file_is_dir(src_git)) {
        mgit_error("'%s' is not a git repository", src_path);
        return -1;
    }

    /* 目标目录 */
    char dst_buf[512];
    if (!dst_dir) {
        path_basename(src_path, dst_buf, sizeof(dst_buf));
        dst_dir = dst_buf;
    }
    if (file_exists(dst_dir)) {
        mgit_error("destination path '%s' already exists", dst_dir);
        return -1;
    }

    printf("Cloning into '%s'...\n", dst_dir);

    /* 1. 创建 .git 结构 */
    char dst_git[1024];
    snprintf(dst_git, sizeof(dst_git), "%s/.git", dst_dir);
    if (create_git_dir(dst_git) != 0) {
        mgit_error("failed to create repository structure");
        return -1;
    }

    /* 2. 复制对象（松散 + pack） */
    if (copy_all_objects(src_git, dst_git) < 0) {
        mgit_error("failed to copy objects");
        return -1;
    }
    copy_pack_files(src_git, dst_git);
    copy_packed_refs(src_git, dst_git);

    /* 3. 复制分支引用 */
    RefManager *src_refs = ref_manager_open(src_git);
    RefManager *dst_refs = ref_manager_open(dst_git);
    if (!src_refs || !dst_refs) {
        if (src_refs) ref_manager_close(src_refs);
        if (dst_refs) ref_manager_close(dst_refs);
        mgit_error("failed to open refs");
        return -1;
    }

    char branches[64][256];
    int count = ref_list_branches(src_refs, branches, 64);
    for (int i = 0; i < count; i++) {
        Hash hash;
        if (ref_resolve_quiet(src_refs, branches[i], &hash) == 0) {
            ref_update(dst_refs, branches[i], &hash);
        }
    }

    /* 同步 HEAD 指向 */
    char head_branch[256];
    if (ref_get_head_branch(src_refs, head_branch, sizeof(head_branch)) == 0) {
        const char *name = head_branch;
        if (strncmp(name, "refs/heads/", 11) == 0) name += 11;
        ref_set_head(dst_refs, name);
    }

    ref_manager_close(src_refs);

    /* 4. 检出工作区 */
    checkout_worktree(dst_dir, dst_refs);
    ref_manager_close(dst_refs);

    /* 5. 配置 remote origin（存绝对路径，避免相对路径失效） */
    char *abs_src = absolute_path_dup(src_path);
    if (abs_src) {
        remote_config_set(dst_git, "origin", abs_src);
        free(abs_src);
    } else {
        remote_config_set(dst_git, "origin", src_path);
    }

    return 0;
}

Command cmd_clone = {
    .name = "clone",
    .description = "Clone a repository into a new directory",
    .run = clone_run,
    .help = clone_help
};
