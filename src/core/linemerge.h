#ifndef MGIT_LINEMERGE_H
#define MGIT_LINEMERGE_H

#include <stddef.h>

/*
 * 行级三路内容合并（LCS diff + chunk 合并）
 *
 * 对同一文件的 base / ours / theirs 三个版本逐行合并：
 * - 只有一方修改 → 取该方
 * - 两方修改相同 → 取一份
 * - 两方修改不同 → 输出 <<<<<<< / ======= / >>>>>>> 冲突标记
 *
 * 相比整文件冲突，能自动合并不同区域的修改。
 */

/*
 * 执行三路行级合并
 *
 * @param out_data    输出合并结果（malloc 分配，调用方 free）
 * @param out_size    输出结果长度
 * @param conflicts   输出冲突块数量
 * @return 0 成功，-1 失败
 */
int linemerge_3way(const char *base_data, size_t base_size,
                   const char *ours_data, size_t ours_size,
                   const char *theirs_data, size_t theirs_size,
                   const char *ours_label, const char *theirs_label,
                   char **out_data, size_t *out_size, int *conflicts);

#endif /* MGIT_LINEMERGE_H */
