#!/bin/bash
set -e
cd "$(dirname "$0")/.."
mkdir -p build
i686-w64-mingw32-gcc -ffreestanding -fno-stack-protector -fno-builtin \
  -O2 -Ikernel -c kernel/kernel.c -o build/k32.o
# 32 位入口符号带下划线
i686-w64-mingw32-gcc -nostdlib -Wl,--subsystem,10 -Wl,-e,_efi_main \
  -Wl,--image-base=0x10000000 -Wl,--enable-reloc-section -Wl,--gc-sections \
  -o build/wctk32.efi build/k32.o -lgcc
echo "ok: build/wctk32.efi ($(stat -c%s build/wctk32.efi) B)"
