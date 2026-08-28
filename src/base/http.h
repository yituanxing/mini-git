#ifndef MGIT_HTTP_H
#define MGIT_HTTP_H

#include <stddef.h>
#include <stdint.h>

/*
 * 极简 HTTP 客户端接口。
 *
 * 只覆盖 Git Smart HTTP 需要的能力：
 * - GET / POST（二进制请求与响应）
 * - 自动跟随重定向
 * - URL 内嵌 user:pass 时走 Basic 认证
 *
 * 平台后端由构建系统选择：
 * - Windows: WinHTTP（系统自带 TLS）
 * - Linux:   libcurl
 *
 * transport/core 层只依赖这份接口，不感知具体 HTTP 实现。
 */

typedef struct {
    int status;         /* HTTP 状态码，如 200；0 表示网络层失败 */
    uint8_t *body;      /* 响应体（malloc，调用方 free） */
    size_t size;        /* 响应体长度 */
} HttpResponse;

void http_response_free(HttpResponse *resp);

/*
 * GET 请求
 * @param url        完整 URL（http:// 或 https://）
 * @param accept     Accept 头（可为 NULL）
 * @param resp       输出响应
 * @return 0 网络层成功（HTTP 状态码在 resp->status），-1 网络失败
 */
int http_get(const char *url, const char *accept, HttpResponse *resp);

/*
 * POST 请求
 * @param url          完整 URL
 * @param content_type Content-Type 头
 * @param accept       Accept 头（可为 NULL）
 * @param body         请求体
 * @param size         请求体长度
 * @param resp         输出响应
 * @return 0 网络层成功，-1 网络失败
 */
int http_post(const char *url, const char *content_type, const char *accept,
              const uint8_t *body, size_t size, HttpResponse *resp);

#endif /* MGIT_HTTP_H */
