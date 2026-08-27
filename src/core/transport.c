#include "transport.h"
#include "../base/http.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/*
 * Smart HTTP 协议实现（protocol v0）
 *
 * pkt-line 是 git 传输协议的基本帧：
 *   - 普通帧: 4 个十六进制数字（帧总长，含这 4 字节）+ 负载
 *   - flush 帧: "0000"
 *
 * 引用广告（GET info/refs）响应示例：
 *   001e# service=git-upload-pack
 *   0000
 *   00d1<hash> HEAD\0multi_ack side-band-64k ofs-delta agent=...
 *   003f<hash> refs/heads/master
 *   0035<hash> refs/tags/v1.0^{}
 *   0000
 */

#define PKT_FLUSH  ((size_t)-1)

void ref_ad_free(RefAd *ad) {
    free(ad->refs);
    ad->refs = NULL;
    ad->count = 0;
}

/* ---------- pkt-line 流读取 ---------- */

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
} PktReader;

/*
 * 读一帧。payload 指向 buf 内部（原始字节，不剥任何内容），
 * *len 为负载长度。文本帧的行尾 \n 由调用方自行处理
 * （二进制通道帧不能剥）。
 * 返回: 0 普通帧 / 1 flush / -1 格式错误或数据截断
 */
static int pkt_read(PktReader *r, const char **payload, size_t *len) {
    if (r->pos + 4 > r->size) return -1;

    char hex[5] = {0};
    memcpy(hex, r->buf + r->pos, 4);
    char *endp;
    unsigned long frame = strtoul(hex, &endp, 16);
    if (endp != hex + 4) return -1;

    if (frame == 0) {           /* flush */
        r->pos += 4;
        *payload = NULL;
        *len = 0;
        return 1;
    }
    if (frame < 4 || r->pos + frame > r->size) return -1;

    size_t plen = frame - 4;
    const char *p = (const char *)r->buf + r->pos + 4;

    r->pos += frame;
    *payload = p;
    *len = plen;
    return 0;
}

/* 文本帧：去掉行尾 \n */
static void strip_nl(const char **payload, size_t *len) {
    if (*len > 0 && (*payload)[*len - 1] == '\n') (*len)--;
}

/* ---------- URL 拼接 ---------- */

/* 去掉尾部 /，再拼路径段（保留 .git 后缀，与真实 git 一致；
 * 去掉会被部分服务器拒绝或重定向） */
static void url_join(char *out, size_t size, const char *repo_url,
                     const char *suffix) {
    size_t len = strlen(repo_url);
    char base[4096];
    if (len >= sizeof(base)) len = sizeof(base) - 1;
    memcpy(base, repo_url, len);
    while (len > 0 && base[len - 1] == '/') len--;
    base[len] = 0;
    snprintf(out, size, "%s/%s", base, suffix);
}

/* ---------- 引用广告 ---------- */

static int parse_hex_hash(const char *hex, Hash *out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        char c1 = hex[i * 2], c2 = hex[i * 2 + 1];
        int v1 = (c1 >= 'a') ? c1 - 'a' + 10 : (c1 >= '0' ? c1 - '0' : -1);
        int v2 = (c2 >= 'a') ? c2 - 'a' + 10 : (c2 >= '0' ? c2 - '0' : -1);
        if (v1 < 0 || v1 > 15 || v2 < 0 || v2 > 15) return -1;
        out->bytes[i] = (uint8_t)((v1 << 4) | v2);
    }
    return 0;
}

/* 解析一行引用记录: "<40-hex> <refname>" */
static int parse_ref_line(const char *line, size_t len, Hash *hash,
                          char *name, size_t name_size) {
    if (len < HASH_SIZE * 2 + 2 || line[HASH_SIZE * 2] != ' ') return -1;
    char hex[HASH_SIZE * 2 + 1];
    memcpy(hex, line, HASH_SIZE * 2);
    hex[HASH_SIZE * 2] = 0;
    if (parse_hex_hash(hex, hash) != 0) return -1;

    size_t nlen = len - HASH_SIZE * 2 - 1;
    if (nlen >= name_size) nlen = name_size - 1;
    memcpy(name, line + HASH_SIZE * 2 + 1, nlen);
    name[nlen] = 0;
    return 0;
}

