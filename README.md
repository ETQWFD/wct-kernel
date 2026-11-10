# WCT-Kernel

WCT-OS 自研内核（UEFI 原生，**不依赖 Linux 内核**）+ 自研系统压缩格式 `.wcte`。

```
kernel/        内核源码
  kernel.c     入口、内核 C 库、WCT Shell
  gui.inc      GOP 图形栈：窗口/任务栏/内置字体/桌面
  ata.c        磁盘：ATA PIO(识别/读扇区) + MBR/GPT 分区解析
  ps2.c        鼠标：i8042 PS/2 驱动（轮询）
  efi.h        UEFI 类型定义（自带，零外部依赖）
  font8x16.h   8x16 点阵字体
  UsbIo.h      EDK2 UsbIo 协议定义（USB HID 输入用）
compression/   .wcte 压缩格式：编码器/解码器/命令行/基准
boot/          原生 UEFI 引导器源码
prebuilt/      已编译 wctk.efi（x86_64）与 wctk32.efi（IA32）
scripts/       构建脚本
doc/           格式规范、基准数据、状态矩阵
```

## 能力状态（说清楚哪些真跑过、哪些没有）

| 模块 | 实现 | 验证 |
|---|---|---|
| GOP 图形栈（双缓冲、窗口、任务栏、光标） | 完成 | QEMU(OVMF) 实跑，截图见 doc/ |
| 桌面 5 应用：Files/Terminal/Settings/Theme/Exit | 完成 | 实跑，键盘 `s`/`1-5` 验过 |
| 5 套主题（默认 Blue） | 完成 | 实跑切主题截图 |
| 输入：USB HID → Simple Pointer → Absolute Pointer，三重回退 + 3 秒轮询等待 | 完成 | 键盘链路实跑；**无头 QEMU 指针事件注入不生效**（宿主侧限制，非内核逻辑） |
| PS/2 鼠标（i8042 直接寄存器编程） | 完成 | 代码级实现 + 交叉编译通过；QEMU 无头未做指针移动截图 |
| ATA PIO 识别 / LBA28 读扇区 | 完成 | 宿主单测跑真实镜像 |
| MBR 解析（4 主分区） | 完成 | 单测：`p1 0x83 @2048 +409600`、`p2 0x82 @411648 +204800` |
| GPT 解析（128 项，跨 32 扇区全读） | 完成 | 单测：2 分区 `@2048`、`@411648` 全部正确 |
| 32 位（IA32 UEFI）构建 | 完成（`wctk32.efi` 34,257 B） | **仅编译验证**：本机无 32 位 OVMF 固件，未实跑 |
| `.wcte` 编码/解码 + 块级 64 位校验 | 完成 | 逐字节比对通过，见 doc/BENCH.md |
| 网卡 / 声卡驱动 | **未实现** | — |
| 原生 WCTFS 读写、`.wct` 加载器 | **未实现** | — |

## 构建

```bash
# x86_64 UEFI（默认）
bash scripts/build-x64.sh
# IA32 UEFI
bash scripts/build-ia32.sh
# .wcte 工具
cd compression && gcc -O2 -o wctetool wcte.c wctetool.c && ./wctetool b <file>
```

## 关于驱动来源（Intel 官方代码）

Intel 公开的网卡驱动源码（e1000 / ixgbe / igb，`linux/` 与 `freebsd/` 目录，BSD-2-Clause 许可）可以在 Intel 官网下载页找到。但**把它并入本内核不是"拷文件"**：这些驱动依赖 Linux 的 PCI 子系统、DMA/中断框架、sk_buff、netdev 与工作队列，需要先在内核里搭出对应框架才能编译。因此本项目当前**未纳入任何 Intel 驱动代码**，也没有声称支持网卡/声卡。要做，正确顺序是：PCI 枚举 → MMIO/IO BAR 映射 → 中断(MSI/IOAPIC) → DMA 描述符环 → 再移植驱动。这属于后续阶段的工作量，不在本轮交付范围内。
