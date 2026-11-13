/*
 * wctmod.c —— .wct 可执行模块加载器
 *
 * .wct 格式（WCT 自有可执行格式，不依赖 PE/ELF）：
 *   [0..3]  magic "WCT1"
 *   [4]     version = 1
 *   [5]     flags（保留）
 *   [6..7]  段数量（u16，当前只用 1 段：整块代码）
 *   [8..11] 链接基址（u32）
 *   [12..15] 入口偏移（相对链接基址）
 *   [16..19] 代码长度
 *   [20..23] 代码 CRC32（加载时校验，损坏拒绝执行）
 *   [24..]  代码
 *
 * 加载流程：读文件 → 校验 magic/长度/CRC → AllocatePages(AllocateAddress) 固定在链接基址
 *          → 拷贝代码 → 清指令缓存 → 以 WCT_API* 为唯一参数调用 entry。
 *
 * 模块只通过 API 表跟内核交互，不直接调固件，卸载后不留悬空引用。
 */
#include "efi.h"

/* 统一用 EfiLoaderData 分配；BS 由 kernel.c 在包含本文件前定义 */
static void *m_alloc(unsigned long long bytes)
{
    VOID *p = NULL;
    if (EFI_IS_ERROR(BS->AllocatePool(EfiLoaderData, (UINTN)bytes, &p))) return NULL;
    return p;
}
static void m_free(void *p) { if (p) BS->FreePool(p); }

typedef struct {
    void (*puts)(const char *s);        /* ASCII，自动处理 \n */
    void (*putu)(unsigned long long v); /* 十进制 */
    void (*clear)(void);
    int  version;
    const char *kernel_tag;
} WCT_API;

static void m_puts(const char *s)
{
    CHAR16 w[192];
    int i = 0;
    while (s[i] && i < 191) { w[i] = (CHAR16)s[i]; ++i; }
    w[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, w);
}
static void m_putu(unsigned long long v)
{
    char t[24]; int i = 0;
    if (!v) { m_puts("0"); return; }
    while (v && i < 23) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) { char c[2] = { t[--i], 0 }; m_puts(c); }
}
static void m_clear(void) { ST->ConOut->ClearScreen(ST->ConOut); }

static WCT_API g_api;

/* ---------- 小工具 ---------- */
static unsigned int m_rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned int m_crc32(const unsigned char *p, size_t n)
{
    unsigned int c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

/* 读整个文件到缓冲区（EFI 文件协议；这是固件文件服务，不是 Linux） */
static unsigned char *m_read_file(EFI_FILE_PROTOCOL *root, const CHAR16 *path, size_t *out_len)
{
    EFI_FILE_PROTOCOL *f = NULL;
    if (EFI_IS_ERROR(root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0)) || !f) return NULL;
    /* 逐次读并扩展缓冲（不依赖 GetInfo 的文件大小字段） */
    size_t cap = 4096, len = 0;
    unsigned char *data = (unsigned char *)m_alloc(cap);
    if (!data) { f->Close(f); return NULL; }
    for (;;) {
        UINTN chunk = cap - len;
        if (chunk == 0) {
            size_t ncap = cap * 2;
            unsigned char *nd = (unsigned char *)m_alloc(ncap);
            if (!nd) { m_free(data); f->Close(f); return NULL; }
            for (size_t i = 0; i < len; ++i) nd[i] = data[i];
            m_free(data);
            data = nd; cap = ncap; chunk = cap - len;
        }
        UINTN want = chunk;
        if (EFI_IS_ERROR(f->Read(f, &want, chunk ? (VOID *)(data + len) : NULL))) break;
        if (want == 0) break;
        len += (size_t)want;
    }
    f->Close(f);
    *out_len = len;
    return data;
}

/* ---------- 加载并执行 ---------- */
typedef int (*WCT_ENTRY)(WCT_API *);

