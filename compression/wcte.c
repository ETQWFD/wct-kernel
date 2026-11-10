/*
 * wcte.c —— WCTE 压缩格式（WCT Encoded）v2，DUESLIN / WCT-OS 专用
 *
 * 目标：简单、绝不损坏、解压极快、压缩比可接受。
 *
 * 容器（全部小端）：
 *   [0..3]   magic "WCTE"
 *   [4]      version = 2
 *   [5]      flags   bit0=1 整体存储
 *   [6]      block_log2（14..20，默认 16 = 64KB）
 *   [7]      reserved
 *   [8..15]  original_size (u64)
 *   [16..19] block_count (u32)
 *   [20]     块索引，每块 16 字节：{orig_len u32, comp_len u32, hash64 u64}
 *   之后     块数据顺序排列
 *
 * 单块负载（字节流，逐条指令，遇“输出满 orig_len”即结束）：
 *   0x00..0x7F  字面量：整数值 + 1 字节数（1..128），其后跟该数量的原始字节
 *   0x80..0xFF  匹配：低 7 位 + 3 为长度（3..130），随后 2 字节偏移(1..65535)
 *     （长度超过 130 的匹配拆成多条，实现简单且解码无需查表）
 *
 * 每块独立：可并行、可随机定位；块内 64 位哈希校验，损坏必定被发现，绝不静默出错。
 */
#include "wcte.h"
#include <stdlib.h>
#include <string.h>

#define WCTE_VERSION 2
#define MIN_MATCH 3
#define MAX_RUN 128
#define MAX_MATCH 130
#define HLOG 15
#define HSIZE (1u << HLOG)  /* 128KB 表 */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* 64 位块哈希：8 字节并行混合，速度 ~GB/s 级；用于损坏检测 */
static uint64_t wcte_hash64(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ ((uint64_t)n * 0xBF58476D1CE4E5B9ull);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h ^= w * 0xD6E8FEB86659FD93ull;
        h = (h << 31) | (h >> 33);
        h *= 0x94D049BB133111EBull;
    }
    for (; i < n; ++i) { h ^= (uint64_t)p[i] << ((i & 7) * 8); h *= 0x100000001B3ull; }
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return h;
}

/* ---------------- 单块压缩 ---------------- */
static size_t wcte_compress_block(const uint8_t *src, size_t n, uint8_t *dst, size_t cap, uint32_t *ht)
{
    if (n < 8) return 0;
    memset(ht, 0, HSIZE * sizeof(uint32_t));

    size_t ip = 0, op = 0, anchor = 0;

    while (ip + MIN_MATCH <= n) {
        uint32_t seq;
        memcpy(&seq, src + ip, 4 < (n - ip) ? 4 : (n - ip));
        uint32_t h = (seq * 2654435761u) >> (32 - HLOG);
        size_t ref = ht[h];
        ht[h] = (uint32_t)(ip + 1);

        int found = 0;
        size_t mlen = 0;
        if (ref) {
            size_t cand = ref - 1;
            if (ip > cand && (ip - cand) <= 65535) {
                const uint8_t *a = src + cand, *b = src + ip;
                size_t limit = n - ip, m = 0;
                if (limit > MAX_MATCH) limit = MAX_MATCH;
                while (m < limit && a[m] == b[m]) ++m;
                if (m >= MIN_MATCH) { found = 1; mlen = m; }
            }
        }
        if (!found) { ++ip; continue; }

        /* 先刷字面量（可能多段，每段 ≤128） */
        {
            size_t lit = ip - anchor;
            size_t need = (lit + MAX_RUN - 1) / MAX_RUN * 1 + lit + 3;
            if (op + need > cap) return 0;
            size_t left = lit;
            const uint8_t *p = src + anchor;
            while (left) {
                size_t run = left > MAX_RUN ? MAX_RUN : left;
                dst[op++] = (uint8_t)(run - 1);
                memcpy(dst + op, p, run);
                op += run; p += run; left -= run;
            }
        }
        /* 再输出匹配（可能多段，每段 ≤130） */
        {
            size_t left = mlen;
            size_t off = ip - (ref - 1);
            if (op + 3 > cap) return 0;
            while (left >= MIN_MATCH) {
                size_t run = left > MAX_MATCH ? MAX_MATCH : left;
                if (op + 3 > cap) return 0;
                dst[op++] = (uint8_t)(0x80 | (run - MIN_MATCH));
                dst[op++] = (uint8_t)(off & 0xFF);
                dst[op++] = (uint8_t)((off >> 8) & 0xFF);
                left -= run;
            }
            /* 残留 1~2 字节并入字面量，避免出现 <MIN_MATCH 的匹配 */
            if (left) {
                if (op + left + 1 > cap) return 0;
                dst[op++] = (uint8_t)(left - 1);
                memcpy(dst + op, src + ip + (mlen - left), left);
                op += left;
            }
        }
        ip += mlen;
        anchor = ip;
    }

    /* 尾部字面量 */
    {
        size_t left = n - anchor;
        const uint8_t *p = src + anchor;
        if (op + left + (left + MAX_RUN - 1) / MAX_RUN > cap) { free(ht); return 0; }
        while (left) {
            size_t run = left > MAX_RUN ? MAX_RUN : left;
            dst[op++] = (uint8_t)(run - 1);
            memcpy(dst + op, p, run);
            op += run; p += run; left -= run;
        }
    }
    return op;
}

