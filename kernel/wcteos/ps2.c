/*
 * ps2.c —— WCT 内核原生 PS/2 鼠标驱动（i8042 轮询模式）
 *
 * 不依赖固件指针协议：直接操作 0x60/0x64 端口。
 * 用途：无 USB HID 指针协议的环境（部分虚拟机/老主板/安全启动固件）也能有鼠标。
 * 输出：相对位移 + 按键状态；由 gui.inc 的 cursor 逻辑消费。
 *
 * 设计约束：纯轮询，不开 IRQ；AUX 与键盘共享 0x60，键盘字节一律丢弃不消费
 * （键盘由固件 ConIn 负责，避免两个消费者抢同一端口）。
 */
#include "efi.h"

/* ---- i8042 端口与命令常量 ---- */
#define PS2_DATA        0x60
#define PS2_STATUS      0x64
#define PS2_CMD         0x64
#define PS2_ST_OUT_FULL 0x01
#define PS2_ST_IN_FULL  0x02
#define PS2_ST_AUX      0x20
#define PS2_ST_TIMEOUT  0x40
#define PS2_ST_PARITY   0x80

#define PS2_CMD_READ_CFG   0x20
#define PS2_CMD_WRITE_CFG  0x60
#define PS2_CMD_DISABLE_K  0xAD
#define PS2_CMD_DISABLE_M  0xA7
#define PS2_CMD_ENABLE_M   0xA8
#define PS2_CMD_SEND_AUX   0xD4

#define PS2_ACK   0xFA
#define PS2_RESEND 0xFE
#define PS2_ERROR  0xFC

#define PS2_DEV_RESET      0xF6   /* 恢复默认 */
#define PS2_DEV_SET_RATE   0xF3
#define PS2_DEV_SET_RES    0xE8
#define PS2_DEV_ENABLE     0xF4

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
        if (!(p_inb(PS2_STATUS) & PS2_ST_IN_FULL)) return 1;
    }
    return 0;
}
static int ps2_wait_read(void)
{
    for (int t = 0; t < 100000; ++t) {
        if (p_inb(PS2_STATUS) & PS2_ST_OUT_FULL) return 1;
    }
    return 0;
}

/* 发布命令字节到 0x64；老芯片需要一点恢复时间 */
static int ps2_cmd(unsigned char cmd)
{
    if (!ps2_wait_write()) return 0;
    p_outb(PS2_CMD, cmd);
    io_wait();
    return 1;
}
static int ps2_cmd_arg(unsigned char cmd, unsigned char arg)
{
    if (!ps2_cmd(cmd)) return 0;
    if (!ps2_wait_write()) return 0;
    p_outb(PS2_DATA, arg);
    io_wait();
    return 1;
}

/* 所有设备命令前先非阻塞排空残留字节，避免把上一轮数据当 ACK */
static void ps2_drain(void)
{
    for (int i = 0; i < 32; ++i) {
        unsigned char st = p_inb(PS2_STATUS);
        if (!(st & PS2_ST_OUT_FULL)) break;
        (void)p_inb(PS2_DATA);
    }
}

/* 等 ACK：0xFA 成功；0xFE 重发 / 0xFC 错误返回 0，由上层重试 */
static int ps2_read_ack(void)
{
    for (int t = 0; t < 16; ++t) {
        if (!ps2_wait_read()) return 0;
        unsigned char v = p_inb(PS2_DATA);
        if (v == PS2_ACK) return 1;
        if (v == PS2_RESEND || v == PS2_ERROR) return 0;
    }
    return 0;
}

static int ps2_dev_write(unsigned char v)
{
    if (!ps2_wait_write()) return 0;
    p_outb(PS2_DATA, v);
    io_wait();
    return 1;
}

/* 设备命令：先 0xD4 切 AUX，再把命令字节写 0x60；失败最多重发 3 次 */
static int ps2_dev_cmd(unsigned char cmd)
{
    for (int try = 0; try < 3; ++try) {
        ps2_drain();
        if (!ps2_cmd(PS2_CMD_SEND_AUX)) continue;
        if (!ps2_dev_write(cmd)) continue;
        if (ps2_read_ack()) return 1;
    }
    return 0;
}

