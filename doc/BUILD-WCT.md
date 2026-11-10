# 如何编译 .wct 可执行文件

.wct 是 wcteos 的原生可执行格式（PE 风格外壳 + 裸代码段）。

## 1. 写代码
```c
/* hello.c —— 必须用 Windows x64 ABI（首参 RCX） */
int wct_main(void *api) { ((void(*)(const char*))api)("hello from .wct"); return 0; }
```

## 2. 用 MinGW 交叉编译（不要在 Linux gcc 下编，SysV ABI 首参是 RDI）
```bash
x86_64-w64-mingw32-gcc -ffreestanding -O2 -c hello.c -o hello.o
x86_64-w64-mingw32-ld -e wct_main --image-base 0x00400000 -o hello.elf hello.o
x86_64-w64-mingw32-objcopy -O binary hello.elf hello.bin
```

## 3. 取入口偏移（关键）
入口偏移 = `nm` 里 `wct_main` 的地址 − `objdump -h` 里 **`.text` 段 VMA**。
不能用“第一个 CONTENTS 段”去算，否则会匹配到 `.comment` 得到 0。

## 4. 打壳
```bash
python3 build-wct.py hello.bin <入口偏移> hello.wct 0x00400000
```
`build-wct.py` 在仓库 `wct/` 目录；产物可直接双击运行，文件名随意，后缀必须是 `.wct`。

## 5. 相关格式
- `.dwc` 系统依赖库（等价 .dll）
- `.wet` 一键脚本（等价 .bat/.sh）
- `.wcte` 系统压缩包，仅 wcteos 内可用：`wctetool c <in> <out.wcte>` / `wctetool d <in.wcte> <out>`