/* ---------------- 单块解压 ---------------- */
static int wcte_decompress_block(const uint8_t *src, size_t clen, uint8_t *dst, size_t olen)
{
    size_t ip = 0, op = 0;
    while (op < olen) {
        if (ip >= clen) return -1;
        uint8_t tag = src[ip++];
        if (tag < 0x80) {
            size_t run = (size_t)tag + 1;
            if (op + run > olen || ip + run > clen) return -1;
            memcpy(dst + op, src + ip, run);
            ip += run; op += run;
        } else {
            size_t run = (size_t)(tag & 0x7F) + MIN_MATCH;
            if (ip + 2 > clen) return -1;
            size_t off = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
            ip += 2;
            if (off == 0 || off > op) return -1;
            if (op + run > olen) return -1;
            const uint8_t *from = dst + op - off;
            uint8_t *to = dst + op;
            if (off >= run) memcpy(to, from, run);
            else {
                size_t done = 0;
                while (done < run) {
                    size_t chunk = off;
                    if (chunk > run - done) chunk = run - done;
                    memcpy(to + done, from + done, chunk);
                    done += chunk;
                }
            }
            op += run;
        }
    }
    return (op == olen) ? 0 : -1;
}

/* ---------------- 容器 ---------------- */
int wcte_compress(const uint8_t *src, size_t n, uint8_t **out, size_t *out_len,
                  int block_log2, size_t *orig_written)
{
    if (!src || !out || !out_len) return -1;
    if (block_log2 < 14 || block_log2 > 20) block_log2 = 18;
    size_t bsz = (size_t)1 << block_log2;
    uint32_t blocks = (uint32_t)((n + bsz - 1) / bsz);
    if (blocks == 0) blocks = 1;

    size_t hdr = 20 + (size_t)blocks * 16;
    size_t cap = hdr + n + (size_t)blocks * 16 + 1024;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return -1;
    uint32_t *ht = (uint32_t *)malloc(HSIZE * sizeof(uint32_t));
    if (!ht) { free(buf); return -1; }

    size_t body = hdr;
    size_t total = 0;

    for (uint32_t i = 0; i < blocks; ++i) {
        size_t off = (size_t)i * bsz;
        size_t len = (off + bsz <= n) ? bsz : n - off;
        uint8_t *idx = buf + 20 + (size_t)i * 16;
        size_t clen = 0;

        if (len) {
            size_t tcap = len * 2 + 256;          /* 最坏情况：匹配密集时输出可超原长 */
            uint8_t *tmp = (uint8_t *)malloc(tcap);
            if (!tmp) { free(buf); free(ht); return -1; }
            if (len >= 32) clen = wcte_compress_block(src + off, len, tmp, tcap, ht);
            if (clen == 0 || clen + 4 >= len) {
                memcpy(buf + body, src + off, len);
                clen = len;
            } else {
                memcpy(buf + body, tmp, clen);
            }
            free(tmp);
        }
        wr32(idx, (uint32_t)len);
        wr32(idx + 4, (uint32_t)clen);
        wr64(idx + 8, wcte_hash64(src + off, len));
        body += clen;
        total += clen;
    }

    free(ht);
    buf[0] = 'W'; buf[1] = 'C'; buf[2] = 'T'; buf[3] = 'E';
    buf[4] = WCTE_VERSION;
    buf[5] = 0;
    buf[6] = (uint8_t)block_log2;
    buf[7] = 0;
    wr64(buf + 8, (uint64_t)n);
    wr32(buf + 16, blocks);

    *out = buf;
    *out_len = body;
    if (orig_written) *orig_written = total;
    return 0;
}

int wcte_decompress(const uint8_t *src, size_t n, uint8_t **out, size_t *out_len)
{
    if (!src || n < 20) return -1;
    if (!(src[0] == 'W' && src[1] == 'C' && src[2] == 'T' && src[3] == 'E')) return -1;
    if (src[4] != WCTE_VERSION) return -1;
    size_t bsz_log = src[6];
    if (bsz_log < 14 || bsz_log > 20) return -1;
    uint64_t orig = rd64(src + 8);
    uint32_t blocks = rd32(src + 16);
    if (blocks == 0) return -1;
    size_t hdr = 20 + (size_t)blocks * 16;
    if (hdr > n) return -1;

    uint8_t *dst = (uint8_t *)malloc((size_t)orig + 1);
    if (!dst) return -1;

    size_t ip = hdr, op = 0;
    for (uint32_t i = 0; i < blocks; ++i) {
        const uint8_t *idx = src + 20 + (size_t)i * 16;
        uint32_t blen = rd32(idx);
        uint32_t clen = rd32(idx + 4);
        uint64_t hash = rd64(idx + 8);
        if (op + blen > (size_t)orig || ip + clen > n) { free(dst); return -1; }
        if (clen == blen) memcpy(dst + op, src + ip, blen);
        else if (wcte_decompress_block(src + ip, clen, dst + op, blen) != 0) { free(dst); return -1; }
        if (wcte_hash64(dst + op, blen) != hash) { free(dst); return -1; }   /* 数据损坏必被发现 */
        ip += clen; op += blen;
    }
    if (op != (size_t)orig) { free(dst); return -1; }

    *out = dst;
    *out_len = (size_t)orig;
    return 0;
}

int wcte_probe(const uint8_t *src, size_t n)
{
    if (n < 20) return 0;
    return (src[0] == 'W' && src[1] == 'C' && src[2] == 'T' && src[3] == 'E') ? 1 : 0;
}
