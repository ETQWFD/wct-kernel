/* wcteos 原生网卡驱动：Intel 8254x (e1000) MMIO + 描述符环
   目标：不依赖 Linux 网络栈，自己收发以太网帧。
   验证方式：发 ARP 请求给 10.0.2.2（QEMU 用户网络网关），收到 ARP 应答即证明收发通路真实可用。 */
#ifndef WCT_NET_C
#define WCT_NET_C

#define E1000_CTRL   0x0000
#define E1000_STATUS 0x0008
#define E1000_RCTL   0x0100
#define E1000_TCTL   0x0400
#define E1000_TIPG   0x0410
#define E1000_RDBAL  0x2800
#define E1000_RDBAH  0x2804
#define E1000_RDLEN  0x2808
#define E1000_RDH    0x2810
#define E1000_RDT    0x2818
#define E1000_TDBAL  0x3800
#define E1000_TDBAH  0x3804
#define E1000_TDLEN  0x3808
#define E1000_TDH    0x3810
#define E1000_TDT    0x3818
#define E1000_RAL0   0x5400
#define E1000_RAH0   0x5404
#define E1000_MTA    0x5200

#define NET_RX_N 16
#define NET_TX_N 8
#define NET_BUF  2048

static volatile UINT32 *net_mmio;
static UINT8 net_rx_desc[NET_RX_N * 16] __attribute__((aligned(16)));
static UINT8 net_tx_desc[NET_TX_N * 16] __attribute__((aligned(16)));
static UINT8 net_rx_buf[NET_RX_N][NET_BUF] __attribute__((aligned(16)));
static UINT8 net_tx_buf[NET_TX_N][NET_BUF] __attribute__((aligned(16)));

static unsigned char net_mac[6];
static int net_present, net_link, net_ready, net_arp_ok, net_tx_tail;
static int net_kind;              /* 1 = e1000, 2 = rtl8139 */
static const char *net_drv_name = "none";
static UINT32 net_rx_packets, net_tx_packets;
static UINT32 net_ip;          /* 假定 10.0.2.15 */

static UINT32 net_rd(UINT32 reg) { return net_mmio[reg / 4]; }
static void net_wr(UINT32 reg, UINT32 v) { net_mmio[reg / 4] = v; }

static void net_desc(UINT8 *base, int i, UINT32 addr, UINT16 len, UINT8 cmd, UINT8 status)
{
    UINT8 *d = base + i * 16;
    *(UINT32 *)(d + 0) = addr;
    *(UINT32 *)(d + 4) = 0;
    *(UINT16 *)(d + 8) = len;
    d[10] = 0; d[11] = cmd; d[12] = status; d[13] = 0;
    *(UINT16 *)(d + 14) = 0;
}

static void net_put_dec(char *out, int *pos, UINT32 v)
{
    char t[12]; int n = 0;
    if (!v) { out[(*pos)++] = '0'; return; }
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) out[(*pos)++] = t[--n];
}


/* ---------- Realtek 8139（PCI 10EC:8139，IO BAR，QEMU/老主板常见） ---------- */
#define RTL_IDR0    0x00
#define RTL_TSD0    0x10
#define RTL_TSAD0   0x20
#define RTL_RBSTART 0x30
#define RTL_CR      0x37
#define RTL_CAPR    0x38
#define RTL_CBR     0x3A
#define RTL_IMR     0x3C
#define RTL_ISR     0x3E
#define RTL_TCR     0x40
#define RTL_RCR     0x44
#define RTL_CFG1    0x52

#define RTL_RX_SZ   (8192 + 16)
static UINT16 rtl_io;
static UINT8 rtl_rxbuf[RTL_RX_SZ] __attribute__((aligned(16)));
static UINT8 rtl_txbuf[2][2048] __attribute__((aligned(16)));
static int rtl_tx_i, rtl_rx_cur;

