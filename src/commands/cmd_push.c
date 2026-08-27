#include "../command.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../core/tree.h"
#include "../core/ref.h"
#include "../core/remote.h"
#include "../core/transport.h"
#include "../core/pack.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * mgit push [<remote>] [<branch>]
 *
 * 将本地分支推送到远程仓库：
 * - 本地目录：复制对象 + 更新引用（原路径）
 * - http(s) URL：走 Git Smart HTTP 的 git-receive-pack 协议
 *
 * 流程：
 * 1. 解析本地分支指向的 commit
 * 2. 检查快进（远程分支存在时，远程 tip 必须是本地的祖先）
 * 3. 传输 commit 可达的所有对象（远端已有的不重发）
 * 4. 更新远程的 refs/heads/<branch>
 */

static void push_help(void) {
    printf("usage: mgit push [<remote>] [<branch>]\n\n");
    printf("Update remote refs along with associated objects.\n");
    printf("Defaults: remote 'origin', current branch.\n");
    printf("Supports local paths and http(s) URLs (Git Smart HTTP).\n");
}

/* 判断是否为网络 URL */
static int is_url(const char *s) {
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

/* BFS 检查 target 是否在 start 的历史中（本地对象库） */
static int commit_is_ancestor(ObjectStore *store, const Hash *target,
                              const Hash *start) {
    if (hash_equal(target, start)) return 1;
    Hash queue[1000];
    int head = 0, tail = 0;
    queue[tail++] = *start;

    while (head < tail && head < 999) {
        Hash cur = queue[head++];
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &cur, &obj) != 0 || obj.type != OBJ_COMMIT) {
            object_free(&obj);
            continue;
        }
        Commit c;
        memset(&c, 0, sizeof(c));
        commit_parse(obj.data, obj.size, &c);
        object_free(&obj);
        for (int i = 0; i < c.parent_count; i++) {
            if (hash_equal(target, &c.parents[i])) {
                commit_free(&c);
                return 1;
            }
            if (tail < 1000) queue[tail++] = c.parents[i];
        }
        commit_free(&c);
    }
    return 0;
}

/* ---------- 网络推送（git-receive-pack） ---------- */

/* 动态哈希数组（线性去重，教学规模够用） */
typedef struct {
    Hash *items;
    size_t count;
    size_t cap;
} HashArr;

static void hasharr_free(HashArr *a) {
    free(a->items);
    a->items = NULL;
    a->count = a->cap = 0;
}

/* 已存在返回 0 不插入；新插入返回 1；失败返回 -1 */
static int hasharr_add(HashArr *a, const Hash *h) {
    for (size_t i = 0; i < a->count; i++) {
        if (hash_equal(&a->items[i], h)) return 0;
    }
    if (a->count >= a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 64;
        Hash *p = (Hash *)realloc(a->items, ncap * sizeof(Hash));
        if (!p) return -1;
        a->items = p;
        a->cap = ncap;
    }
    a->items[a->count++] = *h;
    return 1;
}

/* 哈希是否在引用广告中（远端已知的对象边界） */
static int ad_contains(const RefAd *ad, const Hash *h) {
    for (size_t i = 0; i < ad->count; i++) {
        if (hash_equal(&ad->refs[i].hash, h)) return 1;
        if (ad->refs[i].has_peeled &&
            hash_equal(&ad->refs[i].peeled, h)) return 1;
    }
    return 0;
}

