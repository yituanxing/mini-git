#include "zlib_util.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

int zlib_compress(const uint8_t *src, size_t src_len,
                  uint8_t *dst, size_t *dst_len) {
    uLongf out_len = (uLongf)*dst_len;
    int ret = compress(dst, &out_len, src, (uLong)src_len);
    if (ret != Z_OK) {
        return -1;
    }
    *dst_len = (size_t)out_len;
    return 0;
}

int zlib_decompress(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t *dst_len) {
    uLongf out_len = (uLongf)*dst_len;
    int ret = uncompress(dst, &out_len, src, (uLong)src_len);
    if (ret != Z_OK) {
        return -1;
    }
    *dst_len = (size_t)out_len;
    return 0;
}

int zlib_decompress_alloc(const uint8_t *src, size_t src_len,
                          uint8_t **dst, size_t *dst_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    
    int ret = inflateInit(&strm);
    if (ret != Z_OK) {
        return -1;
    }

    /* 初始分配：压缩数据大小的 4 倍，或至少 1024 字节 */
    size_t alloc_size = src_len * 4;
    if (alloc_size < 1024) {
        alloc_size = 1024;
    }
    
    uint8_t *buffer = (uint8_t *)malloc(alloc_size);
    if (!buffer) {
        inflateEnd(&strm);
        return -1;
    }

    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = buffer;
    strm.avail_out = (uInt)alloc_size;

    size_t total_out = 0;

    while (1) {
        ret = inflate(&strm, Z_FINISH);
        
        if (ret == Z_STREAM_END) {
            total_out = strm.total_out;
            break;
        }
        
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            free(buffer);
            inflateEnd(&strm);
            return -1;
        }

        /* 需要更多空间 */
        if (strm.avail_out == 0) {
            size_t new_size = alloc_size * 2;
            uint8_t *new_buffer = (uint8_t *)realloc(buffer, new_size);
            if (!new_buffer) {
                free(buffer);
                inflateEnd(&strm);
                return -1;
            }
            buffer = new_buffer;
            strm.next_out = buffer + alloc_size;
            strm.avail_out = (uInt)alloc_size;
            alloc_size = new_size;
        } else if (ret == Z_BUF_ERROR) {
            /* 输出缓冲未满却 Z_BUF_ERROR：输入耗尽且无进展，数据损坏，避免死循环 */
            free(buffer);
            inflateEnd(&strm);
            return -1;
        }
    }

    inflateEnd(&strm);
    
    *dst = buffer;
    *dst_len = total_out;
    return 0;
}

int zlib_inflate_stream(const uint8_t *src, size_t src_len,
                        uint8_t **dst, size_t *dst_len, size_t *consumed) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    if (inflateInit(&strm) != Z_OK) return -1;

    /*
     * 初始缓冲取小值按需翻倍：解包器会同时持有全部已解对象，
     * 若按剩余 pack 大小预估（×4）会很快耗尽 32 位地址空间
     */
    size_t cap = 65536;
    uint8_t *buffer = (uint8_t *)malloc(cap);
    if (!buffer) {
        inflateEnd(&strm);
        return -1;
    }

    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = buffer;
    strm.avail_out = (uInt)cap;

    int ret;
    for (;;) {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            free(buffer);
            inflateEnd(&strm);
            return -1;
        }
        if (strm.avail_out == 0) {
            /* 输出空间不足，扩容后继续 */
            size_t new_cap = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(buffer, new_cap);
            if (!nb) {
                free(buffer);
                inflateEnd(&strm);
                return -1;
            }
            buffer = nb;
            strm.next_out = buffer + cap;
            strm.avail_out = (uInt)cap;
            cap = new_cap;
        }
        if (strm.avail_in == 0 && ret == Z_OK && strm.avail_out > 0) {
            /* 输入耗尽但流未结束：数据截断 */
            free(buffer);
            inflateEnd(&strm);
            return -1;
        }
    }

    *dst_len = strm.total_out;
    *consumed = strm.total_in;
    inflateEnd(&strm);
    *dst = buffer;
    return 0;
}