static UINT32 io_inl(UINT16 p) { UINT32 v; __asm__ __volatile__("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void io_outl(UINT16 p, UINT32 v) { __asm__ __volatile__("outl %0,%1" : : "a"(v), "Nd"(p)); }
static void io_outw(UINT16 p, UINT16 v) { __asm__ __volatile__("outw %0,%1" : : "a"(v), "Nd"(p)); }
static UINT16 io_inw(UINT16 p) { UINT16 v; __asm__ __volatile__("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static int net_rtl_probe(void)
{
    UINT32 io = 0;
    for (int i = 0; i < pci_count(); ++i) {
        const PCI_INFO *p = pci_get(i);
        if (!p || p->class_code != 0x02) continue;
        if (p->vendor != 0x10EC) continue;                 /* Realtek */
        if (p->device != 0x8139 && p->device != 0x8139) continue;
        if (!(p->bar[0] & 1u)) continue;                   /* 需要 IO BAR */
        io = p->bar[0] & 0xFFFC;
        break;
    }
    if (!io) return 0;
    rtl_io = (UINT16)io;

    p_outb(rtl_io + RTL_CFG1, 0x00);                        /* 退出低功耗 */
    p_outb(rtl_io + RTL_CR, 0x10);                          /* RST */
    for (int i = 0; i < 1000; ++i) {
        if (!(p_inb(rtl_io + RTL_CR) & 0x10)) break;
        ST->BootServices->Stall(1000);
    }
    p_outb(rtl_io + RTL_IMR, 0x00);
    io_outw(rtl_io + RTL_ISR, 0xFFFF);
    io_outl(rtl_io + RTL_TCR, 0x03000700);
    io_outl(rtl_io + RTL_RCR, 0x0000008F);                  /* AB|AM|APM|AR + 8K */
    io_outl(rtl_io + RTL_RBSTART, (UINT32)(UINTN)rtl_rxbuf);
    p_outb(rtl_io + RTL_CAPR, 0x00);
    p_outb(rtl_io + RTL_CR, 0x0C);                           /* RE|TE */

    for (int i = 0; i < 6; ++i) net_mac[i] = p_inb(rtl_io + RTL_IDR0 + i);
    int allz = 1;
    for (int i = 0; i < 6; ++i) if (net_mac[i]) allz = 0;
    if (allz) { net_mac[0]=0x52; net_mac[1]=0x54; net_mac[2]=0x00; net_mac[3]=0x12; net_mac[4]=0x34; net_mac[5]=0x56; }

    rtl_tx_i = 0; rtl_rx_cur = 0; net_tx_tail = 0;
    net_ip = 0x0A00020F;
    net_kind = 2; net_drv_name = "rtl8139"; net_link = 1; net_ready = 1; net_present = 1;
    return 1;
}

static int net_rtl_send(const UINT8 *frame, UINT16 len)
{
    if (len > 2048) return 0;
    int i = rtl_tx_i;
    for (int k = 0; k < len; ++k) rtl_txbuf[i][k] = frame[k];
    io_outl(rtl_io + RTL_TSAD0 + i * 4, (UINT32)(UINTN)rtl_txbuf[i]);
    io_outl(rtl_io + RTL_TSD0 + i * 4, (UINT32)len);         /* 写长度即启动 DMA */
    rtl_tx_i = (i + 1) & 1;
    ++net_tx_packets;
    for (int spin = 0; spin < 20000; ++spin) {
        UINT32 tsd = io_inl(rtl_io + RTL_TSD0 + i * 4);
        if ((tsd & 0x00003FFF) == 0) return 1;
        ST->BootServices->Stall(50);
    }
    return 1;
}

static int net_rtl_poll(UINT8 *out, UINT16 *out_len)
{
    UINT16 cbr = io_inw(rtl_io + RTL_CBR);
    if (cbr == (UINT16)rtl_rx_cur) return 0;
    UINT8 *pkt = rtl_rxbuf + rtl_rx_cur;
    UINT16 status = *(UINT16 *)pkt;
    UINT16 len = *(UINT16 *)(pkt + 2);
    if (!(status & 0x0001) || len < 4 || len > 1600) {       /* ROK */
        UINT16 next = (UINT16)((rtl_rx_cur + 4 + (len ? len : 4) + 3) & ~3u);
        rtl_rx_cur = (int)((next + 4) % (RTL_RX_SZ - 16));
        p_outb(rtl_io + RTL_CAPR, (UINT8)((rtl_rx_cur - 16) & 0xFF));
        return 0;
    }
    UINT16 dlen = (UINT16)(len - 4);                         /* 去掉 CRC */
    if (dlen > NET_BUF) dlen = NET_BUF;
    for (int k = 0; k < dlen; ++k) out[k] = pkt[4 + k];
    *out_len = dlen;
    UINT16 next = (UINT16)((rtl_rx_cur + 4 + len + 3) & ~3u);
    rtl_rx_cur = (int)((next + 4) % (RTL_RX_SZ - 16));
    p_outb(rtl_io + RTL_CAPR, (UINT8)((rtl_rx_cur - 16) & 0xFF));
    ++net_rx_packets;
    return 1;
}

static int net_link_up(void) { return net_ready && net_arp_ok; }

int net_probe(void)
{
    net_present = 0; net_ready = 0; net_link = 0; net_arp_ok = 0;
    net_kind = 0; net_drv_name = "none";
    for (int i = 0; i < pci_count(); ++i) {
        const PCI_INFO *p = pci_get(i);
        if (!p) continue;
        if (p->class_code != 0x02) continue;         /* network controller */
        if (p->vendor != 0x8086) continue;
        UINT32 bar = p->bar[0] & 0xFFFFFFF0u;        /* MMIO BAR */
        if (!bar) continue;
        net_mmio = (volatile UINT32 *)(UINTN)bar;
        net_present = 1;
        break;
    }
    if (!net_present) return net_rtl_probe();   /* 没有 Intel，就试 Realtek 8139 */

    /* 复位 */
    net_wr(E1000_CTRL, net_rd(E1000_CTRL) | (1u << 26));
    for (int i = 0; i < 1000 && (net_rd(E1000_CTRL) & (1u << 26)); ++i)
        ST->BootServices->Stall(1000);

    /* 清组播表 */
    for (int i = 0; i < 128; ++i) net_wr(E1000_MTA + i * 4, 0);

    /* 取 MAC（芯片出厂 RAL0/RAH0） */
    UINT32 lo = net_rd(E1000_RAL0), hi = net_rd(E1000_RAH0);
    net_mac[0] = (unsigned char)(lo & 0xFF);
    net_mac[1] = (unsigned char)((lo >> 8) & 0xFF);
    net_mac[2] = (unsigned char)((lo >> 16) & 0xFF);
    net_mac[3] = (unsigned char)((lo >> 24) & 0xFF);
    net_mac[4] = (unsigned char)(hi & 0xFF);
    net_mac[5] = (unsigned char)((hi >> 8) & 0xFF);
    if (!net_mac[0] && !net_mac[1] && !net_mac[2]) {
        /* 芯片没给 MAC（部分虚拟环境），退回本地管理地址 */
        net_mac[0] = 0x02; net_mac[1] = 0x00; net_mac[2] = 0x27;
        net_mac[3] = 0x10; net_mac[4] = 0x20; net_mac[5] = 0x30;
        lo = (UINT32)net_mac[0] | ((UINT32)net_mac[1] << 8) | ((UINT32)net_mac[2] << 16) | ((UINT32)net_mac[3] << 24);
        hi = (UINT32)net_mac[4] | ((UINT32)net_mac[5] << 8) | (1u << 31);   /* AV */
        net_wr(E1000_RAL0, lo); net_wr(E1000_RAH0, hi);
    }
    net_ip = 0x0A00020F;    /* 10.0.2.15 */

    /* 接收环 */
    for (int i = 0; i < NET_RX_N; ++i)
        net_desc(net_rx_desc, i, (UINT32)(UINTN)net_rx_buf[i], NET_BUF, 0, 0);
    net_wr(E1000_RDBAL, (UINT32)(UINTN)net_rx_desc);
    net_wr(E1000_RDBAH, 0);
    net_wr(E1000_RDLEN, NET_RX_N * 16);
    net_wr(E1000_RDH, 0);
    net_wr(E1000_RDT, NET_RX_N - 1);

    /* 发送环 */
    for (int i = 0; i < NET_TX_N; ++i)
        net_desc(net_tx_desc, i, (UINT32)(UINTN)net_tx_buf[i], 0, 0, 0);
    net_wr(E1000_TDBAL, (UINT32)(UINTN)net_tx_desc);
    net_wr(E1000_TDBAH, 0);
    net_wr(E1000_TDLEN, NET_TX_N * 16);
    net_wr(E1000_TDH, 0);
    net_wr(E1000_TDT, 0);
    net_tx_tail = 0;

    net_wr(E1000_TIPG, 0x0060200A);
    /* RCTL: EN | BAM | SECRC | BSIZE 2048 | 无 SBP */
    net_wr(E1000_RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    /* TCTL: EN | PSP | CT=0x10 | COLD=0x40 */
    net_wr(E1000_TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));

    net_link = (net_rd(E1000_STATUS) & 2u) ? 1 : 0;
    net_kind = 1; net_drv_name = "e1000"; net_ready = 1;
    return 1;
}

static int net_send(const UINT8 *frame, UINT16 len)
{
    if (!net_ready || len > NET_BUF) return 0;
    if (net_kind == 2) return net_rtl_send(frame, len);
    int i = net_tx_tail;
    for (int k = 0; k < len; ++k) net_tx_buf[i][k] = frame[k];
    net_desc(net_tx_desc, i, (UINT32)(UINTN)net_tx_buf[i], len, 0x0B /*EOP|IFCS|RS*/, 0);
    net_tx_tail = (i + 1) % NET_TX_N;
    net_wr(E1000_TDT, (UINT32)net_tx_tail);
    ++net_tx_packets;
    /* 等发送完（DD 位置 1） */
    UINT8 *d = net_tx_desc + i * 16;
    for (int spin = 0; spin < 10000; ++spin) {
        if (d[12] & 0x01) return 1;
        ST->BootServices->Stall(50);
    }
    return 0;
}

static int net_poll(UINT8 *out, UINT16 *out_len)
{
    if (!net_ready) return 0;
    if (net_kind == 2) return net_rtl_poll(out, out_len);
    UINT32 head = net_rd(E1000_RDH);
    static UINT32 seen = 0;
    if (head == seen) return 0;
    UINT8 *d = net_rx_desc + seen * 16;
    UINT16 len = *(UINT16 *)(d + 8);
    if (!(d[12] & 0x01)) return 0;          /* DD */
    if (len > NET_BUF) len = NET_BUF;
    for (int k = 0; k < len; ++k) out[k] = net_rx_buf[seen][k];
    *out_len = len;
    d[12] = 0;
    net_wr(E1000_RDT, seen);
    seen = (seen + 1) % NET_RX_N;
    ++net_rx_packets;
    return 1;
}

/* 发 ARP 请求并等应答：真实验证收发通路 */
int net_arp_test(void)
{
    if (!net_ready) return 0;
    UINT8 frame[64];
    for (int i = 0; i < 64; ++i) frame[i] = 0;
    for (int i = 0; i < 6; ++i) frame[i] = 0xFF;                /* dst broadcast */
    for (int i = 0; i < 6; ++i) frame[6 + i] = net_mac[i];      /* src */
    frame[12] = 0x08; frame[13] = 0x06;                         /* ARP */
    frame[14] = 0x00; frame[15] = 0x01;                         /* ethernet */
    frame[16] = 0x08; frame[17] = 0x00;                         /* IPv4 */
    frame[18] = 6; frame[19] = 4; frame[20] = 0x00; frame[21] = 0x01;  /* request */
    for (int i = 0; i < 6; ++i) frame[22 + i] = net_mac[i];
    frame[28] = (UINT8)(net_ip & 0xFF); frame[29] = (UINT8)((net_ip >> 8) & 0xFF);
    frame[30] = (UINT8)((net_ip >> 16) & 0xFF); frame[31] = (UINT8)((net_ip >> 24) & 0xFF);
    for (int i = 0; i < 6; ++i) frame[32 + i] = 0;
    frame[38] = 0x0A; frame[39] = 0x00; frame[40] = 0x02; frame[41] = 0x02;  /* 10.0.2.2 */
    /* 重发最多 3 次：TCG 下 slirp 的应答经常晚于 0.8s，一次不中就判失败太苛刻 */
    UINT8 rx[NET_BUF]; UINT16 len = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!net_send(frame, 42)) { ST->BootServices->Stall(5000); continue; }
        for (int tries = 0; tries < 500; ++tries) {
            if (net_poll(rx, &len)) {
                if (len >= 42 && rx[12] == 0x08 && rx[13] == 0x06 && rx[20] == 0x00 && rx[21] == 0x02) {
                    net_arp_ok = 1;
                    return 1;
                }
            }
            ST->BootServices->Stall(3000);
        }
    }
    return 0;
}

const char *net_short_status(void)
{
    static char b[48];
    int i = 0;
    for (const char *d = net_drv_name; *d && i < 10; ++d) b[i++] = *d;
    const char *tail = "  link:up  arp:ok";
    if (!net_link) tail = "  link:down";
    else if (!net_arp_ok) tail = "  link:up  arp:no";
    for (const char *q = tail; *q && i < 46; ++q) b[i++] = *q;
    b[i] = 0;
    return b;
}

const char *net_status_line(void)
{
    static char line[96];
    int i = 0;
    if (!net_present) return "net: no NIC (e1000/rtl8139 均未找到)";
    line[i++] = 'n'; line[i++] = 'e'; line[i++] = 't'; line[i++] = ':'; line[i++] = ' ';
    for (const char *d = net_drv_name; *d && i < 20; ++d) line[i++] = *d;
    line[i++] = ' ';
    for (int k = 0; k < 6; ++k) {
        if (k) line[i++] = ':';
        line[i++] = "0123456789ABCDEF"[net_mac[k] >> 4];
        line[i++] = "0123456789ABCDEF"[net_mac[k] & 15];
    }
    const char *lk = net_link ? "  link:up" : "  link:down";
    while (*lk) line[i++] = *lk++;
    const char *ar = net_arp_ok ? "  arp:ok" : "  arp:no";
    while (*ar) line[i++] = *ar++;
    line[i++] = ' '; line[i++] = 'r'; line[i++] = 'x'; line[i++] = ':';
    net_put_dec(line, &i, net_rx_packets);
    line[i++] = ' '; line[i++] = 't'; line[i++] = 'x'; line[i++] = ':';
    net_put_dec(line, &i, net_tx_packets);
    line[i] = 0;
    return line;
}

#endif
