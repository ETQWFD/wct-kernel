#!/bin/bash
# 生成可启动的 FAT32 ESP 镜像（U 盘直接 dd 即可）
set -e
OUT=${1:-wcteos-esp.img}
SIZE=${2:-48M}
KERNEL_X64=${3:-build_wctk.efi}
KERNEL_IA32=${4:-build_wctk32.efi}

rm -f "$OUT"
truncate -s "$SIZE" "$OUT"
mkfs.fat -F32 "$OUT" >/dev/null
mmd -i "$OUT" ::/EFI ::/EFI/BOOT ::/wct ::/wct/wall
mcopy -i "$OUT" "$KERNEL_X64"  ::/EFI/BOOT/BOOTX64.EFI
[ -f "$KERNEL_IA32" ] && mcopy -i "$OUT" "$KERNEL_IA32" ::/EFI/BOOT/BOOTIA32.EFI
printf 'fs0:\\EFI\\BOOT\\BOOTX64.EFI\r\nfs1:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/_startup.nsh
mcopy -i "$OUT" /tmp/_startup.nsh ::/startup.nsh
: > /tmp/_autogui
mcopy -i "$OUT" /tmp/_autogui ::/wct/autogui
printf 'wcteos 0.9-test\n' > /tmp/_install.txt
mcopy -i "$OUT" /tmp/_install.txt ::/wct/install.txt
echo "已生成 $OUT"
