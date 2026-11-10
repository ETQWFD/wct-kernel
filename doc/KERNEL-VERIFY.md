# wcteos 内核验证记录（2026-11-10）

宿主：Linux，QEMU 无 KVM（纯 TCG 软件模拟），OVMF 4M 固件。

## 已验证
- 启动横幅 `wcteos Native 1.0 / C / x86_64 UEFI / FAT32`，提示符 `wcteos:`
- 桌面标题 `wcteos Desktop`，右上 `wcteos 0.9-test | WCT kernel`，任务栏左 `MENU` 右 `NET`
- 开始菜单九项：Files / Terminal / Settings / Task Manager / Run / About (wtcer) / Lock / Restart / Shutdown
- 设置面板：五种主题色（Purple/Green/Matrix/Amber/Blue，默认 Blue）+ 四种壁纸（Solid/Gradient/Grid/Dynamic .glf）
- 动态壁纸：`\wct\wall\default.glf`（GLF1，128x96x4 帧，RGB332）真实加载并铺满缩放绘制
- 任务管理器：进程表 + New Process（跳运行框）+ Refresh + Close
- 运行框：`Run (administrator)`、Admin ON/OFF、OK/Cancel、提示 `wtr / wtcer / *.wct / *.wet`
- 系统属性（wtcer）：版本、内核说明（no Linux underneath）、arch、CPU vendor、磁盘/PCI 计数、build 日期
- 双架构编译：x64 124KB，ia32 159KB
- `.wcte` 往返校验通过（文本 2.42%、随机 100%、全零 2.34%）

## 已知限制
- 无 KVM，帧率与开机耗时受软件模拟影响，不能直接与实体机对比
- 网卡/声卡仅完成 PCI 枚举，驱动未落地
