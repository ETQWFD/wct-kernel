/* wctetool —— WCTE 压缩/解压命令行
   用法：
     wctetool c <in> <out.wcte>     压缩
     wctetool d <in.wcte> <out>     解压
     wctetool t <in>                压缩+解压+逐字节比对（自检）
     wctetool b <in>                基准：压缩比 / 压缩速度 / 解压速度
*/
#include "wcte.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *p = (uint8_t *)malloc((size_t)sz + 1);
    if (!p) { fclose(f); return NULL; }
    if (sz && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return p;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s c|d|t|b <in> [out]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    size_t n = 0;
    uint8_t *data = read_file(argv[2], &n);
    if (!data) { fprintf(stderr, "读取失败: %s\n", argv[2]); return 1; }

    if (!strcmp(mode, "c") || !strcmp(mode, "d")) {
        if (argc < 4) { fprintf(stderr, "缺少输出路径\n"); return 2; }
        uint8_t *out = NULL; size_t outn = 0;
        if (!strcmp(mode, "c")) {
            double t0 = now_ms();
            if (wcte_compress(data, n, &out, &outn, 16, NULL) != 0) { fprintf(stderr, "压缩失败\n"); return 1; }
            double t1 = now_ms();
            fprintf(stderr, "压缩: %zu -> %zu B (%.1f%%)  %zu B/ms\n", n, outn, n ? 100.0 * outn / n : 0.0, (size_t)(n / (t1 - t0 + 1)));
        } else {
            if (wcte_decompress(data, n, &out, &outn) != 0) { fprintf(stderr, "解压失败（文件损坏或不是 WCTE）\n"); return 1; }
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) { fprintf(stderr, "写入失败: %s\n", argv[3]); return 1; }
        fwrite(out, 1, outn, f);
        fclose(f);
        free(out);
        free(data);
        return 0;
    }

    if (!strcmp(mode, "t")) {
        uint8_t *c = NULL; size_t cn = 0;
        double t0 = now_ms();
        if (wcte_compress(data, n, &c, &cn, 16, NULL) != 0) { fprintf(stderr, "压缩失败\n"); return 1; }
        double t1 = now_ms();
        uint8_t *d = NULL; size_t dn = 0;
        if (wcte_decompress(c, cn, &d, &dn) != 0) { fprintf(stderr, "解压失败\n"); return 1; }
        double t2 = now_ms();
        int ok = (dn == n) && (memcmp(d, data, n) == 0);
        printf("自检: %s  原始=%zu 压缩=%zu (%.1f%%)  压缩=%.0f ms 解压=%.0f ms\n",
               ok ? "通过" : "失败", n, cn, n ? 100.0 * cn / n : 0.0, t1 - t0, t2 - t1);
        if (n && t2 - t1 > 0)
            printf("速度: 压缩 %.1f MB/s  解压 %.1f MB/s\n",
                   (double)n / 1048576.0 / ((t1 - t0) / 1000.0),
                   (double)n / 1048576.0 / ((t2 - t1) / 1000.0));
        free(c); free(d); free(data);
        return ok ? 0 : 1;
    }

    if (!strcmp(mode, "b")) {
        uint8_t *c = NULL; size_t cn = 0;
        /* 预热 */
        wcte_compress(data, n, &c, &cn, 16, NULL);
        free(c); c = NULL;
        double t0 = now_ms();
        wcte_compress(data, n, &c, &cn, 16, NULL);
        double t1 = now_ms();
        uint8_t *d = NULL; size_t dn = 0;
        wcte_decompress(c, cn, &d, &dn);
        double t2 = now_ms();
        printf("输入 %.2f MB  压缩后 %.2f MB  压缩比 %.2f%%\n", n / 1048576.0, cn / 1048576.0, n ? 100.0 * cn / n : 0.0);
        printf("压缩 %.1f MB/s   解压 %.1f MB/s\n",
               n / 1048576.0 / ((t1 - t0) / 1000.0), n / 1048576.0 / ((t2 - t1) / 1000.0));
        printf("校验: %s\n", (dn == n && memcmp(d, data, n) == 0) ? "一致" : "不一致");
        free(c); free(d); free(data);
        return 0;
    }

    fprintf(stderr, "未知模式: %s\n", mode);
    return 2;
}
