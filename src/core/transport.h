#ifndef MGIT_TRANSPORT_H
#define MGIT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "../base/hash.h"

/*
 * Git Smart HTTP 协议（protocol v0）客户端
 *
 * 一次完整 clone 的协议交互：
 * 1. GET  <url>/info/refs?service=git-upload-pack
 *    -> 引用广告（refs advertisement），含所有分支/标签 + HEAD 指向
 * 2. POST <url>/git-upload-pack
 *    <- want 列表（协议 v0，无 have = 全量克隆）
 *    -> NAK + side-band 封装的 packfile
 *
 * pkt-line 帧格式：4 位十六进制长度（含自身 4 字节），"0000" 为 flush。
 */

typedef struct {
    Hash hash;          /* 引用指向的提交/标签对象 */
    char name[256];     /* 如 refs/heads/master */
    Hash peeled;        /* ^{} 标注的标签解引用目标（has_peeled 为真时有效） */
    int has_peeled;
} RemoteRef;

typedef struct {
    RemoteRef *refs;
    size_t count;
    char head_branch[256];   /* 服务端 HEAD 指向的分支名（不含 refs/heads/） */
} RefAd;

void ref_ad_free(RefAd *ad);

/*
 * 拉取并解析引用广告（指定服务）
 * @param repo_url  仓库 URL（可含或不含 .git 后缀）
 * @param service   "git-upload-pack"（拉取）或 "git-receive-pack"（推送）
 * @param ad        输出
 * @return 0 成功，-1 失败
 */
int transport_get_refs_service(const char *repo_url, const char *service,
                               RefAd *ad);

/*
 * 拉取并解析引用广告（upload-pack，即 clone/fetch 用）
 * @param repo_url  仓库 URL（可含或不含 .git 后缀）
 * @param ad        输出
 * @return 0 成功，-1 失败
 */
int transport_get_refs(const char *repo_url, RefAd *ad);

/*
 * 请求全量 pack（want 所有引用，无 have）
 * @param repo_url  仓库 URL
 * @param ad        引用广告（用于构造 want 列表）
 * @param pack_out  输出：pack 原始字节（malloc，调用方 free）
 * @param pack_size 输出：长度
 * @return 0 成功，-1 失败
 */
int transport_fetch_pack(const char *repo_url, const RefAd *ad,
                         uint8_t **pack_out, size_t *pack_size);

/* ---------- 推送（git-receive-pack） ---------- */

/* 一条引用更新指令 */
typedef struct {
    char ref[256];      /* 如 refs/heads/master */
    Hash old_hash;      /* 远端当前值；全零 = 新建引用 */
    Hash new_hash;      /* 推送后的目标值 */
} PushUpdate;

/*
 * 推送引用更新 + pack 数据
 *
 * 请求体（protocol v0）：
 *   pkt "<old> <new> <refname> report-status side-band-64k agent=...\n"
 *   pkt "<old> <new> <refname>\n" ...
 *   flush
 *   裸 pack 字节
 *
 * 响应（report-status）：
 *   pkt "unpack ok\n" / "unpack <原因>\n"
 *   pkt "ok <refname>\n" / "ng <refname> <原因>\n" ...
 *   flush（可能被 side-band 通道帧包裹）
 *
 * @return 0 全部成功，-1 失败（错误已写入 mgit_error）
 */
int transport_push_refs(const char *repo_url, const PushUpdate *updates,
                        size_t count, const uint8_t *pack, size_t pack_size);

/*
 * 协商式拉取（fetch 用）：want 想要的尖端，have 告知本地已有的提交，
 * 服务器只返回差集对象。wants 为空时直接返回 0（无新内容）。
 * 成功时 *pack_out 指向 malloc 的 pack 数据，调用方负责释放；
 * 服务器无新对象时 *pack_size == 0。
 */
int transport_fetch_pack_negotiate(const char *repo_url,
                                   const Hash *wants, size_t want_count,
                                   const Hash *haves, size_t have_count,
                                   uint8_t **pack_out, size_t *pack_size);

#endif /* MGIT_TRANSPORT_H */
