#include "linemerge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 行级三路合并实现
 *
 * 步骤：
 * 1. 把三个版本切成行视图（不复制内容）
 * 2. LCS 动态规划计算 base→ours、base→theirs 的匹配锚点
 * 3. 由锚点生成变更块（chunk）：base 中一段被 side 中一段替换
 * 4. 沿 base 坐标双指针扫描两份 chunk 列表：
 *    - 无变更区 → 原样输出 base 行
 *    - 只有一方变更 → 输出该方
 *    - 两方变更内容一致 → 输出一份
 *    - 两方变更不同 → 输出冲突标记块
 */

typedef struct {
    const char *start;
    size_t len;      /* 含行尾 \n（如有） */
} Line;

typedef struct {
    int base_start;  /* base 中被替换区间起点 */
    int base_len;
    int side_start;  /* side 中替换内容起点 */
    int side_len;
} Chunk;

/* ---- 行切分 ---- */

static Line *split_lines(const char *data, size_t size, int *count) {
    int cap = 64, n = 0;
    Line *lines = (Line *)malloc(cap * sizeof(Line));
    if (!lines) return NULL;

    size_t i = 0;
    while (i < size) {
        size_t j = i;
        while (j < size && data[j] != '\n') j++;
        if (j < size) j++;  /* 包含 \n */
        if (n >= cap) {
            cap *= 2;
            Line *nl = (Line *)realloc(lines, cap * sizeof(Line));
            if (!nl) { free(lines); return NULL; }
            lines = nl;
        }
        lines[n].start = data + i;
        lines[n].len = j - i;
        n++;
        i = j;
    }
    *count = n;
    return lines;
}

static int line_equal(const Line *a, const Line *b) {
    return a->len == b->len && (a->len == 0 || memcmp(a->start, b->start, a->len) == 0);
}

/* ---- LCS → 匹配锚点 ---- */

/*
 * 返回匹配对 (base_idx, side_idx) 数组，按升序；*count 为对数。
 * DP 表堆分配，防止大文件栈溢出。规模过大时返回 NULL（调用方降级）。
 */
typedef struct { int bi, si; } Anchor;

static Anchor *lcs_anchors(const Line *base, int m, const Line *side, int n, int *count) {
    *count = 0;
    if ((size_t)m * (size_t)n > (size_t)50 * 1024 * 1024) return NULL;

    int w = n + 1;
    int *dp = (int *)malloc((size_t)(m + 1) * w * sizeof(int));
    if (!dp) return NULL;
    memset(dp, 0, (size_t)(m + 1) * w * sizeof(int));

    for (int i = 1; i <= m; i++) {
        for (int j = 1; j <= n; j++) {
            if (line_equal(&base[i - 1], &side[j - 1])) {
                dp[i * w + j] = dp[(i - 1) * w + (j - 1)] + 1;
            } else {
                int up = dp[(i - 1) * w + j];
                int left = dp[i * w + (j - 1)];
                dp[i * w + j] = (up > left) ? up : left;
            }
        }
    }

    int total = dp[m * w + n];
    Anchor *anchors = (Anchor *)malloc(((size_t)total + 1) * sizeof(Anchor));
    if (!anchors) { free(dp); return NULL; }

    /* 回溯（倒序填入再反转） */
    int i = m, j = n, k = total;
    while (i > 0 && j > 0) {
        if (line_equal(&base[i - 1], &side[j - 1])) {
            k--;
            anchors[k].bi = i - 1;
            anchors[k].si = j - 1;
            i--; j--;
        } else if (dp[(i - 1) * w + j] >= dp[i * w + (j - 1)]) {
            i--;
        } else {
            j--;
        }
    }
    free(dp);
    *count = total;
    return anchors;
}

/* ---- 锚点 → 变更块 ---- */