int transport_get_refs_service(const char *repo_url, const char *service,
                               RefAd *ad) {
    memset(ad, 0, sizeof(*ad));

    char url[4096];
    char suffix[128];
    snprintf(suffix, sizeof(suffix), "info/refs?service=%s", service);
    url_join(url, sizeof(url), repo_url, suffix);

    HttpResponse resp = {0};
    char accept[128];
    snprintf(accept, sizeof(accept), "application/x-%s-advertisement", service);
    if (http_get(url, accept, &resp) != 0) {
        return -1;
    }
    if (resp.status == 401 || resp.status == 403) {
        mgit_error("server returned HTTP %d (auth required; embed "
                   "user:token@ in the URL)", resp.status);
        http_response_free(&resp);
        return -1;
    }
    if (resp.status != 200) {
        mgit_error("server returned HTTP %d for %s", resp.status, url);
        http_response_free(&resp);
        return -1;
    }

    PktReader r = { resp.body, resp.size, 0 };
    const char *payload;
    size_t len;

    /* 首帧: "# service=<服务名>"，随后一个 flush */
    if (pkt_read(&r, &payload, &len) != 0) {
        mgit_error("unexpected refs advertisement");
        goto fail;
    }
    strip_nl(&payload, &len);
    if (len == 0 || payload[0] != '#') {
        mgit_error("unexpected refs advertisement");
        goto fail;
    }
    if (pkt_read(&r, &payload, &len) != 1) {  /* 期望 flush */
        mgit_error("malformed refs advertisement");
        goto fail;
    }

    /* 逐条引用，直到 flush */
    size_t cap = 16;
    ad->refs = (RemoteRef *)malloc(cap * sizeof(RemoteRef));
    if (!ad->refs) goto fail;

    int first = 1;
    for (;;) {
        int rc = pkt_read(&r, &payload, &len);
        if (rc == 1) break;     /* flush：广告结束 */
        if (rc != 0) {
            mgit_error("truncated refs advertisement");
            goto fail;
        }
        strip_nl(&payload, &len);

        /* 首行可能带能力串：老 git 用 \0 分隔，新 git 用空格 */
        const char *line = payload;
        size_t llen = len;
        const char *nul = (const char *)memchr(payload, '\0', len);
        if (nul) llen = (size_t)(nul - payload);
        /* 引用名不含空格：截到名字后的第一个空格（若有） */
        if (llen > HASH_SIZE * 2 + 1) {
            const char *sp = (const char *)memchr(
                payload + HASH_SIZE * 2 + 1, ' ', llen - (HASH_SIZE * 2 + 1));
            if (sp) llen = (size_t)(sp - payload);
        }

        Hash hash;
        char name[256];
        if (parse_ref_line(line, llen, &hash, name, sizeof(name)) != 0) {
            continue;  /* 容错：跳过无法解析的行 */
        }

        int empty = 1;  /* 全零哈希 = 空仓库广告 */
        for (int i = 0; i < HASH_SIZE && empty; i++) {
            if (hash.bytes[i]) empty = 0;
        }
        if (empty) break;

        if (first) first = 0;

        /* 记录服务端 HEAD 指向的分支 */
        if (strcmp(name, "HEAD") == 0) {
            /*
             * symref 能力位置随版本而异：
             * - 老 git：跟在 \0 之后（能力串）
             * - 新 git：HEAD 行直接用空格分隔能力
             * 两种都扫一遍，形如 symref=HEAD:refs/heads/main
             * 注意：只能在本帧负载长度内搜索（payload 后无 \0，
             * 越界 strstr 会扫进后续帧导致误匹配）
             */
            const char *p = NULL;
            const char *hit = payload;
            size_t remain = len;
            while (remain >= strlen("symref=HEAD:")) {
                const char *h = (const char *)memchr(hit, 's', remain);
                if (!h) break;
                remain -= (size_t)(h - hit);
                hit = h;
                if (strncmp(hit, "symref=HEAD:", 12) == 0) {
                    p = hit;
                    break;
                }
                hit++;
                remain--;
            }
            if (p) {
                p += strlen("symref=HEAD:");
                const char *e = p;
                size_t left = len - (size_t)(p - payload);
                while (left > 0 && *e && *e != ' ' && *e != '\n') {
                    e++;
                    left--;
                }
                size_t bl = (size_t)(e - p);
                if (strncmp(p, "refs/heads/", 11) == 0) {
                    p += 11;
                    bl -= 11;
                }
                if (bl > 0 && bl < sizeof(ad->head_branch)) {
                    memcpy(ad->head_branch, p, bl);
                    ad->head_branch[bl] = 0;
                }
            }
            continue;  /* HEAD 本身不作为普通引用保存 */
        }

        if (ad->count >= cap) {
            cap *= 2;
            RemoteRef *nr = (RemoteRef *)realloc(ad->refs,
                                                 cap * sizeof(RemoteRef));
            if (!nr) goto fail;
            ad->refs = nr;
        }

        RemoteRef *rr = &ad->refs[ad->count];
        memset(rr, 0, sizeof(*rr));

        /* 标签解引用行: refs/tags/v1.0^{} 合并到对应标签条目 */
        size_t nlen = strlen(name);
        if (nlen > 3 && strcmp(name + nlen - 3, "^{}") == 0) {
            name[nlen - 3] = 0;
            for (size_t i = 0; i < ad->count; i++) {
                if (strcmp(ad->refs[i].name, name) == 0) {
                    ad->refs[i].peeled = hash;
                    ad->refs[i].has_peeled = 1;
                    break;
                }
            }
            continue;
        }

        rr->hash = hash;
        snprintf(rr->name, sizeof(rr->name), "%s", name);
        ad->count++;
    }

    http_response_free(&resp);
    return 0;

fail:
    http_response_free(&resp);
    ref_ad_free(ad);
    return -1;
}

