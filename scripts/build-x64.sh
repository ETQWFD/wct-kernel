#!/bin/bash
set -e
cd "$(dirname "$0")/.."
mkdir -p build
x86_64-w64-mingw32-gcc -ffreestanding -fno-stack-protector -fno-builtin -mno-red-zone \
  -O2 -Ikernel -c kernel/kernel.c -o build/k.o
x86_64-w64-mingw32-gcc -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main \
  -Wl,--image-base=0x10000000 -Wl,--enable-reloc-section -Wl,--gc-sections \
  -o build/wctk.efi build/k.o -lgcc
echo "ok: build/wctk.efi ($(stat -c%s build/wctk.efi) B)"