static Chunk *anchors_to_chunks(const Anchor *anchors, int acount,
                                int m, int n, int *count) {
    Chunk *chunks = (Chunk *)malloc(((size_t)acount + 1) * sizeof(Chunk));
    if (!chunks) return NULL;
    int c = 0;
    int prev_b = 0, prev_s = 0;

    for (int i = 0; i <= acount; i++) {
        int bi = (i < acount) ? anchors[i].bi : m;
        int si = (i < acount) ? anchors[i].si : n;
        int blen = bi - prev_b;
        int slen = si - prev_s;
        if (blen > 0 || slen > 0) {
            chunks[c].base_start = prev_b;
            chunks[c].base_len = blen;
            chunks[c].side_start = prev_s;
            chunks[c].side_len = slen;
            c++;
        }
        prev_b = bi + 1;
        prev_s = si + 1;
    }
    *count = c;
    return chunks;
}

/* ---- 输出缓冲 ---- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} OutBuf;

static int out_reserve(OutBuf *o, size_t extra) {
    if (o->len + extra <= o->cap) return 0;
    size_t ncap = (o->cap ? o->cap : 256);
    while (ncap < o->len + extra) ncap *= 2;
    char *nb = (char *)realloc(o->buf, ncap);
    if (!nb) return -1;
    o->buf = nb;
    o->cap = ncap;
    return 0;
}

static int out_append(OutBuf *o, const char *data, size_t size) {
    if (out_reserve(o, size) != 0) return -1;
    memcpy(o->buf + o->len, data, size);
    o->len += size;
    return 0;
}

static int out_line(OutBuf *o, const Line *l) {
    if (out_append(o, l->start, l->len) != 0) return -1;
    /* 保证每行以换行结束（冲突块内对齐需要） */
    if (l->len == 0 || l->start[l->len - 1] != '\n') {
        return out_append(o, "\n", 1);
    }
    return 0;
}

static int out_cstr(OutBuf *o, const char *s) {
    return out_append(o, s, strlen(s));
}

/* ---- 区域内容比较 ---- */

static int region_equal(const Line *lines_a, int a_start, int a_len,
                        const Line *lines_b, int b_start, int b_len) {
    if (a_len != b_len) return 0;
    for (int i = 0; i < a_len; i++) {
        if (!line_equal(&lines_a[a_start + i], &lines_b[b_start + i])) return 0;
    }
    return 1;
}

/* ---- 主合并流程 ---- */