int transport_get_refs(const char *repo_url, RefAd *ad) {
    return transport_get_refs_service(repo_url, "git-upload-pack", ad);
}

/* ---------- upload-pack（拉取 pack） ---------- */

/* 动态缓冲区 */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_append(Buf *b, const void *data, size_t len) {
    if (b->len + len > b->cap) {
        size_t ncap = b->cap ? b->cap : 65536;
        while (ncap < b->len + len) ncap *= 2;
        uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
        if (!nd) return -1;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static int append_pkt(Buf *b, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line)) return -1;

    char header[5];
    snprintf(header, sizeof(header), "%04x", (unsigned)(n + 4));
    if (buf_append(b, header, 4) != 0) return -1;
    return buf_append(b, line, (size_t)n);
}

/*
 * 构造请求体（协议 v0）
 * 能力串跟在第一个 want 后（空格分隔，与真实 git 客户端一致；
 * git 2.x 的 upload-pack 只认空格分隔）
 * haves 为 NULL/0 时即全量克隆请求；fetch 时告知服务器本地已有的
 * 提交，服务器只返回差集对象
 */
static int build_request(const Hash *wants, size_t want_count,
                         const Hash *haves, size_t have_count, Buf *req) {
    const char *caps = "multi_ack side-band-64k ofs-delta agent=mgit/1.0";

    int first = 1;
    for (size_t i = 0; i < want_count; i++) {
        char hex[HASH_SIZE * 2 + 1];
        const Hash *h = &wants[i];
        for (int j = 0; j < HASH_SIZE; j++) {
            snprintf(hex + j * 2, 3, "%02x", h->bytes[j]);
        }
        if (first) {
            if (append_pkt(req, "want %s %s\n", hex, caps) != 0) return -1;
            first = 0;
        } else {
            if (append_pkt(req, "want %s\n", hex) != 0) return -1;
        }
    }
    if (buf_append(req, "0000", 4) != 0) return -1;
    for (size_t i = 0; i < have_count; i++) {
        char hex[HASH_SIZE * 2 + 1];
        for (int j = 0; j < HASH_SIZE; j++) {
            snprintf(hex + j * 2, 3, "%02x", haves[i].bytes[j]);
        }
        if (append_pkt(req, "have %s\n", hex) != 0) return -1;
    }
    if (append_pkt(req, "done\n") != 0) return -1;
    return 0;
}

