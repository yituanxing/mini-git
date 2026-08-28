#ifndef MGIT_PACK_H
#define MGIT_PACK_H

#include "object.h"
#include "../base/hash.h"
#include <stddef.h>

/*
 * Packfile 打包存储
 *
 * Git 的两种对象存储形式：
 * - 松散对象：一个对象一个文件（.git/objects/xx/yyyy...）
 * - 打包存储：大量对象合并进一个 .pack 文件，配一个 .idx 索引
 *
 * gc（垃圾回收）的核心动作就是把松散对象打包成 packfile，
 * 既节省空间（zlib + 未来的 delta），又清理不可达对象。
 *
 * 文件格式（与真实 Git 完全兼容）：
 *
 * .pack（v2）：
 *   "PACK" | 版本(4B BE,=2) | 对象数(4B BE)
 *   对象序列：每个对象 = 变长头(类型3bit+大小) + zlib 数据
 *   尾部 20 字节：整个 pack 内容的 SHA-1
 *
 * .idx（v2）：
 *   魔数 \377tOc | 版本(4B BE,=2)
 *   fanout[256]：哈希首字节 <= i 的对象累计数（4B BE）
 *   哈希表：按哈希排序的 20B SHA-1 序列
 *   CRC32 表：每个对象在 pack 中原始字节的 CRC32（4B BE）
 *   偏移表：每个对象在 pack 中的偏移（4B BE；超 2GB 用大偏移表）
 *   pack 校验和(20B) | idx 自身校验和(20B)
 */

/*
 * 把一组对象写入新的 packfile（含 idx）
 *
 * @param store        对象存储（从中读取对象内容）
 * @param hashes       要打包的对象哈希数组（内部会排序去重）
 * @param count        哈希数量
 * @param packed_out   输出：实际打包的对象数
 * @param name_out     输出：pack 基础名（如 "pack-abc123..."，不含扩展名）
 * @param name_size    name_out 缓冲区大小
 * @return             0 成功，-1 失败
 */
int pack_write(ObjectStore *store, Hash *hashes, size_t count,
               size_t *packed_out, char *name_out, size_t name_size);

/*
 * 构建 pack 到内存（push 网络发送用）
 *
 * @param store       对象存储（从中读取对象内容）
 * @param hashes      要打包的对象哈希数组（内部会排序去重）
 * @param count       哈希数量
 * @param packed_out  输出：实际打包的对象数
 * @param pack_out    输出：pack 原始字节（malloc，调用方 free）
 * @param pack_size   输出：长度
 * @return            0 成功，-1 失败
 */
int pack_build_memory(ObjectStore *store, Hash *hashes, size_t count,
                      size_t *packed_out, uint8_t **pack_out,
                      size_t *pack_size);

/*
 * 从 .git/objects/pack/ 下的 packfile 中读取对象
 * 支持完整对象与 OFS_DELTA / REF_DELTA（可解开 git gc 生成的 pack）
 *
 * @return 0 成功，-1 未找到或解码失败
 */
int pack_read_object(ObjectStore *store, const Hash *hash, Object *obj);

/* 检查 pack 中是否包含某对象（只查 idx，不解压） */
int pack_contains(ObjectStore *store, const Hash *hash);

/*
 * 应用 git delta 指令（解包器 pack_unpack 复用）
 *
 * delta 指令流：复制（高位 1）/ 插入字面数据（高位 0）
 * @param base      基对象内容
 * @param base_len  基对象长度
 * @param delta     delta 指令流（含两个变长尺寸头）
 * @param delta_len 指令流长度
 * @param out       输出：重建后的对象内容（malloc，调用方 free）
 * @param out_len   输出：长度
 * @return 0 成功，-1 失败
 */
int pack_apply_delta(const uint8_t *base, size_t base_len,
                     const uint8_t *delta, size_t delta_len,
                     uint8_t **out, size_t *out_len);

/*
 * 按十六进制前缀在 pack 中查找对象（短哈希支持）
 * @param prefix  十六进制前缀
 * @param plen    前缀长度
 * @param out     输出：唯一匹配的完整哈希
 * @return        0 唯一命中，-1 未找到，-2 前缀歧义
 */
int pack_find_by_prefix(ObjectStore *store, const char *prefix,
                        size_t plen, Hash *out);

/*
 * 统计 pack 信息（count-objects 用）
 * @param num_packs    输出：pack 文件数
 * @param num_objects  输出：pack 中的对象总数
 * @param total_bytes  输出：所有 .pack 文件的字节总数
 */
int pack_stats(ObjectStore *store, size_t *num_packs,
               size_t *num_objects, size_t *total_bytes);

#endif /* MGIT_PACK_H */
