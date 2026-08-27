#ifndef MGIT_ZLIB_UTIL_H
#define MGIT_ZLIB_UTIL_H

#include <stddef.h>
#include <stdint.h>

/*
 * zlib 压缩/解压工具函数
 * Git 使用 zlib 压缩对象数据
 */

/*
 * 压缩数据
 * @param src       源数据
 * @param src_len   源数据长度
 * @param dst       输出缓冲区（调用者分配）
 * @param dst_len   输入：缓冲区大小；输出：压缩后大小
 * @return          0 成功，-1 失败
 */
int zlib_compress(const uint8_t *src, size_t src_len,
                  uint8_t *dst, size_t *dst_len);

/*
 * 解压数据
 * @param src       压缩数据
 * @param src_len   压缩数据长度
 * @param dst       输出缓冲区（调用者分配）
 * @param dst_len   输入：缓冲区大小；输出：解压后大小
 * @return          0 成功，-1 失败
 */
int zlib_decompress(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t *dst_len);

/*
 * 从文件解压数据，自动分配内存
 * @param src       压缩数据
 * @param src_len   压缩数据长度
 * @param dst       输出：解压后的数据（调用者负责释放）
 * @param dst_len   输出：解压后数据长度
 * @return          0 成功，-1 失败
 */
int zlib_decompress_alloc(const uint8_t *src, size_t src_len,
                          uint8_t **dst, size_t *dst_len);

/*
 * 流式解压：解出一个完整 zlib 流，并报告消耗的输入字节数
 *
 * 与 zlib_decompress_alloc 的区别：额外输出 consumed，
 * 用于 packfile 顺序扫描（解完一个对象后推进到下一个）。
 *
 * @param consumed  输出：实际消耗的压缩字节数
 */
int zlib_inflate_stream(const uint8_t *src, size_t src_len,
                        uint8_t **dst, size_t *dst_len, size_t *consumed);

#endif /* MGIT_ZLIB_UTIL_H */
