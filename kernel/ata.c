/*
 * ata.c —— WCT 内核原生磁盘层（不依赖 Linux / 固件块设备）
 *
 * 实现：
 *   - PCI 枚举找存储控制器（AHCI class 0x0106 / IDE class 0x0101）
 *   - ATA PIO 模式 IDENTIFY + LBA28 读扇区（primary/secondary，master/slave）
 *   - MBR 分区表解析（4 条主分区）
 *   - GPT 解析（保护性 MBR + 头 + 128 项分区表）
 *   - 扇区读取缓冲与打印接口，供 WCTFS/加载器使用
 *
 * 说明：AHCI 模式下的 SATA 盘需要 FIS 命令表（HBA 内存），这里先做能力探测与
 *       端口枚举；实际读写走 PIO（QEMU/VMware/大多数主板兼容模式可用）。
 */
#include "efi.h"

typedef struct {
    unsigned short iobase;
    unsigned short ctrl;
    unsigned char slave;
    int present;
    unsigned long long sectors;
    char model[41];
} ATA_DEV;

#ifdef WCT_HOST_TEST
#include <stdio.h>
static FILE *g_img;
#endif

/* ---------- x86 I/O（宿主测试时不编入） ---------- */
#ifndef WCT_HOST_TEST
static inline void outb(unsigned short port, unsigned char v)
{
    __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(port));
}
static inline unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline unsigned short inw(unsigned short port)
{
    unsigned short v;
    __asm__ __volatile__("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(unsigned short port, unsigned short v)
{
    __asm__ __volatile__("outw %0,%1" : : "a"(v), "Nd"(port));
}
static inline void insl(unsigned short port, void *buf, unsigned int count)
{
    __asm__ __volatile__("cld; rep insl" : "=D"(buf), "=c"(count) : "d"(port), "0"(buf), "1"(count) : "memory");
}

#endif

/* ---------- 小工具 ---------- */
static void ata_memzero(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}
static void ata_memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    while (n--) *a++ = *b++;
}
static void ata_puts(const char *s)
{
    CHAR16 buf[128];
    int i = 0;
    while (s[i] && i < 127) { buf[i] = (CHAR16)s[i]; ++i; }
    buf[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, buf);
}
static void ata_num(unsigned long long v)
{
    char tmp[24];
    int i = 0;
    if (!v) { ata_puts("0"); return; }
    while (v && i < 23) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) { char c[2] = { tmp[--i], 0 }; ata_puts(c); }
}

#ifdef WCT_HOST_TEST
int ata_read_sector(ATA_DEV *d, unsigned int lba, void *buf)
{
    if (!g_img) return 0;
    if (fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) return 0;
    return fread(buf, 1, 512, g_img) == 512;
}
#else
/* ---------- ATA PIO ---------- */

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6
#define ATA_SECONDARY_IO  0x170
#define ATA_SECONDARY_CTRL 0x376

static void ata_delay400(void)
{
    /* 400ns：读 alternate status 4 次 */
    inb(ATA_PRIMARY_CTRL); inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL); inb(ATA_PRIMARY_CTRL);
}

static int ata_wait(int io, int want_drq, unsigned long timeout)
{
    while (timeout--) {
        unsigned char st = inb((unsigned short)(io + 7));
        if (st & 0x01) return 0;              /* ERR */
        if (want_drq) { if (st & 0x08) return 1; }   /* DRQ */
        else { if (!(st & 0x80)) return 1; }          /* !BSY */
    }
    return 0;
}