int wct_run_file(EFI_FILE_PROTOCOL *root, const CHAR16 *path)
{
    size_t n = 0;
    unsigned char *img = m_read_file(root, path, &n);
    if (!img) { m_puts("  [wct] module not found\r\n"); return -1; }

    /* 打包/混淆格式 WCTX：magic + u32 key + u32 len + XOR 载荷（载荷是完整的 WCT1 镜像）。
       只是提高反编译门槛，不是密码学意义上的加密。 */
    if (n >= 12 && img[0] == 'W' && img[1] == 'C' && img[2] == 'T' && img[3] == 'X') {
        unsigned int key = m_rd32(img + 4);
        unsigned int pl = m_rd32(img + 8);
        if (pl + 12 > n) { m_puts("  [wct] packed image truncated\r\n"); m_free(img); return -1; }
        unsigned char *dec = (unsigned char *)m_alloc(pl ? pl : 1);
        if (!dec) { m_free(img); return -1; }
        for (unsigned int i = 0; i < pl; ++i) dec[i] = (unsigned char)(img[12 + i] ^ (unsigned char)(key >> ((i & 3) * 8)));
        m_free(img);
        img = dec; n = pl;
    }

    if (n < 24 || !(img[0] == 'W' && img[1] == 'C' && img[2] == 'T' && img[3] == '1')) {
        m_puts("  [wct] bad magic: ");
        for (int q = 0; q < 8; ++q) { m_putu(img[q]); m_puts(" "); }
        m_puts(" len="); m_putu(n); m_puts("\r\n");
        m_free(img);
        return -1;
    }
    unsigned int base = m_rd32(img + 8);
    unsigned int entry_off = m_rd32(img + 12);
    unsigned int code_len = m_rd32(img + 16);
    unsigned int crc = m_rd32(img + 20);
    if (n < (size_t)24 + code_len) {
        m_puts("  [wct] truncated image: file="); m_putu(n);
        m_puts(" need="); m_putu((unsigned long long)(24 + code_len)); m_puts("\r\n");
        m_free(img); return -1;
    }
    if (m_crc32(img + 24, code_len) != crc) { m_puts("  [wct] checksum mismatch, refusing to run\r\n"); m_free(img); return -1; }

    unsigned int pages = (code_len + 4095) / 4096 + 1;
    EFI_PHYSICAL_ADDRESS at = base;
    int exact = 1;
    if (EFI_IS_ERROR(BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &at))) {
        /* 首选基址不可用时退化为任意页：模块代码为位置无关（仅 RIP 相对寻址），
           加载到任意地址仍然正确，入口按偏移计算 */
        at = 0;
        exact = 0;
        if (EFI_IS_ERROR(BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &at))) {
            m_puts("  [wct] out of memory for module\r\n");
            m_free(img);
            return -1;
        }
    }
    unsigned char *dst = (unsigned char *)(UINTN)at;
    for (unsigned int i = 0; i < code_len; ++i) dst[i] = img[24 + i];
    m_free(img);

    /* 指令缓存一致性：UEFI x86 一般无需手动 flush，为稳妥执行 wbinvd */
    __asm__ __volatile__("wbinvd");

    g_api.puts = m_puts;
    g_api.putu = m_putu;
    g_api.clear = m_clear;
    g_api.version = 1;
    g_api.kernel_tag = "WCT-Kernel";

    m_puts("  [wct] loading module at 0x");
    {
        static const char *H = "0123456789ABCDEF";
        char b[9];
        for (int i = 7, v = (int)at; i >= 0; --i, v >>= 4) b[i] = H[v & 0xF];
        b[8] = 0;
        m_puts(b);
    }
    m_puts(exact ? "  (fixed base)" : "  (relocated)");
    m_puts("  entry=+");
    m_putu((unsigned long long)entry_off);
    m_puts("\r\n");

    WCT_ENTRY fn = (WCT_ENTRY)(UINTN)(at + entry_off);
    int rc = fn(&g_api);
    m_puts("  [wct] module returned ");
    m_putu((unsigned long long)(unsigned int)rc);
    m_puts("\r\n");
    return rc;
}