/* 带参数命令：命令本身要 ACK，参数也要 ACK */
static int ps2_dev_cmd_arg(unsigned char cmd, unsigned char arg)
{
    for (int try = 0; try < 3; ++try) {
        ps2_drain();
        if (!ps2_cmd(PS2_CMD_SEND_AUX)) continue;
        if (!ps2_dev_write(cmd)) continue;
        if (!ps2_read_ack()) continue;
        if (!ps2_dev_write(arg)) continue;
        if (ps2_read_ack()) return 1;
    }
    return 0;
}

static unsigned char g_pkt[3];
static int g_pkt_i = 0;
static int g_ready = 0;
static int g_overflow = 0;

int ps2_mouse_init(void)
{
    unsigned char old_cfg = 0;

    /* 只关 AUX，键盘口留给固件 ConIn，否则安装菜单/Shell 收不到按键 */
    ps2_cmd(PS2_CMD_DISABLE_M);
    ps2_drain();

    /* 读配置字节；轮询模式：关 INT1/INT2，开时钟 */
    if (!ps2_cmd(PS2_CMD_READ_CFG)) return 0;
    if (!ps2_wait_read()) return 0;
    old_cfg = p_inb(PS2_DATA);
    unsigned char cfg = old_cfg;
    cfg &= (unsigned char)~0x30;   /* bit4/bit5 = 0，开启键盘/鼠标时钟；中断位保持固件原值 */
    if (!ps2_cmd_arg(PS2_CMD_WRITE_CFG, cfg)) {
        ps2_cmd_arg(PS2_CMD_WRITE_CFG, old_cfg);
        return 0;
    }

    ps2_cmd(PS2_CMD_ENABLE_M);   /* 启用 AUX 口 */

    if (!ps2_dev_cmd(PS2_DEV_RESET)) goto fail;
    if (!ps2_dev_cmd_arg(PS2_DEV_SET_RATE, 100)) goto fail;
    if (!ps2_dev_cmd_arg(PS2_DEV_SET_RES, 3)) goto fail;
    if (!ps2_dev_cmd(PS2_DEV_ENABLE)) goto fail;

    /* 丢弃开启上报前残留的包，避免首次 poll 解析到旧数据 */
    ps2_drain();
    g_pkt_i = 0;
    g_overflow = 0;
    g_ready = 1;
    return 1;

fail:
    g_ready = 0;
    ps2_cmd(PS2_CMD_DISABLE_M);
    ps2_cmd_arg(PS2_CMD_WRITE_CFG, old_cfg);   /* 失败时恢复 i8042 原配置 */
    return 0;
}

int ps2_mouse_ready(void) { return g_ready; }

/* 轮询：返回 1 表示取到一个完整包，dx/dy 为相对位移
 * btn 位：bit0 左键、bit1 中键、bit2 右键（与 X11 Button1/2/3 一致） */
int ps2_mouse_poll(int *dx, int *dy, int *btn)
{
    if (!g_ready) return 0;
    int guard = 0;
    while (guard++ < 64) {
        unsigned char st = p_inb(PS2_STATUS);
        if (!(st & PS2_ST_OUT_FULL)) break;
        if (!(st & PS2_ST_AUX)) break;           /* 键盘字节：不读不消费，留给固件键盘驱动 */
        unsigned char v = p_inb(PS2_DATA);

        if (g_pkt_i == 0 && !(v & 0x08)) { g_pkt_i = 0; continue; }  /* 丢弃非包头 */
        g_pkt[g_pkt_i++] = v;
        if (g_pkt_i == 3) {
            g_pkt_i = 0;
            int b = g_pkt[0];
            int ddx = (int)g_pkt[1] - ((b & 0x10) ? 256 : 0);
            int ddy = (int)g_pkt[2] - ((b & 0x20) ? 256 : 0);
            if (b & 0xC0) { g_overflow = 1; continue; }   /* X/Y 溢出：整包丢弃 */
            *dx = ddx;
            *dy = -ddy;                                  /* PS/2 Y 轴朝上为正，屏幕朝下为正 */
            *btn = (b & 0x07);
            return 1;
        }
    }
    return 0;
}

int ps2_mouse_overflowed(void) { int o = g_overflow; g_overflow = 0; return o; }

/* 让内核“喂”一次，测试无固件指针时是否仍能移动光标 */
int ps2_mouse_step(void) { return g_ready; }
