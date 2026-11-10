#!/bin/bash
set -e
cd "$(dirname "$0")"
MINCC=x86_64-w64-mingw32-gcc   # 必须用 mingw：Windows x64 ABI（首参在 RCX）
$MINCC -ffreestanding -fno-stack-protector -fno-builtin -mno-red-zone -O2 -c hello.c -o hello.o
x86_64-w64-mingw32-ld -e wct_main --image-base 0x00400000 -o hello.elf hello.o 2>/dev/null
x86_64-w64-mingw32-objcopy -O binary hello.elf hello.bin
# objcopy 的裸二进制从“最低 CONTENTS 段 VMA”开始，入口偏移必须相对它算，
# 而不是相对链接基址（否则会跳进代码中段，触发 #UD）
VMA=$(x86_64-w64-mingw32-objdump -h hello.elf | awk '$2==".text"{print $4; exit}')
SYM=$(x86_64-w64-mingw32-nm hello.elf | awk '/ T wct_main$/{print $1}')
OFF=$((0x$SYM - 0x$VMA))
python3 build-wct.py hello.bin $OFF hello.wct 0x00400000
echo "vma=0x$VMA sym=0x$SYM entry_off=$OFF"