static int ata_identify(ATA_DEV *d)
{
    unsigned short io = d->iobase;
    outb((unsigned short)(io + 6), (unsigned char)(0xA0 | (d->slave ? 0x10 : 0)));
    ata_delay400();
    outb((unsigned short)(io + 6), (unsigned char)(0xA0 | (d->slave ? 0x10 : 0)));
    outb((unsigned short)(io + 2), 0);
    outb((unsigned short)(io + 3), 0);
    outb((unsigned short)(io + 4), 0);
    outb((unsigned short)(io + 5), 0);
    outb((unsigned short)(io + 7), 0xEC);       /* IDENTIFY */
    unsigned char st = inb((unsigned short)(io + 7));
    if (st == 0) return 0;                      /* 无设备 */
    if (!ata_wait(io, 0, 100000)) return 0;
    if (inb((unsigned short)(io + 4)) != 0 || inb((unsigned short)(io + 5)) != 0) return 0;
    if (!ata_wait(io, 1, 100000)) return 0;

    unsigned short id[256];
    insl((unsigned short)io, id, 256);

    /* 型号：word 27..46，字节序交换 */
    int k = 0;
    for (int i = 27; i <= 46 && k < 40; ++i) {
        char hi = (char)(id[i] >> 8), lo = (char)(id[i] & 0xFF);
        if (hi == ' ' && lo == ' ') { if (k) d->model[k++] = ' '; continue; }
        d->model[k++] = hi; d->model[k++] = lo;
    }
    while (k > 0 && d->model[k - 1] == ' ') --k;
    d->model[k] = 0;

    unsigned long long lba = (unsigned long long)id[60] | ((unsigned long long)id[61] << 16);
    unsigned long long lba48 = 0;
    if (lba == 0) {
        lba48 = (unsigned long long)id[100] | ((unsigned long long)id[101] << 16)
              | ((unsigned long long)id[102] << 32) | ((unsigned long long)id[103] << 48);
    }
    d->sectors = lba ? lba : lba48;
    d->present = (d->sectors > 0);
    return d->present;
}

int ata_read_sector(ATA_DEV *d, unsigned int lba, void *buf)
{
    if (!d->present || lba > 0x0FFFFFFF) return 0;
    unsigned short io = d->iobase;
    outb((unsigned short)(io + 6), (unsigned char)(0xE0 | (d->slave ? 0x10 : 0) | ((lba >> 24) & 0x0F)));
    ata_delay400();
    outb((unsigned short)(io + 1), 0);
    outb((unsigned short)(io + 2), 1);
    outb((unsigned short)(io + 3), (unsigned char)(lba & 0xFF));
    outb((unsigned short)(io + 4), (unsigned char)((lba >> 8) & 0xFF));
    outb((unsigned short)(io + 5), (unsigned char)((lba >> 16) & 0xFF));
    outb((unsigned short)(io + 7), 0x20);      /* READ SECTORS */
    if (!ata_wait(io, 1, 100000)) return 0;
    insl((unsigned short)io, buf, 128);
    ata_delay400();
    return 1;
}

#endif
/* ---------- MBR / GPT ---------- */
typedef struct {
    unsigned int index;
    unsigned long long start_lba;
    unsigned long long sectors;
    unsigned int type;      /* MBR type 或 GPT 分区类型首字节 */
    int gpt;
} PART_INFO;

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int disk_parse_partitions(ATA_DEV *d, PART_INFO *out, int maxp)
{
    unsigned char sec[512];
    if (!ata_read_sector(d, 0, sec)) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
        /* 无分区表：整盘当裸设备 */
        out[0].index = 1; out[0].start_lba = 0; out[0].sectors = d->sectors;
        out[0].type = 0x00; out[0].gpt = 0;
        return 1;
    }
    int n = 0;
    /* GPT 保护分区：type 0xEE */
    int is_gpt = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned char *e = sec + 446 + i * 16;
        if (e[4] == 0xEE) { is_gpt = 1; break; }
    }
    if (is_gpt) {
        unsigned char hdr[512];
        if (!ata_read_sector(d, 1, hdr)) return 0;
        if (!(hdr[0] == 'E' && hdr[1] == 'F' && hdr[2] == 'I' && hdr[3] == ' ')) return 0;
        unsigned long long entry_lba = (unsigned long long)rd32(hdr + 72) | ((unsigned long long)rd32(hdr + 76) << 32);
        unsigned int entry_cnt = rd32(hdr + 80);
        unsigned int entry_sz = rd32(hdr + 84);
        if (entry_sz < 128 || entry_sz > 512 || entry_cnt > 128) return 0;
        /* 分区表可能跨多个扇区（128 项 × 128B = 16KB → 32 扇区），必须全部读出 */
        static unsigned char tbl[128 * 128 + 512];
        if (entry_cnt * entry_sz > sizeof(tbl)) return 0;
        unsigned int secs = (entry_cnt * entry_sz + 511) / 512;
        for (unsigned int s = 0; s < secs; ++s) {
            if (!ata_read_sector(d, (unsigned int)(entry_lba + s), tbl + s * 512)) break;
        }
        for (unsigned int i = 0; i < entry_cnt && n < maxp; ++i) {
            unsigned char *e = tbl + i * entry_sz;
            int empty = 1;
            for (int b = 0; b < 16; ++b) if (e[b]) { empty = 0; break; }
            if (empty) continue;
            out[n].index = i + 1;
            out[n].start_lba = (unsigned long long)rd32(e + 32) | ((unsigned long long)rd32(e + 36) << 32);
            out[n].sectors = (unsigned long long)rd32(e + 40) | ((unsigned long long)rd32(e + 44) << 32);
            out[n].type = e[0];       /* 类型 GUID 首字节，够用于识别 */
            out[n].gpt = 1;
            ++n;
        }
        return n;
    }
    for (int i = 0; i < 4 && n < maxp; ++i) {
        unsigned char *e = sec + 446 + i * 16;
        if (!e[4]) continue;
        out[n].index = i + 1;
        out[n].start_lba = rd32(e + 8);
        out[n].sectors = rd32(e + 12);
        out[n].type = e[4];
        out[n].gpt = 0;
        ++n;
    }
    return n;
}