int linemerge_3way(const char *base_data, size_t base_size,
                   const char *ours_data, size_t ours_size,
                   const char *theirs_data, size_t theirs_size,
                   const char *ours_label, const char *theirs_label,
                   char **out_data, size_t *out_size, int *conflicts) {
    *out_data = NULL;
    *out_size = 0;
    *conflicts = 0;

    int bm = 0, om = 0, tm = 0;
    Line *base = split_lines(base_data, base_size, &bm);
    Line *ours = split_lines(ours_data, ours_size, &om);
    Line *theirs = split_lines(theirs_data, theirs_size, &tm);
    if (!base || !ours || !theirs) {
        free(base); free(ours); free(theirs);
        return -1;
    }

    int oa = 0, ta = 0;
    Anchor *anch_o = lcs_anchors(base, bm, ours, om, &oa);
    Anchor *anch_t = lcs_anchors(base, bm, theirs, tm, &ta);

    if (!anch_o || !anch_t) {
        /* 文件过大等异常 → 降级为整文件冲突 */
        free(anch_o); free(anch_t);
        free(base); free(ours); free(theirs);
        return -1;
    }

    int oc = 0, tc = 0;
    Chunk *ch_o = anchors_to_chunks(anch_o, oa, bm, om, &oc);
    Chunk *ch_t = anchors_to_chunks(anch_t, ta, bm, tm, &tc);
    free(anch_o);
    free(anch_t);
    if (!ch_o || !ch_t) {
        free(ch_o); free(ch_t);
        free(base); free(ours); free(theirs);
        return -1;
    }

    OutBuf out = {0};
    int lo = 0, lt = 0, bp = 0;
    int ret = 0;

    while (lo < oc || lt < tc || bp < bm) {
        Chunk *co = (lo < oc) ? &ch_o[lo] : NULL;
        Chunk *ct = (lt < tc) ? &ch_t[lt] : NULL;

        int next_o = co ? co->base_start : bm;
        int next_t = ct ? ct->base_start : bm;
        int next = (next_o < next_t) ? next_o : next_t;
        if (next > bm) next = bm;

        /* 输出无变更区 [bp, next) */
        for (int i = bp; i < next && ret == 0; i++) {
            ret = out_line(&out, &base[i]);
        }
        if (ret != 0) break;
        bp = next;

        if (!co && !ct) break;

        /* 收集覆盖当前区域的所有变更块：
         * - 与已收区域重叠（base_start < extent）→ 必须一起处理
         * - 该侧首块且起点 == bp → 本区域的一部分
         * - 仅相邻（base_start == extent）不吸收，留作下一区域
         * 交替收集直到稳定（一侧扩张可能吞掉另一侧的块） */
        int extent = bp;
        int o_start = -1, o_end = -1;   /* ours 行区间 [o_start, o_end) */
        int t_start = -1, t_end = -1;
        int absorbed = 1;

        while (absorbed) {
            absorbed = 0;
            while (co && (co->base_start < extent ||
                          (o_start < 0 && co->base_start == bp))) {
                int e = co->base_start + co->base_len;
                if (e > extent) extent = e;
                if (o_start < 0) o_start = co->side_start;
                o_end = co->side_start + co->side_len;
                lo++;
                co = (lo < oc) ? &ch_o[lo] : NULL;
                absorbed = 1;
            }
            while (ct && (ct->base_start < extent ||
                          (t_start < 0 && ct->base_start == bp))) {
                int e = ct->base_start + ct->base_len;
                if (e > extent) extent = e;
                if (t_start < 0) t_start = ct->side_start;
                t_end = ct->side_start + ct->side_len;
                lt++;
                ct = (lt < tc) ? &ch_t[lt] : NULL;
                absorbed = 1;
            }
        }
        /* 零宽块（纯插入）不推进 bp；lo/lt 已前进，不会死循环 */

        int o_len = (o_start >= 0) ? (o_end - o_start) : -1;
        int t_len = (t_start >= 0) ? (t_end - t_start) : -1;

        if (o_start < 0) {
            /* 仅 theirs 变更 */
            for (int i = t_start; i < t_end && ret == 0; i++) {
                ret = out_line(&out, &theirs[i]);
            }
        } else if (t_start < 0) {
            /* 仅 ours 变更 */
            for (int i = o_start; i < o_end && ret == 0; i++) {
                ret = out_line(&out, &ours[i]);
            }
        } else if (region_equal(ours, o_start, o_len, theirs, t_start, t_len)) {
            /* 两方相同修改 → 取一份 */
            for (int i = o_start; i < o_end && ret == 0; i++) {
                ret = out_line(&out, &ours[i]);
            }
        } else {
            /* 真冲突 → 输出标记块 */
            ret = out_cstr(&out, "<<<<<<< ");
            if (ret == 0) ret = out_cstr(&out, ours_label);
            if (ret == 0) ret = out_cstr(&out, "\n");
            for (int i = o_start; i < o_end && ret == 0; i++) {
                ret = out_line(&out, &ours[i]);
            }
            if (ret == 0) ret = out_cstr(&out, "=======\n");
            for (int i = t_start; i < t_end && ret == 0; i++) {
                ret = out_line(&out, &theirs[i]);
            }
            if (ret == 0) ret = out_cstr(&out, ">>>>>>> ");
            if (ret == 0) ret = out_cstr(&out, theirs_label);
            if (ret == 0) ret = out_cstr(&out, "\n");
            if (ret == 0) (*conflicts)++;
        }
        if (ret != 0) break;
        bp = extent;
    }

    free(ch_o);
    free(ch_t);
    free(base);
    free(ours);
    free(theirs);

    if (ret != 0) {
        free(out.buf);
        return -1;
    }
    if (out.buf) {
        *out_data = out.buf;
    } else {
        *out_data = (char *)malloc(1);
        if (!*out_data) return -1;
        (*out_data)[0] = 0;
    }
    *out_size = out.len;
    return 0;
}
