/* wcteos 原生音频驱动（Intel HDA 控制器初始化 + codec 探测）
   不依赖 Linux：直接操作 HDA 寄存器做复位、codec 枚举、厂商/设备 ID 读取。
   播放通路需要 codec 具体 verb 序列，不同 codec 差异较大，这里先保证控制器与 codec 可访问。 */
#ifndef WCT_AUDIO_C
#define WCT_AUDIO_C

#define HDA_GCAP      0x00
#define HDA_GCTL      0x08
#define HDA_STATESTS  0x0E
#define HDA_ICW       0x60
#define HDA_IRR       0x64
#define HDA_ICS       0x68

static volatile UINT32 *hda_mmio;
static int hda_present, hda_codecs, hda_vendor_ok;
static UINT32 hda_vendor, hda_device;

static UINT32 hda_rd(UINT32 r) { return hda_mmio[r / 4]; }
static void hda_wr(UINT32 r, UINT32 v) { hda_mmio[r / 4] = v; }

static int hda_verb(UINT32 cmd, UINT32 *resp)
{
    for (int i = 0; i < 1000; ++i) {
        if (!(hda_rd(HDA_ICS) & 1u)) break;
        ST->BootServices->Stall(100);
    }
    hda_wr(HDA_ICW, cmd);
    for (int i = 0; i < 3000; ++i) {
        UINT32 st = hda_rd(HDA_ICS);
        if (!(st & 1u)) {
            if (st & 2u) { *resp = hda_rd(HDA_IRR); return 1; }   /* IRV: 有应答 */
            return 0;
        }
        ST->BootServices->Stall(100);
    }
    return 0;
}

int audio_probe(void)
{
    hda_present = 0; hda_codecs = 0; hda_vendor_ok = 0;
    for (int i = 0; i < pci_count(); ++i) {
        const PCI_INFO *p = pci_get(i);
        if (!p || p->class_code != 0x04 || p->subclass != 0x03) continue;
        UINT32 bar = p->bar[0] & 0xFFFFFFF0u;
        if (!bar) continue;
        hda_mmio = (volatile UINT32 *)(UINTN)bar;
        hda_present = 1;
        break;
    }
    if (!hda_present) return 0;

    hda_wr(HDA_GCTL, 1);                                  /* CRST: 进入复位 */
    ST->BootServices->Stall(20000);
    hda_wr(HDA_GCTL, 0);                                  /* 退出复位 */
    ST->BootServices->Stall(30000);
    hda_wr(HDA_GCTL, 0);
    ST->BootServices->Stall(10000);

    UINT32 st = hda_rd(HDA_STATESTS);
    for (int tries = 0; tries < 5 && !st; ++tries) {
        ST->BootServices->Stall(20000);
        st = hda_rd(HDA_STATESTS);
    }
    for (int i = 0; i < 16; ++i) if (st & (1u << i)) ++hda_codecs;
    hda_wr(HDA_STATESTS, st);                             /* 写 1 清零 */

    /* GET PARAMETER(VENDOR_ID)：verb 0xF00，codec 地址 0 */
    UINT32 resp = 0;
    if (hda_verb(0x000F0000u, &resp)) { hda_vendor = (resp >> 16) & 0xFFFF; hda_device = resp & 0xFFFF; hda_vendor_ok = 1; }
    return 1;
}

const char *audio_status_line(void)
{
    static char line[80];
    int i = 0;
    if (!hda_present) return "audio: no HDA controller";
    const char *p = "audio: HDA codecs=";
    while (*p) line[i++] = *p++;
    line[i++] = (char)('0' + (hda_codecs % 10));
    if (hda_vendor_ok) {
        const char *q = "  vendor=0x";
        while (*q) line[i++] = *q++;
        for (int s = 12; s >= 0; s -= 4) line[i++] = "0123456789ABCDEF"[(hda_vendor >> s) & 0xF];
        const char *r = " dev=0x";
        while (*r) line[i++] = *r++;
        for (int s = 12; s >= 0; s -= 4) line[i++] = "0123456789ABCDEF"[(hda_device >> s) & 0xF];
    } else {
        const char *q = "  verb:no-response";
        while (*q) line[i++] = *q++;
    }
    line[i] = 0;
    return line;
}

#endif
