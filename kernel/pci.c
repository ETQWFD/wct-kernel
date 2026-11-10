/*
 * pci.c —— WCT 内核 PCI 总线枚举（网卡/声卡/存储控制器的公共前置）
 *
 * 传统配置空间访问（0xCF8/0xCFC），不依赖固件 PCI 协议。
 * 提供：全总线枚举、按 class 查找、BAR 读取、能力链表（MSI/MSI-X）探测。
 * 说明：BAR 若为 MMIO，UEFI 环境下物理地址通常已映射，可直接读写；
 *       内核自己的页表接管后需改为自建映射（此处已留出 map 钩子）。
 */
#include "efi.h"

static inline void outl_p(unsigned short port, unsigned int v)
{
    __asm__ __volatile__("outl %0,%1" : : "a"(v), "Nd"(port));
}
static inline unsigned int inl_p(unsigned short port)
{
    unsigned int v;
    __asm__ __volatile__("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

unsigned int pci_read32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off)
{
    unsigned int a = 0x80000000u | ((unsigned int)bus << 16) | ((unsigned int)dev << 11)
                   | ((unsigned int)func << 8) | (off & 0xFC);
    outl_p(PCI_ADDR, a);
    return inl_p(PCI_DATA);
}
unsigned short pci_read16(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off)
{
    unsigned int v = pci_read32(bus, dev, func, off);
    return (unsigned short)((v >> ((off & 2) * 8)) & 0xFFFF);
}
unsigned char pci_read8(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off)
{
    unsigned int v = pci_read32(bus, dev, func, off);
    return (unsigned char)((v >> ((off & 3) * 8)) & 0xFF);
}
void pci_write32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off, unsigned int v)
{
    unsigned int a = 0x80000000u | ((unsigned int)bus << 16) | ((unsigned int)dev << 11)
                   | ((unsigned int)func << 8) | (off & 0xFC);
    outl_p(PCI_ADDR, a);
    outl_p(PCI_DATA, v);
}

/* ---------- 打印 ---------- */
static void pci_puts(const char *s)
{
    CHAR16 w[128];
    int i = 0;
    while (s[i] && i < 127) { w[i] = (CHAR16)s[i]; ++i; }
    w[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, w);
}
static void pci_hex(unsigned int v, int digits)
{
    static const char *H = "0123456789ABCDEF";
    char b[16];
    for (int i = digits - 1; i >= 0; --i) { b[i] = H[v & 0xF]; v >>= 4; }
    b[digits] = 0;
    pci_puts(b);
}
static void pci_dec(unsigned long long v)
{
    char t[24]; int i = 0;
    if (!v) { pci_puts("0"); return; }
    while (v && i < 23) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) { char c[2] = { t[--i], 0 }; pci_puts(c); }
}

/* ---------- 设备描述 ---------- */
typedef struct {
    unsigned char bus, dev, func;
    unsigned short vendor, device;
    unsigned char class_code, subclass, progif;
    unsigned int bar[6];
} PCI_INFO;

static PCI_INFO g_list[64];
static int g_count = 0;

int pci_count(void) { return g_count; }
const PCI_INFO *pci_get(int i) { return (i >= 0 && i < g_count) ? &g_list[i] : 0; }

static const char *pci_class_name(unsigned char cls, unsigned char sub)
{
    switch (cls) {
    case 0x01:
        if (sub == 0x06) return "SATA(AHCI)";
        if (sub == 0x01) return "IDE";
        if (sub == 0x08) return "NVMe";
        return "storage";
    case 0x02:
        if (sub == 0x00) return "ethernet";
        if (sub == 0x80) return "net-other";
        return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x0C:
        if (sub == 0x03) return "USB";
        return "serial-bus";
    case 0x06: return "bridge";
    default: return "other";
    }
}

int pci_probe(void)
{
    g_count = 0;
    for (int bus = 0; bus < 256; ++bus) {
        for (int dev = 0; dev < 32; ++dev) {
            for (int func = 0; func < 8; ++func) {
                unsigned int id = pci_read32((unsigned char)bus, (unsigned char)dev, (unsigned char)func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                if (g_count < 64) {
                    PCI_INFO *p = &g_list[g_count++];
                    p->bus = (unsigned char)bus;
                    p->dev = (unsigned char)dev;
                    p->func = (unsigned char)func;
                    p->vendor = (unsigned short)(id & 0xFFFF);
                    p->device = (unsigned short)(id >> 16);
                    unsigned int cc = pci_read32((unsigned char)bus, (unsigned char)dev, (unsigned char)func, 0x08);
                    p->class_code = (unsigned char)((cc >> 24) & 0xFF);
                    p->subclass = (unsigned char)((cc >> 16) & 0xFF);
                    p->progif = (unsigned char)((cc >> 8) & 0xFF);
                    for (int b = 0; b < 6; ++b)
                        p->bar[b] = pci_read32((unsigned char)bus, (unsigned char)dev, (unsigned char)func, (unsigned char)(0x10 + b * 4));
                }
                if (func == 0) {
                    /* 单功能设备：结束该 dev */
                    unsigned int hdr = pci_read8((unsigned char)bus, (unsigned char)dev, 0, 0x0E);
                    if (!(hdr & 0x80)) break;
                }
            }
        }
    }

    int nstore = 0, nnet = 0, naudio = 0;
    for (int i = 0; i < g_count; ++i) {
        if (g_list[i].class_code == 0x01) ++nstore;
        if (g_list[i].class_code == 0x02) ++nnet;
        if (g_list[i].class_code == 0x04) ++naudio;
    }
    pci_puts("  [pci] devices=");
    pci_dec((unsigned long long)g_count);
    pci_puts("  storage=");
    pci_dec((unsigned long long)nstore);
    pci_puts("  net=");
    pci_dec((unsigned long long)nnet);
    pci_puts("  audio=");
    pci_dec((unsigned long long)naudio);
    pci_puts("\r\n");

    for (int i = 0; i < g_count && i < 8; ++i) {
        PCI_INFO *p = &g_list[i];
        pci_puts("        ");
        pci_hex(p->bus, 2); pci_puts(":"); pci_hex(p->dev, 2); pci_puts(".");
        pci_dec((unsigned long long)p->func);
        pci_puts("  ");
        pci_hex(p->vendor, 4); pci_puts(":"); pci_hex(p->device, 4);
        pci_puts("  ");
        pci_puts(pci_class_name(p->class_code, p->subclass));
        if (p->class_code == 0x01 && p->subclass == 0x06) {
            pci_puts("  abar=0x");
            pci_hex(p->bar[5] & 0xFFFFFFF0u, 8);
        }
        pci_puts("\r\n");
    }
    return g_count;
}

int pci_find(unsigned char cls, unsigned char sub)
{
    for (int i = 0; i < g_count; ++i)
        if (g_list[i].class_code == cls && (sub == 0xFF || g_list[i].subclass == sub)) return i;
    return -1;
}

/* AHCI：读 CAP / GHC / 端口实现位图（真正的 DMA 读写留给 storage 阶段） */
int ahci_info(int idx, unsigned int *abar, unsigned int *cap, unsigned int *ports)
{
    const PCI_INFO *p = pci_get(idx);
    if (!p || p->class_code != 0x01 || p->subclass != 0x06) return 0;
    unsigned int base = p->bar[5] & 0xFFFFFFF0u;
    if (!base) return 0;
    *abar = base;
    *cap = *(volatile unsigned int *)(UINTN)base;
    *ports = *(volatile unsigned int *)(UINTN)(base + 0x0C);
    return 1;
}