/* 递归收集 tree 及其引用的所有对象 */
static int collect_tree(ObjectStore *store, const Hash *tree_hash,
                        HashArr *out) {
    int added = hasharr_add(out, tree_hash);
    if (added <= 0) return added;  /* 已收过或失败 */

    Object obj;
    memset(&obj, 0, sizeof(obj));
    if (object_store_read(store, tree_hash, &obj) != 0 ||
        obj.type != OBJ_TREE) {
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

    int ret = 0;
    for (size_t i = 0; i < tree.count; i++) {
        if (hasharr_add(out, &tree.entries[i].hash) < 0) {
            ret = -1;
            break;
        }
        if (tree.entries[i].type == TREE_ENTRY_TREE) {
            if (collect_tree(store, &tree.entries[i].hash, out) != 0) {
                ret = -1;
                break;
            }
        }
    }
    tree_free(&tree);
    return ret;
}

/*
 * 收集待推送对象：从 tip 向后遍历 commit，遇到远端广告里
 * 已存在的 commit 即停（与真实 git 的 have/want 简化等价：
 * 直接拿广告引用当分界点）
 */
static int collect_push_objects(ObjectStore *store, const Hash *tip,
                                const RefAd *ad, HashArr *out) {
    /* 动态队列：深历史一次推送的提交数可能很多，
     * 固定长度会在溢出时静默丢提交，导致推上去的 pack 缺祖先 */
    size_t cap = 1024, head = 0, tail = 0;
    Hash *queue = (Hash *)malloc(cap * sizeof(Hash));
    if (!queue) return -1;
    queue[tail++] = *tip;

    int rc = 0;
    while (head < tail) {
        Hash h = queue[head++];
        if (ad_contains(ad, &h)) continue;   /* 远端已有：历史与树都在 */
        if (hasharr_add(out, &h) < 0) { rc = -1; break; }

        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &h, &obj) != 0 ||
            obj.type != OBJ_COMMIT) {
            object_free(&obj);
            rc = -1;
            break;
        }
        Commit c;
        memset(&c, 0, sizeof(c));
        commit_parse(obj.data, obj.size, &c);
        object_free(&obj);

        if (collect_tree(store, &c.tree, out) != 0) {
            commit_free(&c);
            rc = -1;
            break;
        }
        for (int i = 0; i < c.parent_count; i++) {
            if (ad_contains(ad, &c.parents[i])) continue;
            if (tail >= cap) {
                size_t ncap = cap * 2;
                Hash *nq = (Hash *)realloc(queue, ncap * sizeof(Hash));
                if (!nq) { rc = -1; break; }
                queue = nq;
                cap = ncap;
            }
            queue[tail++] = c.parents[i];
        }
        commit_free(&c);
        if (rc != 0) break;
    }
    free(queue);
    return rc;
}

/*
 * 网络推送：Smart HTTP receive-pack
 *
 * 1. GET info/refs?service=git-receive-pack -> 远端引用广告
 * 2. 快进检查（远端 tip 必须是本地祖先）
 * 3. 收集远端缺的对象 -> 内存 pack
 * 4. POST git-receive-pack：引用更新指令 + pack，解析回执
 */
static int push_to_url(const char *url,
                       const char *ref_name, const char *branch,
                       const Hash *local_hash) {
    /* 1. 引用广告 */
    RefAd ad;
    if (transport_get_refs_service(url, "git-receive-pack", &ad) != 0) {
        return -1;
    }

    /* 远端该分支的当前值（不存在 = 全零，新建分支） */
    Hash old_hash;
    memset(&old_hash, 0, sizeof(old_hash));
    for (size_t i = 0; i < ad.count; i++) {
        if (strcmp(ad.refs[i].name, ref_name) == 0) {
            old_hash = ad.refs[i].hash;
            break;
        }
    }

    /* 2. 引用未变化：无事可做 */
    if (hash_equal(&old_hash, local_hash)) {
        printf("Everything up-to-date\n");
        ref_ad_free(&ad);
        return 0;
    }

    /* 3. 快进检查 */
    if (!hash_is_zero(&old_hash)) {
        ObjectStore *store = object_store_open(".git");
        int ff = store && commit_is_ancestor(store, &old_hash, local_hash);
        if (store) object_store_close(store);
        if (!ff) {
            mgit_error("failed to push: non-fast-forward "
                       "(fetch and merge first)");
            ref_ad_free(&ad);
            return -1;
        }
    }

    /* 4. 收集缺的对象并构建 pack（可能为空：对象远端都有，
     * 但引用本身仍需更新，照常发送空 pack） */
    ObjectStore *store = object_store_open(".git");
    if (!store) {
        ref_ad_free(&ad);
        return -1;
    }
    HashArr objs = {0};
    int rc = collect_push_objects(store, local_hash, &ad, &objs);

    uint8_t *pack = NULL;
    size_t pack_size = 0, packed = 0;
    if (rc == 0) {
        rc = pack_build_memory(store, objs.items, objs.count,
                               &packed, &pack, &pack_size);
    }
    hasharr_free(&objs);
    object_store_close(store);
    if (rc != 0) {
        ref_ad_free(&ad);
        return -1;
    }
    if (packed > 0) {
        printf("Writing objects: %lu, %.1f KiB, done.\n",
               (unsigned long)packed, (double)pack_size / 1024.0);
    }

    /* 5. POST git-receive-pack */
    PushUpdate upd;
    memset(&upd, 0, sizeof(upd));
    snprintf(upd.ref, sizeof(upd.ref), "%s", ref_name);
    upd.old_hash = old_hash;
    upd.new_hash = *local_hash;

    rc = transport_push_refs(url, &upd, 1, pack, pack_size);
    free(pack);
    ref_ad_free(&ad);
    if (rc != 0) return -1;

    /* 6. 输出结果（与本地路径输出风格一致） */
    char old_hex[HASH_HEX_SIZE] = {0}, new_hex[HASH_HEX_SIZE];
    hash_to_hex(&old_hash, old_hex);
    hash_to_hex(local_hash, new_hex);
    printf("To %s\n", url);
    if (hash_is_zero(&old_hash)) {
        printf(" * [new branch]      %s -> %s\n", branch, branch);
    } else {
        printf("   %.7s..%.7s  %s -> %s\n", old_hex, new_hex,
               branch, branch);
    }
    return 0;
}

