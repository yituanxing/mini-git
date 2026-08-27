#include "http.h"
#include "error.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 基于 WinHTTP 的极简 HTTP 客户端
 *
 * WinHTTP 是 Windows 自带的 HTTP 库，原生支持 HTTPS/TLS，
 * 无需引入第三方依赖（MinGW 链接 -lwinhttp 即可）。
 *
 * 实现要点：
 * - URL 拆成 host / port / path 三段
 * - WinHTTP 使用 UTF-16 字符串，需要 MultiByteToWideChar 转换
 * - 重定向手工跟随（读 Location 头，最多 10 次）
 */

#define MAX_REDIRECTS 10

/* 老版本 MinGW 头文件可能未定义这组常量 */
#ifndef WINHTTP_OPTION_REDIRECT_POLICY_NEVER
#define WINHTTP_OPTION_REDIRECT_POLICY_NEVER 0
#endif

typedef struct {
    char scheme[8];      /* "http" / "https" */
    char user[128];      /* URL 内嵌用户名（可为空） */
    char pass[128];      /* URL 内嵌密码/令牌（可为空） */
    char host[256];
    int port;
    char path[2048];     /* 含 query string，以 / 开头 */
} UrlParts;

/* ---------- URL 解析 ---------- */

static int url_parse(const char *url, UrlParts *out) {
    memset(out, 0, sizeof(*out));

    if (strncmp(url, "https://", 8) == 0) {
        strcpy(out->scheme, "https");
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        strcpy(out->scheme, "http");
        url += 7;
    } else {
        return -1;
    }
    out->port = (strcmp(out->scheme, "https") == 0) ? 443 : 80;

    /* user:pass@ 前缀（可选） */
    const char *at = strchr(url, '@');
    const char *slash = strchr(url, '/');
    if (at && (!slash || at < slash)) {
        const char *colon = memchr(url, ':', (size_t)(at - url));
        size_t ulen = colon ? (size_t)(colon - url) : (size_t)(at - url);
        if (ulen >= sizeof(out->user)) ulen = sizeof(out->user) - 1;
        memcpy(out->user, url, ulen);
        if (colon && colon + 1 < at) {
            size_t plen = (size_t)(at - colon - 1);
            if (plen >= sizeof(out->pass)) plen = sizeof(out->pass) - 1;
            memcpy(out->pass, colon + 1, plen);
        }
        url = at + 1;
    }

    /* host[:port] */
    const char *hend = url;
    while (*hend && *hend != '/') hend++;
    const char *colon = memchr(url, ':', (size_t)(hend - url));
    size_t hlen = colon ? (size_t)(colon - url) : (size_t)(hend - url);
    if (hlen == 0 || hlen >= sizeof(out->host)) return -1;
    memcpy(out->host, url, hlen);
    if (colon && colon + 1 < hend) {
        out->port = atoi(colon + 1);
        if (out->port <= 0) return -1;
    }

    /* path（缺省为 /） */
    if (*hend == 0) {
        strcpy(out->path, "/");
    } else {
        if (strlen(hend) >= sizeof(out->path)) return -1;
        strcpy(out->path, hend);
    }
    return 0;
}

/* ---------- UTF-8 -> UTF-16 ---------- */

static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* ---------- Base64（Basic 认证用） ---------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t len, char *out) {
    size_t i = 0, o = 0;
    while (i + 2 < len) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = in[i] << 16;
        if (i + 1 < len) v |= in[i + 1] << 8;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
}

/* ---------- 响应体读取 ---------- */

static int read_body(HINTERNET hRequest, uint8_t **out, size_t *out_size) {
    size_t cap = 65536, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return -1;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            free(buf);
            return -1;
        }
        if (avail == 0) break;  /* 读完 */

        if (len + avail > cap) {
            while (cap < len + avail) cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
        }
        DWORD got = 0;
        if (!WinHttpReadData(hRequest, buf + len, avail, &got)) {
            free(buf);
            return -1;
        }
        len += got;
    }

    *out = buf;
    *out_size = len;
    return 0;
}

/* ---------- 读单个响应头 ---------- */

