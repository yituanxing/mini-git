#ifndef MGIT_PACK_INDEX_H
#define MGIT_PACK_INDEX_H

#include <stddef.h>
#include <stdint.h>
#include "object.h"

/*
 * 解包：把收到的 packfile 展开为松散对象
 *
 * 等价于真实 git 的 "unpack-objects"：
 * 顺序扫描 pack 中的每个对象，解开 OFS_DELTA / REF_DELTA，
 * 计算完整哈希后写入本地对象存储（.git/objects/xx/yyy...）。
 *
 * 之所以展开而不直接保留 pack：
 * - 服务端 pack 使用 delta 压缩，直接保存需要同时生成 .idx，
 *   而展开为松散对象可以完全复用已有的对象写入/读取路径
 * - 仓库变大后可用 mgit gc 重新打包（教学上分两步更清晰）
 *
 * @param store      目标对象存储
 * @param pack       pack 原始字节
 * @param pack_size  长度
 * @param unpacked   输出：解出的对象数（可为 NULL）
 * @return 0 成功，-1 失败
 */
int pack_unpack(ObjectStore *store, const uint8_t *pack, size_t pack_size,
                size_t *unpacked);

#endif /* MGIT_PACK_INDEX_H */
