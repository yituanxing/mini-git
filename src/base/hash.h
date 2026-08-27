#ifndef MGIT_HASH_H
#define MGIT_HASH_H

#include <stdint.h>
#include <stddef.h>

/* SHA-1 产生 160 位 (20 字节) 的哈希值 */
#define HASH_SIZE 20
#define HASH_HEX_SIZE 41  /* 40 字符 + 1 终止符 */

/* 哈希值类型 */
typedef struct {
    uint8_t bytes[HASH_SIZE];
} Hash;

/* SHA-1 上下文，用于增量哈希计算 */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} SHA1_CTX;

/* SHA-1 函数 */
void sha1_init(SHA1_CTX *ctx);
void sha1_update(SHA1_CTX *ctx, const uint8_t *data, size_t len);
void sha1_final(SHA1_CTX *ctx, Hash *out);

/* 便捷函数：一次性计算数据的哈希 */
void hash_data(const uint8_t *data, size_t len, Hash *out);

/* 哈希值转十六进制字符串 */
void hash_to_hex(const Hash *hash, char *out);

/* 十六进制字符串转哈希值 */
int hex_to_hash(const char *hex, Hash *out);

/* 比较两个哈希值 */
int hash_equal(const Hash *a, const Hash *b);

/* 检查哈希是否为零值 */
int hash_is_zero(const Hash *hash);

#endif /* MGIT_HASH_H */