static int query_header_str(HINTERNET hRequest, DWORD which,
                            char *out, size_t out_size) {
    DWORD sz = 0;
    WinHttpQueryHeaders(hRequest, which, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (sz == 0) return -1;

    wchar_t *w = (wchar_t *)malloc(sz + sizeof(wchar_t));
    if (!w) return -1;
    if (!WinHttpQueryHeaders(hRequest, which, WINHTTP_HEADER_NAME_BY_INDEX,
                             w, &sz, WINHTTP_NO_HEADER_INDEX)) {
        free(w);
        return -1;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_size,
                                NULL, NULL);
    free(w);
    return (n > 0) ? 0 : -1;
}

/*
 * 从响应头取 Location 并转成绝对 URL
 * 支持两种形式：完整 URL / 以 / 开头的绝对路径
 */
static int resolve_location(const char *cur_url, const UrlParts *parts,
                            const char *location,
                            char *out, size_t out_size) {
    if (strncmp(location, "http://", 7) == 0 ||
        strncmp(location, "https://", 8) == 0) {
        if (strlen(location) >= out_size) return -1;
        strcpy(out, location);
        return 0;
    }
    if (location[0] == '/') {
        int n = snprintf(out, out_size, "%s://%s:%d%s",
                         parts->scheme, parts->host, parts->port, location);
        (void)cur_url;
        return (n > 0 && (size_t)n < out_size) ? 0 : -1;
    }
    return -1;  /* 相对路径重定向：git 服务器不会出现 */
}

/* ---------- 单次请求（不跟重定向） ---------- */

static int do_request(const UrlParts *parts, const char *verb,
                      const char *content_type, const char *accept,
                      const uint8_t *body, size_t body_size,
                      HttpResponse *resp, char *location, size_t loc_size) {
    int rc = -1;
    int step = 0;  /* 失败位置诊断 */
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    /*
     * UA 用 git 风格字符串：部分托管服务（如 Gitee）的 WAF
     * 会拦截不以 git/ 开头的 User-Agent
     */
    hSession = WinHttpOpen(L"git/2.40.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        mgit_error("WinHttpOpen failed");
        return -1;
    }

    wchar_t *whost = to_wide(parts->host);
    wchar_t *wpath = to_wide(parts->path);
    if (!whost || !wpath) { step = 1; goto done; }

    hConnect = WinHttpConnect(hSession, whost, (INTERNET_PORT)parts->port, 0);
    if (!hConnect) { step = 2; goto done; }

    DWORD flags = (strcmp(parts->scheme, "https") == 0)
                      ? WINHTTP_FLAG_SECURE : 0;
    wchar_t *wverb = to_wide(verb);
    if (!wverb) { step = 3; goto done; }
    hRequest = WinHttpOpenRequest(hConnect, wverb, wpath, NULL,
                                  WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    free(wverb);
    if (!hRequest) { step = 4; goto done; }

    /*
     * 关闭 WinHTTP 自动重定向：它默认会把 POST 改写成 GET，
     * 而 git 协议要求重定向后保持原方法与请求体，
     * 由上层 http_request 手工跟随
     */
    DWORD redir_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redir_policy, sizeof(redir_policy));

    /* 请求头 */
    char header[512];
    if (content_type) {
        snprintf(header, sizeof(header), "Content-Type: %s\r\n", content_type);
        wchar_t *wh = to_wide(header);
        if (wh) {
            WinHttpAddRequestHeaders(hRequest, wh, (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);
            free(wh);
        }
    }
    if (accept) {
        snprintf(header, sizeof(header), "Accept: %s\r\n", accept);
        wchar_t *wh = to_wide(header);
        if (wh) {
            WinHttpAddRequestHeaders(hRequest, wh, (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);
            free(wh);
        }
    }
    if (parts->user[0]) {
        char cred[320], b64[400];
        snprintf(cred, sizeof(cred), "%s:%s", parts->user, parts->pass);
        base64_encode((const uint8_t *)cred, strlen(cred), b64);
        snprintf(header, sizeof(header), "Authorization: Basic %s\r\n", b64);
        wchar_t *wh = to_wide(header);
        if (wh) {
            WinHttpAddRequestHeaders(hRequest, wh, (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);
            free(wh);
        }
    }

    /* 发送 + 等待响应 */
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)body, (DWORD)body_size, (DWORD)body_size,
                            0)) {
        step = 5;
        goto done;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { step = 6; goto done; }

    DWORD status = 0, slen = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE |
                                      WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    resp->status = (int)status;

    /* 重定向时先取 Location（关闭句柄后就取不到了） */
    if (location &&
        (status == 301 || status == 302 || status == 303 || status == 307)) {
        query_header_str(hRequest, WINHTTP_QUERY_LOCATION,
                         location, loc_size);
    }

    if (read_body(hRequest, &resp->body, &resp->size) != 0) {
        resp->status = 0;
        step = 7;
        goto done;
    }
    rc = 0;

done:
    if (rc != 0) {
        DWORD le = GetLastError();
        mgit_error("network request failed (step %d, winhttp error %lu)",
                   step, (unsigned long)le);
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    free(whost);
    free(wpath);
    return rc;
}

/* ---------- 对外接口（含重定向跟随） ---------- */

void http_response_free(HttpResponse *resp) {
    free(resp->body);
    resp->body = NULL;
    resp->size = 0;
    resp->status = 0;
}

static int http_request(const char *url, const char *verb,
                        const char *content_type, const char *accept,
                        const uint8_t *body, size_t body_size,
                        HttpResponse *resp) {
    char cur_url[4096];
    snprintf(cur_url, sizeof(cur_url), "%s", url);

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        UrlParts parts;
        if (url_parse(cur_url, &parts) != 0) {
            mgit_error("bad URL: %s", cur_url);
            return -1;
        }

        memset(resp, 0, sizeof(*resp));
        char location[2048] = {0};
        if (do_request(&parts, verb, content_type, accept,
                       body, body_size, resp,
                       location, sizeof(location)) != 0) {
            return -1;
        }

        int s = resp->status;
        if (s == 301 || s == 302 || s == 303 || s == 307) {
            /* 重定向：跟随 Location（服务端可能改写 host） */
            char next[4096];
            int ok = (location[0] != 0) &&
                     resolve_location(cur_url, &parts, location,
                                      next, sizeof(next)) == 0;
            http_response_free(resp);
            if (!ok) {
                mgit_error("redirect without valid Location header");
                return -1;
            }
            snprintf(cur_url, sizeof(cur_url), "%s", next);
            continue;
        }
        return 0;
    }
    mgit_error("too many redirects");
    return -1;
}

int http_get(const char *url, const char *accept, HttpResponse *resp) {
    return http_request(url, "GET", NULL, accept, NULL, 0, resp);
}

int http_post(const char *url, const char *content_type, const char *accept,
              const uint8_t *body, size_t size, HttpResponse *resp) {
    /* 303 会把 POST 改成 GET，git 服务器不会这样用，这里一视同仁 */
    return http_request(url, "POST", content_type, accept, body, size, resp);
}
