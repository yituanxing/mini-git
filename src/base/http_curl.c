#include "http.h"
#include "error.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Linux Smart HTTP backend based on libcurl.
 *
 * Keep this file deliberately small: URL parsing, TLS, redirects and
 * authentication are delegated to libcurl. The Git protocol framing
 * remains in core/transport.c and is shared with Windows.
 */

#define MAX_REDIRECTS 10

typedef struct {
    uint8_t *data;
    size_t size;
} CurlBuffer;

static int ensure_curl(void) {
    static int initialized = 0;
    if (!initialized) {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            mgit_error("curl_global_init failed: %s", curl_easy_strerror(rc));
            return -1;
        }
        initialized = 1;
    }
    return 0;
}

static size_t write_body(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    CurlBuffer *buf = (CurlBuffer *)userdata;

    if (n == 0) return 0;
    if (buf->size > (size_t)-1 - n) return 0;

    uint8_t *p = (uint8_t *)realloc(buf->data, buf->size + n);
    if (!p) return 0;

    memcpy(p + buf->size, ptr, n);
    buf->data = p;
    buf->size += n;
    return n;
}

void http_response_free(HttpResponse *resp) {
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->size = 0;
    resp->status = 0;
}

static int add_header(struct curl_slist **headers,
                      const char *name, const char *value) {
    if (!value) return 0;

    size_t n = strlen(name) + strlen(value) + 3;
    char *line = (char *)malloc(n);
    if (!line) return -1;

    snprintf(line, n, "%s: %s", name, value);
    struct curl_slist *next = curl_slist_append(*headers, line);
    free(line);
    if (!next) return -1;

    *headers = next;
    return 0;
}

static int http_request(const char *url, int is_post,
                        const char *content_type, const char *accept,
                        const uint8_t *body, size_t body_size,
                        HttpResponse *resp) {
    if (!url || !resp) return -1;
    if (ensure_curl() != 0) return -1;

    memset(resp, 0, sizeof(*resp));

    CURL *curl = curl_easy_init();
    if (!curl) {
        mgit_error("curl_easy_init failed");
        return -1;
    }

    int rc = -1;
    char errbuf[CURL_ERROR_SIZE] = {0};
    struct curl_slist *headers = NULL;
    CurlBuffer out = {0};

    if (add_header(&headers, "Content-Type", content_type) != 0 ||
        add_header(&headers, "Accept", accept) != 0) {
        mgit_error("out of memory while building HTTP headers");
        goto done;
    }

    /*
     * Avoid libcurl's optional "Expect: 100-continue" handshake for large
     * POST bodies. Git's CGI-style endpoints do not need it, and disabling
     * it keeps behavior close to the WinHTTP backend.
     */
    if (is_post) {
        struct curl_slist *next = curl_slist_append(headers, "Expect:");
        if (!next) {
            mgit_error("out of memory while building HTTP headers");
            goto done;
        }
        headers = next;
    }

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "git/2.40.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)MAX_REDIRECTS);
    curl_easy_setopt(curl, CURLOPT_POSTREDIR, (long)CURL_REDIR_POST_ALL);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    /*
     * If user:pass@ is embedded in the URL, libcurl extracts the
     * credentials. Restrict authentication to Basic to match WinHTTP.
     */
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);

    if (is_post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)body_size);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode cr = curl_easy_perform(curl);
    if (cr != CURLE_OK) {
        const char *detail = errbuf[0] ? errbuf : curl_easy_strerror(cr);
        mgit_error("network request failed (curl: %s)", detail);
        goto done;
    }

    long status = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
        mgit_error("cannot read HTTP status from libcurl");
        goto done;
    }

    resp->status = (int)status;
    resp->body = out.data;
    resp->size = out.size;
    out.data = NULL;
    out.size = 0;
    rc = 0;

done:
    free(out.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc;
}

int http_get(const char *url, const char *accept, HttpResponse *resp) {
    return http_request(url, 0, NULL, accept, NULL, 0, resp);
}

int http_post(const char *url, const char *content_type, const char *accept,
              const uint8_t *body, size_t size, HttpResponse *resp) {
    return http_request(url, 1, content_type, accept, body, size, resp);
}
