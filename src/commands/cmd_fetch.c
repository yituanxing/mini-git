#include "../command.h"
#include "../core/object.h"
#include "../core/commit.h"
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

/*
 * mgit fetch [<remote>]
 *
 * 从远程仓库下载对象，并更新本地的远程跟踪分支
 * （refs/remotes/<remote>/<branch>）
 *
 * - 本地路径：直接复制可达对象
 * - http(s) URL：Smart HTTP 协商式拉取（want/have，只传差集）
 *
 * 注意：fetch 不会修改工作区，也不会合并到当前分支。
 */

static void fetch_help(void) {
    printf("usage: mgit fetch [<remote>]\n\n");
    printf("Download objects and refs from another repository.\n");
    printf("Supports local paths and http(s) URLs.\n");
    printf("Defaults: remote 'origin'.\n");
}

/* 判断是否为网络 URL */
static int is_url(const char *s) {
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

/*
 * 收集本地已知提交（协商用的 have 列表）
 * 种子 = 所有本地分支 + HEAD，沿双亲回溯历史；
 * 服务器拿到 have 后能算出共同祖先，只回传差集对象
 */
static size_t collect_local_haves(ObjectStore *store, RefManager *refs,
                                  Hash *out, size_t max) {
    Hash queue_[1024];
    size_t head_ = 0, tail = 0, count = 0;

    char branches[64][256];
    int nb = ref_list_branches(refs, branches, 64);
    /*
     * haves are negotiation hints, not the fetched ref set.  If there are
     * more local branches than this small teaching sample, using the first
     * 64 can only make the server send more objects; it cannot make refs
     * point at missing objects.
     */
    if (nb == -2) nb = 64;
    for (int i = 0; i < nb && tail < 1024; i++) {
        Hash h;
        if (ref_resolve_quiet(refs, branches[i], &h) == 0)
            queue_[tail++] = h;
    }
    Hash h;
    if (ref_resolve_head_quiet(refs, &h) == 0 && tail < 1024)
        queue_[tail++] = h;

    while (head_ < tail && count < max) {
        Hash cur = queue_[head_++];

        int dup = 0;   /* 线性去重（上限 1024，开销可接受） */
        for (size_t i = 0; i < count; i++) {
            if (hash_equal(&out[i], &cur)) { dup = 1; break; }
        }
        if (dup) continue;
        out[count++] = cur;

        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &cur, &obj) != 0) continue;
        if (obj.type != OBJ_COMMIT) { object_free(&obj); continue; }

        Commit c;
        memset(&c, 0, sizeof(c));
        commit_parse(obj.data, obj.size, &c);
        object_free(&obj);
        for (int i = 0; i < c.parent_count && tail < 1024; i++)
            queue_[tail++] = c.parents[i];
        commit_free(&c);
    }
    return count;
}

/*
 * 网络拉取：Git Smart HTTP 协商式 fetch
 *
 * 1. GET info/refs -> 引用广告
 * 2. 计算 wants（远端尖端与本地跟踪分支/标签不一致的）
 * 3. POST git-upload-pack：want 尖端 + have 本地已有 -> 差集 pack
 * 4. 解包，更新 refs/remotes/<remote>/<branch> 与标签，写 FETCH_HEAD
 */
static int fetch_from_url(const char *url, const char *remote_name) {
    int rc = -1;
    uint8_t *pack = NULL;
    Hash *wants = NULL;

    /* 1. 引用广告 */
    RefAd ad;
    if (transport_get_refs(url, &ad) != 0) return -1;

    RefManager *refs = ref_manager_open(".git");
    ObjectStore *store = object_store_open(".git");
    if (!refs || !store) {
        mgit_error("cannot open local repository");
        goto cleanup;
    }

    /* 2. wants：远端尖端与本地对应引用不一致的 */
    if (ad.count > 0) {
        wants = (Hash *)malloc(ad.count * sizeof(Hash));
        if (!wants) {
            mgit_error("out of memory while collecting fetch wants");
            goto cleanup;
        }
    }
    size_t want_count = 0;
    for (size_t i = 0; i < ad.count; i++) {
        RemoteRef *rr = &ad.refs[i];
        char local_ref[512];
        if (strncmp(rr->name, "refs/heads/", 11) == 0) {
            snprintf(local_ref, sizeof(local_ref), "refs/remotes/%s/%s",
                     remote_name, rr->name + 11);
        } else if (strncmp(rr->name, "refs/tags/", 10) == 0) {
            snprintf(local_ref, sizeof(local_ref), "%s", rr->name);
        } else {
            continue;
        }
        Hash old;
        if (ref_resolve_quiet(refs, local_ref, &old) != 0 ||
            !hash_equal(&old, &rr->hash)) {
            wants[want_count++] = rr->hash;
        }
    }

    /* 3. haves：本地已知提交 */
    Hash haves[1024];
    size_t have_count = collect_local_haves(store, refs, haves, 1024);

    /* 4. 协商拉取差集 pack */
    size_t pack_size = 0;
    if (transport_fetch_pack_negotiate(url, wants, want_count,
                                       haves, have_count,
                                       &pack, &pack_size) != 0) {
        goto cleanup;
    }

    if (pack_size > 0) {
        size_t nobj = 0;
        if (pack_unpack(store, pack, pack_size, &nobj) != 0) goto cleanup;
        printf("Receiving objects: %lu, done.\n", (unsigned long)nobj);
    }

    /* 5. 更新引用 */
    printf("From %s\n", url);
    char track_dir[1024];
    snprintf(track_dir, sizeof(track_dir), ".git/refs/remotes/%s", remote_name);
    file_mkdir_p(track_dir);

    FILE *fh = fopen(".git/FETCH_HEAD", "w");
    int head_written = 0;

    for (size_t i = 0; i < ad.count; i++) {
        RemoteRef *rr = &ad.refs[i];

        if (strncmp(rr->name, "refs/heads/", 11) == 0) {
            const char *branch = rr->name + 11;
            char tracking[512];
            snprintf(tracking, sizeof(tracking), "refs/remotes/%s/%s",
                     remote_name, branch);

            Hash old;
            int is_new = (ref_resolve_quiet(refs, tracking, &old) != 0);
            if (ref_update(refs, tracking, &rr->hash) != 0) continue;

            if (is_new) {
                printf(" * [new branch]      %-15s -> %s/%s\n",
                       branch, remote_name, branch);
            } else if (!hash_equal(&old, &rr->hash)) {
                char oh[HASH_HEX_SIZE], nh[HASH_HEX_SIZE];
                hash_to_hex(&old, oh);
                hash_to_hex(&rr->hash, nh);
                printf("   %.7s..%.7s  %-15s -> %s/%s\n",
                       oh, nh, branch, remote_name, branch);
            }

            /* 教学简化：FETCH_HEAD 只记录本次广告里的第一个分支。 */
            if (fh && !head_written) {
                char hex[HASH_HEX_SIZE];
                hash_to_hex(&rr->hash, hex);
                fprintf(fh, "%s\t\tbranch '%s' of %s\n", hex, branch, url);
                head_written = 1;
            }
        } else if (strncmp(rr->name, "refs/tags/", 10) == 0) {
            Hash old;
            if (ref_resolve_quiet(refs, rr->name, &old) == 0 &&
                hash_equal(&old, &rr->hash)) continue;
            ref_update(refs, rr->name, &rr->hash);
            printf(" * [new tag]         %-15s -> %s\n",
                   rr->name + 10, rr->name + 10);
        }
    }
    if (fh) fclose(fh);

    rc = 0;

cleanup:
    free(wants);
    free(pack);
    if (store) object_store_close(store);
    if (refs) ref_manager_close(refs);
    ref_ad_free(&ad);
    return rc;
}