/*
 * 解析响应（协议 v0）
 *
 * 结构：
 *   状态行（NAK）与进度信息为普通 pkt-line；
 *   协商了 side-band 时，pack 数据以通道帧混在同一 pkt 流中：
 *     通道 1 = pack 数据，通道 2 = 进度文本，通道 3 = 错误；
 *   整个流以 flush 结束。
 *   未启用 side-band 的老服务器：flush 之后（或 pkt 流断裂处）
 *   直接是裸 pack。
 */
static int parse_response(const uint8_t *buf, size_t size,
                          int allow_empty, uint8_t **pack_out, size_t *pack_size) {
    PktReader r = { buf, size, 0 };
    const char *payload;
    size_t len;
    Buf pack = {0};
    int got_sideband = 0;

    for (;;) {
        int rc = pkt_read(&r, &payload, &len);
        if (rc == 1) break;     /* 收尾 flush */
        if (rc != 0) {
            /*
             * pkt 流在此断开：若还没见过 side-band 帧，
             * 剩余字节大概率是裸 pack（老服务端行为）
             */
            if (!got_sideband && size - r.pos >= 4 &&
                memcmp(buf + r.pos, "PACK", 4) == 0) {
                if (buf_append(&pack, buf + r.pos, size - r.pos) != 0)
                    goto oom;
                break;
            }
            mgit_error("truncated upload-pack response");
            goto fail;
        }

        /* side-band 通道帧：首字节 1/2/3 */
        if (len > 0 && (uint8_t)payload[0] >= 1 && (uint8_t)payload[0] <= 3) {
            got_sideband = 1;
            uint8_t band = (uint8_t)payload[0];
            const char *data = payload + 1;
            size_t dlen = len - 1;
            if (band == 1) {
                if (buf_append(&pack, data, dlen) != 0) goto oom;
            } else if (band == 2) {
                fwrite(data, 1, dlen, stderr);  /* 服务端进度信息 */
            } else {
                mgit_error("server error: %.*s", (int)dlen, data);
                goto fail;
            }
            continue;
        }

        /* 普通状态行：ERR 前缀是致命错误，NAK/进度文本忽略 */
        strip_nl(&payload, &len);
        if (len >= 4 && strncmp(payload, "ERR ", 4) == 0) {
            mgit_error("server error: %.*s", (int)len, payload);
            goto fail;
        }
    }

    if (pack.len == 0) {
        if (allow_empty) {
            /* 服务器无新对象：只有 ACK/NAK + flush */
            *pack_out = NULL;
            *pack_size = 0;
            return 0;
        }
        mgit_error("server sent no pack data");
        goto fail;
    }
    *pack_out = pack.data;
    *pack_size = pack.len;
    return 0;

oom:
    mgit_error("out of memory while receiving pack");
fail:
    free(pack.data);
    return -1;
}

int transport_fetch_pack(const char *repo_url, const RefAd *ad,
                         uint8_t **pack_out, size_t *pack_size) {
    if (ad->count == 0) {
        mgit_error("remote repository is empty");
        return -1;
    }

    /* 哈希内嵌在 RemoteRef 中不连续，先拷成紧凑数组再构造请求 */
    Hash *ws = (Hash *)malloc(ad->count * sizeof(Hash));
    if (!ws) return -1;
    for (size_t i = 0; i < ad->count; i++) ws[i] = ad->refs[i].hash;

    Buf req = {0};
    int br = build_request(ws, ad->count, NULL, 0, &req);
    free(ws);
    if (br != 0) {
        free(req.data);
        return -1;
    }

    char url[4096];
    url_join(url, sizeof(url), repo_url, "git-upload-pack");

    HttpResponse resp = {0};
    int rc = http_post(url,
                       "application/x-git-upload-pack-request",
                       "application/x-git-upload-pack-result",
                       req.data, req.len, &resp);
    free(req.data);
    if (rc != 0) return -1;

    if (resp.status != 200) {
        mgit_error("server returned HTTP %d for git-upload-pack",
                   resp.status);
        http_response_free(&resp);
        return -1;
    }

    /* 请求了 side-band-64k；解析器自动识别通道帧/裸 pack */
    rc = parse_response(resp.body, resp.size, 0, pack_out, pack_size);
    http_response_free(&resp);
    return rc;
}

