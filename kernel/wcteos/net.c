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

int net_probe(void)
{
    net_present = 0; net_ready = 0; net_link = 0; net_arp_ok = 0;
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
    if (!net_present) return 0;

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
    net_ready = 1;
    return 1;
}

static int net_send(const UINT8 *frame, UINT16 len)
{
    if (!net_ready || len > NET_BUF) return 0;
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
    if (!net_send(frame, 42)) return 0;

    UINT8 rx[NET_BUF]; UINT16 len = 0;
    for (int tries = 0; tries < 400; ++tries) {
        if (net_poll(rx, &len)) {
            if (len >= 42 && rx[12] == 0x08 && rx[13] == 0x06 && rx[20] == 0x00 && rx[21] == 0x02) {
                net_arp_ok = 1;
                return 1;
            }
        }
        ST->BootServices->Stall(2000);
    }
    return 0;
}

const char *net_status_line(void)
{
    static char line[96];
    int i = 0;
    if (!net_present) { return "net: no Intel NIC found"; }
    line[i++] = 'n'; line[i++] = 'e'; line[i++] = 't'; line[i++] = ':'; line[i++] = ' ';
    line[i++] = 'e'; line[i++] = '1'; line[i++] = '0'; line[i++] = '0'; line[i++] = '0'; line[i++] = ' ';
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