static int push_run(int argc, char **argv) {
    const char *remote_name = "origin";
    const char *branch = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (i == 1) {
            remote_name = argv[i];
        } else if (!branch) {
            branch = argv[i];
        }
    }

    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 默认分支：HEAD 指向的分支 */
    char branch_buf[256];
    if (!branch) {
        if (ref_get_head_branch(refs, branch_buf, sizeof(branch_buf)) != 0) {
            mgit_error("not on any branch");
            ref_manager_close(refs);
            return -1;
        }
        branch = branch_buf;
        if (strncmp(branch, "refs/heads/", 11) == 0) branch += 11;
    }

    /* 解析本地分支 */
    Hash local_hash;
    char ref_name[512];
    snprintf(ref_name, sizeof(ref_name), "refs/heads/%s", branch);
    if (ref_resolve_quiet(refs, ref_name, &local_hash) != 0) {
        mgit_error("src refspec '%s' does not match any branch", branch);
        ref_manager_close(refs);
        return -1;
    }

    /* 读取 remote 路径 */
    char remote_path[512];
    if (remote_config_get(".git", remote_name, remote_path, sizeof(remote_path)) != 0) {
        mgit_error("remote '%s' not found (use: mgit remote add)", remote_name);
        ref_manager_close(refs);
        return -1;
    }

    /* 网络 URL：走 Smart HTTP 协议 */
    if (is_url(remote_path)) {
        int rc = push_to_url(remote_path, ref_name, branch, &local_hash);
        ref_manager_close(refs);
        return rc;
    }

    char remote_git[1024];
    snprintf(remote_git, sizeof(remote_git), "%s/.git", remote_path);
    if (!file_is_dir(remote_git)) {
        mgit_error("'%s' is not a git repository", remote_path);
        ref_manager_close(refs);
        return -1;
    }

    RefManager *remote_refs = ref_manager_open(remote_git);
    if (!remote_refs) {
        mgit_error("cannot open remote repository");
        ref_manager_close(refs);
        return -1;
    }

    /* 快进检查 */
    Hash remote_hash;
    char old_hex[HASH_HEX_SIZE] = {0};
    int is_new = 1;
    if (ref_resolve_quiet(remote_refs, ref_name, &remote_hash) == 0) {
        is_new = 0;
        hash_to_hex(&remote_hash, old_hex);
        if (!hash_equal(&remote_hash, &local_hash)) {
            ObjectStore *local_store = object_store_open(".git");
            int ff = local_store &&
                     commit_is_ancestor(local_store, &remote_hash, &local_hash);
            if (local_store) object_store_close(local_store);
            if (!ff) {
                mgit_error("failed to push: non-fast-forward "
                           "(fetch and merge first)");
                ref_manager_close(remote_refs);
                ref_manager_close(refs);
                return -1;
            }
        }
    }

    /* 传输对象 */
    if (remote_send_reachable(".git", remote_git, &local_hash) != 0) {
        mgit_error("failed to transfer objects");
        ref_manager_close(remote_refs);
        ref_manager_close(refs);
        return -1;
    }

    /* 更新远程分支 */
    if (ref_update(remote_refs, ref_name, &local_hash) != 0) {
        mgit_error("failed to update remote ref");
        ref_manager_close(remote_refs);
        ref_manager_close(refs);
        return -1;
    }

    /* 输出结果 */
    char new_hex[HASH_HEX_SIZE];
    hash_to_hex(&local_hash, new_hex);
    printf("To %s\n", remote_path);
    if (is_new) {
        printf(" * [new branch]      %s -> %s\n", branch, branch);
    } else {
        printf("   %.7s..%.7s  %s -> %s\n", old_hex, new_hex, branch, branch);
    }

    ref_manager_close(remote_refs);
    ref_manager_close(refs);
    return 0;
}

Command cmd_push = {
    .name = "push",
    .description = "Push commits to a remote repository",
    .run = push_run,
    .help = push_help
};
