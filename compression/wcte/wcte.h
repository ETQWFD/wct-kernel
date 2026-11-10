#ifndef WCTE_H
#define WCTE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 压缩：src/n 输入，out 与 out_len 输出（调用方 free）。
   block_log2 建议 16（64KB 块）；返回 0 成功，-1 失败。 */
int wcte_compress(const uint8_t *src, size_t n, uint8_t **out, size_t *out_len,
                  int block_log2, size_t *orig_written);

/* 解压：返回 0 成功。失败不会留下半成品（内部已释放）。 */
int wcte_decompress(const uint8_t *src, size_t n, uint8_t **out, size_t *out_len);

/* 判断缓冲区是否 WCTE 容器（读前 4 字节 magic） */
int wcte_probe(const uint8_t *src, size_t n);

#ifdef __cplusplus
}
#endif
#endif