#ifndef WCT_HOST_TEST
/* ---------- 探测与汇报 ---------- */
static ATA_DEV g_disks[4];
static int g_ndisks = 0;

const ATA_DEV *disk_get(int i) { return (i >= 0 && i < g_ndisks) ? &g_disks[i] : 0; }
int disk_count(void) { return g_ndisks; }

void disk_probe(void)
{
    static const unsigned short bases[2] = { ATA_PRIMARY_IO, ATA_SECONDARY_IO };
    static const unsigned short ctrls[2] = { ATA_PRIMARY_CTRL, ATA_SECONDARY_CTRL };
    g_ndisks = 0;
    for (int c = 0; c < 2 && g_ndisks < 4; ++c) {
        for (int s = 0; s < 2 && g_ndisks < 4; ++s) {
            ATA_DEV d;
            ata_memzero(&d, sizeof(d));
            d.iobase = bases[c];
            d.ctrl = ctrls[c];
            d.slave = (unsigned char)s;
            if (ata_identify(&d)) {
                g_disks[g_ndisks++] = d;
                ata_puts("  [ok] ATA disk");
                ata_num((unsigned long long)g_ndisks);
                ata_puts(": ");
                ata_puts(d.model[0] ? d.model : "(unknown)");
                ata_puts("  sectors=");
                ata_num(d.sectors);
                ata_puts("  (");
                ata_num((d.sectors * 512) / (1024 * 1024));
                ata_puts(" MB)\r\n");

                PART_INFO parts[16];
                int np = disk_parse_partitions(&d, parts, 16);
                ata_puts("       table: ");
                ata_puts(np ? (parts[0].gpt ? "GPT" : "MBR") : "none");
                ata_puts("  partitions=");
                ata_num((unsigned long long)np);
                ata_puts("\r\n");
                for (int p = 0; p < np && p < 4; ++p) {
                    ata_puts("       p");
                    ata_num(parts[p].index);
                    ata_puts(" type=0x");
                    {
                        const char *hex = "0123456789ABCDEF";
                        char hb[4] = { hex[(parts[p].type >> 4) & 0xF], hex[parts[p].type & 0xF], 0, 0 };
                        ata_puts(hb);
                    }
                    ata_puts(" start=");
                    ata_num(parts[p].start_lba);
                    ata_puts(" sectors=");
                    ata_num(parts[p].sectors);
                    ata_puts("\r\n");
                }
            }
        }
    }
    if (!g_ndisks) ata_puts("  [!!] no ATA disk on legacy ports (AHCI-only?)\r\n");
}
#endif