/*
 * 协商式拉取（fetch 用）：want 想要的尖端 + have 本地已有提交，
 * 服务器只回传差集；wants 为空时直接返回（无新内容）
 */
int transport_fetch_pack_negotiate(const char *repo_url,
                                   const Hash *wants, size_t want_count,
                                   const Hash *haves, size_t have_count,
                                   uint8_t **pack_out, size_t *pack_size) {
    *pack_out = NULL;
    *pack_size = 0;
    if (want_count == 0) return 0;

    Buf req = {0};
    if (build_request(wants, want_count, haves, have_count, &req) != 0) {
        free(req.data);
        return -1;
    }

    char url[4096];
    url_join(url, sizeof(url), repo_url, "git-upload-pack");

    HttpResponse resp = {0};
    int rc = http_post(url,
                       "application/x-git-upload-pack-request",
                       "application/x-git-upload-pack-result",
                       req.data, req.len, &resp);
    free(req.data);
    if (rc != 0) return -1;

    if (resp.status != 200) {
        if (resp.status == 401 || resp.status == 403) {
            mgit_error("server returned HTTP %d (auth required; "
                       "embed user:token@ in the URL)", resp.status);
        } else {
            mgit_error("server returned HTTP %d for git-upload-pack",
                       resp.status);
        }
        http_response_free(&resp);
        return -1;
    }

    /* 无新对象时服务器只回 ACK/NAK，允许空 pack */
    rc = parse_response(resp.body, resp.size, 1, pack_out, pack_size);
    http_response_free(&resp);
    return rc;
}

/* ---------- receive-pack（推送） ---------- */

static void hash_to_hex_str(const Hash *h, char *out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        snprintf(out + i * 2, 3, "%02x", h->bytes[i]);
    }
    out[HASH_SIZE * 2] = 0;
}

/*
 * 构造推送请求体：
 *   引用更新指令（能力串跟在首条）+ flush + 裸 pack
 * 与 upload-pack 不同，请求方向不做 side-band 封装
 */
static int build_push_request(const PushUpdate *updates, size_t count,
                              const uint8_t *pack, size_t pack_size,
                              Buf *req) {
    const char *caps = "report-status side-band-64k agent=mgit/1.0";

    for (size_t i = 0; i < count; i++) {
        char old_hex[HASH_SIZE * 2 + 1], new_hex[HASH_SIZE * 2 + 1];
        hash_to_hex_str(&updates[i].old_hash, old_hex);
        hash_to_hex_str(&updates[i].new_hash, new_hex);
        if (i == 0) {
            /*
             * 首条指令："<old> <new> <refname>\0<能力串>\n"
             * 注意分隔符是 \0（与 upload-pack 的空格不同）；
             * 用空格会被服务端当成引用名的一部分而拒收
             */
            char line[1024];
            int n = snprintf(line, sizeof(line), "%s %s %s", old_hex,
                             new_hex, updates[i].ref);
            if (n < 0 || (size_t)n + strlen(caps) + 2 >= sizeof(line))
                return -1;
            line[n] = '\0';
            memcpy(line + n + 1, caps, strlen(caps));
            size_t plen = (size_t)n + 1 + strlen(caps);
            line[plen] = '\n';
            plen += 1;

            char header[5];
            snprintf(header, sizeof(header), "%04x",
                     (unsigned)(plen + 4));
            if (buf_append(req, header, 4) != 0) return -1;
            if (buf_append(req, line, plen) != 0) return -1;
        } else {
            if (append_pkt(req, "%s %s %s\n",
                           old_hex, new_hex, updates[i].ref) != 0)
                return -1;
        }
    }
    if (buf_append(req, "0000", 4) != 0) return -1;
    if (pack_size > 0 && buf_append(req, pack, pack_size) != 0) return -1;
    return 0;
}

/*
 * 解析 report-status 回执
 *
 * 状态行可能直接以 pkt 帧到达，也可能被 side-band 通道帧包裹
 * （通道 1 = 状态流，2 = 进度，3 = 错误）。统一归集为一个
 * pkt 流后再解析：
 *   "unpack ok" / "unpack <原因>"
 *   "ok <refname>" / "ng <refname> <原因>"
 */
