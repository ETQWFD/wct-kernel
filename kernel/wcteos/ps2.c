/*
 * ps2.c —— WCT 内核原生 PS/2 鼠标驱动（i8042 轮询模式）
 *
 * 不依赖固件指针协议：直接操作 0x60/0x64 端口。
 * 用途：无 USB HID 指针协议的环境（部分虚拟机/老主板/安全启动固件）也能有鼠标。
 * 输出：相对位移 + 按键状态；由 gui.inc 的 cursor 逻辑消费。
 */
#include "efi.h"

static inline unsigned char p_inb(unsigned short p)
{
    unsigned char v;
    __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void p_outb(unsigned short p, unsigned char v)
{
    __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p));
}

static void io_wait(void) { p_outb(0x80, 0); }

static int ps2_wait_write(void)
{
    for (int t = 0; t < 100000; ++t) {
        if (!(p_inb(0x64) & 0x02)) return 1;
    }
    return 0;
}
static int ps2_wait_read(void)
{
    for (int t = 0; t < 100000; ++t) {
        if (p_inb(0x64) & 0x01) return 1;
    }
    return 0;
}

static int ps2_cmd(unsigned char cmd)
{
    if (!ps2_wait_write()) return 0;
    p_outb(0x64, cmd);
    return 1;
}
static int ps2_cmd_arg(unsigned char cmd, unsigned char arg)
{
    if (!ps2_cmd(cmd)) return 0;
    if (!ps2_wait_write()) return 0;
    p_outb(0x60, arg);
    return 1;
}
/* 期望从 0x60 读到指定 ACK(0xFA) */
static int ps2_read_ack(void)
{
    for (int t = 0; t < 16; ++t) {
        if (!ps2_wait_read()) return 0;
        unsigned char v = p_inb(0x60);
        if (v == 0xFA) return 1;
    }
    return 0;
}

static unsigned char g_pkt[3];
static int g_pkt_i = 0;
static int g_ready = 0;
static int g_overflow = 0;

int ps2_mouse_init(void)
{
    /* 关闭两个端口、清输出缓冲 */
    ps2_cmd(0xAD);
    ps2_cmd(0xA7);
    for (int i = 0; i < 64 && (p_inb(0x64) & 0x01); ++i) (void)p_inb(0x60);  /* 有界清空，避免空口死循环 */

    /* 读配置字节，开启 AUX 中断位与时钟 */
    ps2_cmd(0x20);
    if (!ps2_wait_read()) return 0;
    unsigned char cfg = p_inb(0x60);
    cfg |= 0x03;      /* bit0 INT1 (鼠标)，bit1 INT2 (键盘) */
    cfg &= (unsigned char)~0x30;   /* 开启时钟 */
    ps2_cmd_arg(0x60, cfg);

    ps2_cmd(0xA8);    /* 启用 AUX 口 */
    ps2_cmd_arg(0x20, cfg);

    /* 鼠标：默认设置 + 启用数据上报（采样 100/s，分辨率 4 计数/mm） */
    if (!ps2_cmd(0xD4)) return 0;      /* 下一条命令发往鼠标 */
    if (!ps2_cmd_arg(0xF6, 0)) return 0;
    (void)ps2_read_ack();
    if (!ps2_cmd(0xD4)) return 0;
    if (!ps2_cmd_arg(0xF3, 100)) return 0;
    (void)ps2_read_ack();
    if (!ps2_cmd(0xD4)) return 0;
    if (!ps2_cmd_arg(0xE8, 3)) return 0;
    (void)ps2_read_ack();
    if (!ps2_cmd(0xD4)) return 0;
    if (!ps2_cmd_arg(0xF4, 0)) return 0;
    if (!ps2_read_ack()) {
        g_ready = 0;
        return 0;
    }
    g_pkt_i = 0;
    g_ready = 1;
    return 1;
}

int ps2_mouse_ready(void) { return g_ready; }

/* 轮询：返回 1 表示取到一个完整包，dx/dy 为相对位移，btn 为按键位 (bit0 左,1 右,2 中) */
int ps2_mouse_poll(int *dx, int *dy, int *btn)
{
    if (!g_ready) return 0;
    int guard = 0;
    while ((p_inb(0x64) & 0x21) == 0x21 && guard++ < 64) {
        unsigned char st = p_inb(0x64);
        if (!(st & 0x01)) break;
        int from_aux = (st & 0x20) ? 1 : 0;
        unsigned char v = p_inb(0x60);
        if (!from_aux) continue;                 /* 键盘字节，交给键盘路径 */

        if (g_pkt_i == 0 && !(v & 0x08)) { g_pkt_i = 0; continue; }  /* 丢弃非包头 */
        g_pkt[g_pkt_i++] = v;
        if (g_pkt_i == 3) {
            g_pkt_i = 0;
            int b = g_pkt[0];
            int ddx = (int)g_pkt[1] - ((b & 0x10) ? 256 : 0);
            int ddy = (int)g_pkt[2] - ((b & 0x20) ? 256 : 0);
            if (b & 0x40) g_overflow = 1;
            if (b & 0x80) { g_overflow = 1; continue; }   /* 溢出包丢弃 */
            *dx = ddx;
            *dy = -ddy;                                  /* PS/2 Y 轴朝上为正，屏幕朝下为正 */
            *btn = (b & 0x01) | ((b & 0x02) << 1) | ((b & 0x04) >> 1);
            return 1;
        }
    }
    return 0;
}

int ps2_mouse_overflowed(void) { int o = g_overflow; g_overflow = 0; return o; }

/* 让内核“喂”一次，测试无固件指针时是否仍能移动光标 */
int ps2_mouse_step(void) { return g_ready; }
