#include "hash.h"
#include <string.h>
#include <stdio.h>

/* SHA-1 常量 */
#define SHA1_BLOCK_SIZE 64

/* 循环左移 */
static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

/* SHA-1 初始值 */
void sha1_init(SHA1_CTX *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

/* SHA-1 块处理 */
static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    uint32_t temp;
    int i;

    /* 将块扩展为 80 个 32 位字 */
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    /* 80 轮迭代 */
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            temp = rotl32(a, 5) + ((b & c) | (~b & d)) + e + w[i] + 0x5A827999;
        } else if (i < 40) {
            temp = rotl32(a, 5) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1;
        } else if (i < 60) {
            temp = rotl32(a, 5) + ((b & c) | (b & d) | (c & d)) + e + w[i] + 0x8F1BBCDC;
        } else {
            temp = rotl32(a, 5) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6;
        }
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

/* 更新 SHA-1 上下文 */
void sha1_update(SHA1_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;
    size_t index = (size_t)(ctx->count & 63);
    
    ctx->count += len;

    /* 如果缓冲区有数据，先填满缓冲区 */
    if (index) {
        size_t remaining = SHA1_BLOCK_SIZE - index;
        if (len < remaining) {
            memcpy(ctx->buffer + index, data, len);
            return;
        }
        memcpy(ctx->buffer + index, data, remaining);
        sha1_transform(ctx->state, ctx->buffer);
        i = remaining;
    }

    /* 处理完整的块 */
    for (; i + SHA1_BLOCK_SIZE <= len; i += SHA1_BLOCK_SIZE) {
        sha1_transform(ctx->state, data + i);
    }

    /* 剩余数据放入缓冲区 */
    if (i < len) {
        memcpy(ctx->buffer, data + i, len - i);
    }
}

/* 完成 SHA-1 计算 */
void sha1_final(SHA1_CTX *ctx, Hash *out) {
    uint64_t bits = ctx->count * 8;
    size_t index = (size_t)(ctx->count & 63);
    
    /* 填充 */
    ctx->buffer[index++] = 0x80;
    
    if (index > 56) {
        memset(ctx->buffer + index, 0, SHA1_BLOCK_SIZE - index);
        sha1_transform(ctx->state, ctx->buffer);
        index = 0;
    }
    
    memset(ctx->buffer + index, 0, 56 - index);
    
    /* 添加长度（大端序） */
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha1_transform(ctx->state, ctx->buffer);

    /* 输出哈希值（大端序） */
    for (int i = 0; i < 5; i++) {
        out->bytes[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out->bytes[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out->bytes[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out->bytes[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* 一次性计算数据的哈希 */
void hash_data(const uint8_t *data, size_t len, Hash *out) {
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

/* 哈希值转十六进制字符串 */
void hash_to_hex(const Hash *hash, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < HASH_SIZE; i++) {
        out[i * 2]     = hex[hash->bytes[i] >> 4];
        out[i * 2 + 1] = hex[hash->bytes[i] & 0x0f];
    }
    out[HASH_HEX_SIZE - 1] = '\0';
}

/* 十六进制字符转数值 */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 十六进制字符串转哈希值 */
int hex_to_hash(const char *hex, Hash *out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out->bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* 比较两个哈希值 */
int hash_equal(const Hash *a, const Hash *b) {
    return memcmp(a->bytes, b->bytes, HASH_SIZE) == 0;
}

/* 检查哈希是否为零值 */
int hash_is_zero(const Hash *hash) {
    for (int i = 0; i < HASH_SIZE; i++) {
        if (hash->bytes[i] != 0) return 0;
    }
    return 1;
}