static int parse_push_response(const uint8_t *buf, size_t size) {
    PktReader r = { buf, size, 0 };
    const char *payload;
    size_t len;
    Buf status = {0};

    for (;;) {
        int rc = pkt_read(&r, &payload, &len);
        if (rc == 1) {
            if (buf_append(&status, "0000", 4) != 0) goto oom;
            break;
        }
        if (rc != 0) break;  /* 流在此断开，忽略尾部多余字节 */

        /* side-band 通道帧：首字节 1/2/3（状态行均以字母开头，不冲突） */
        if (len > 0 && (uint8_t)payload[0] >= 1 && (uint8_t)payload[0] <= 3) {
            uint8_t band = (uint8_t)payload[0];
            if (band == 1) {
                if (buf_append(&status, payload + 1, len - 1) != 0) goto oom;
            } else if (band == 2) {
                fwrite(payload + 1, 1, len - 1, stderr);  /* 服务端进度 */
            } else {
                mgit_error("server error: %.*s", (int)(len - 1), payload + 1);
                goto fail;
            }
            continue;
        }

        /* 无 side-band：状态行直接以 pkt 帧到达，原样拼回流中 */
        char hdr[5];
        snprintf(hdr, sizeof(hdr), "%04x", (unsigned)(len + 4));
        if (buf_append(&status, hdr, 4) != 0) goto oom;
        if (buf_append(&status, payload, len) != 0) goto oom;
    }

    /* 解析状态流 */
    PktReader sr = { status.data, status.len, 0 };
    int unpack_ok = 0, any_ng = 0;
    for (;;) {
        int rc = pkt_read(&sr, &payload, &len);
        if (rc != 0) break;
        strip_nl(&payload, &len);

        if (len >= 4 && strncmp(payload, "ERR ", 4) == 0) {
            mgit_error("server error: %.*s", (int)len, payload);
            goto fail;
        }
        if (len >= 7 && strncmp(payload, "unpack ", 7) == 0) {
            if (len - 7 == 2 && strncmp(payload + 7, "ok", 2) == 0) {
                unpack_ok = 1;
            } else {
                mgit_error("unpack failed: %.*s",
                           (int)(len - 7), payload + 7);
                goto fail;
            }
        } else if (len >= 3 && strncmp(payload, "ng ", 3) == 0) {
            any_ng = 1;
            mgit_error("push rejected: %.*s", (int)(len - 3), payload + 3);
        }
        /* "ok <refname>"：成功，不需处理 */
    }

    if (!unpack_ok) {
        mgit_error("server sent no unpack status");
        goto fail;
    }
    if (any_ng) goto fail;
    free(status.data);
    return 0;

oom:
    mgit_error("out of memory while parsing push response");
fail:
    free(status.data);
    return -1;
}

int transport_push_refs(const char *repo_url, const PushUpdate *updates,
                        size_t count, const uint8_t *pack, size_t pack_size) {
    if (count == 0) return 0;

    Buf req = {0};
    if (build_push_request(updates, count, pack, pack_size, &req) != 0) {
        free(req.data);
        mgit_error("out of memory while building push request");
        return -1;
    }

    char url[4096];
    url_join(url, sizeof(url), repo_url, "git-receive-pack");

    HttpResponse resp = {0};
    int rc = http_post(url,
                       "application/x-git-receive-pack-request",
                       "application/x-git-receive-pack-result",
                       req.data, req.len, &resp);
    free(req.data);
    if (rc != 0) return -1;

    if (resp.status == 401 || resp.status == 403) {
        mgit_error("server returned HTTP %d (auth required; embed "
                   "user:token@ in the URL)", resp.status);
        http_response_free(&resp);
        return -1;
    }
    if (resp.status != 200) {
        mgit_error("server returned HTTP %d for git-receive-pack",
                   resp.status);
        http_response_free(&resp);
        return -1;
    }

    rc = parse_push_response(resp.body, resp.size);
    http_response_free(&resp);
    return rc;
}