static int fetch_run(int argc, char **argv) {
    const char *remote_name = "origin";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            remote_name = argv[i];
            break;
        }
    }

    /* 本地仓库 */
    if (!file_is_dir(".git")) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 读取 remote 路径 */
    char remote_path[512];
    if (remote_config_get(".git", remote_name, remote_path, sizeof(remote_path)) != 0) {
        mgit_error("remote '%s' not found (use: mgit remote add)", remote_name);
        return -1;
    }

    /* 网络 URL：Smart HTTP 协商式拉取 */
    if (is_url(remote_path)) {
        return fetch_from_url(remote_path, remote_name);
    }

    char remote_git[1024];
    snprintf(remote_git, sizeof(remote_git), "%s/.git", remote_path);
    if (!file_is_dir(remote_git)) {
        mgit_error("'%s' is not a git repository", remote_path);
        return -1;
    }

    RefManager *remote_refs = ref_manager_open(remote_git);
    if (!remote_refs) {
        mgit_error("cannot open remote repository");
        return -1;
    }
    RefManager *local_refs = ref_manager_open(".git");
    if (!local_refs) {
        ref_manager_close(remote_refs);
        return -1;
    }

    /* 遍历远程的所有分支 */
    char branches[64][256];
    int count = ref_list_branches(remote_refs, branches, 64);
    if (count == -2) {
        mgit_error("remote has too many branches for local-path fetch (limit 64)");
        ref_manager_close(local_refs);
        ref_manager_close(remote_refs);
        return -1;
    }

    printf("From %s\n", remote_path);
    int fetched = 0;

    for (int i = 0; i < count; i++) {
        const char *ref_name = branches[i];              /* refs/heads/xxx */
        const char *branch = ref_name + 11;               /* xxx */

        Hash hash;
        if (ref_resolve_quiet(remote_refs, ref_name, &hash) != 0) continue;

        /* 传输对象到本地 */
        if (remote_send_reachable(remote_git, ".git", &hash) != 0) {
            mgit_error("failed to fetch objects for branch '%s'", branch);
            continue;
        }

        /* 更新本地远程跟踪分支 refs/remotes/<remote>/<branch> */
        char tracking_dir[1024];
        snprintf(tracking_dir, sizeof(tracking_dir),
                 ".git/refs/remotes/%s", remote_name);
        file_mkdir_p(tracking_dir);

        char tracking_ref[1024];
        snprintf(tracking_ref, sizeof(tracking_ref),
                 "refs/remotes/%s/%s", remote_name, branch);

        Hash old_hash;
        int is_new = (ref_resolve_quiet(local_refs, tracking_ref, &old_hash) != 0);

        if (ref_update(local_refs, tracking_ref, &hash) != 0) {
            mgit_error("failed to update %s", tracking_ref);
            continue;
        }

        if (is_new) {
            printf(" * [new branch]      %-15s -> %s/%s\n",
                   branch, remote_name, branch);
        } else if (!hash_equal(&old_hash, &hash)) {
            char oh[HASH_HEX_SIZE], nh[HASH_HEX_SIZE];
            hash_to_hex(&old_hash, oh);
            hash_to_hex(&hash, nh);
            printf("   %.7s..%.7s  %-15s -> %s/%s\n",
                   oh, nh, branch, remote_name, branch);
        }
        fetched++;
    }

    if (fetched == 0) {
        printf(" (no branches)\n");
    }

    ref_manager_close(local_refs);
    ref_manager_close(remote_refs);
    return 0;
}

Command cmd_fetch = {
    .name = "fetch",
    .description = "Download objects and refs from a remote",
    .run = fetch_run,
    .help = fetch_help
};
