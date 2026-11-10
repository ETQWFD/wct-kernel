#!/bin/bash
# 用 ESP 镜像做一张 UEFI 光盘镜像（El Torito EFI + 混合 GPT）
set -e
ESP=${1:-wcteos-esp.img}
OUT=${2:-wcteos.iso}
ROOT=$(mktemp -d)
cp "$ESP" "$ROOT/efi.img"
printf 'wcteos 0.9-test (UEFI)\nBOOTX64.EFI / BOOTIA32.EFI\n' > "$ROOT/README.txt"
xorriso -as mkisofs -o "$OUT" -V WCTEOS \
  -eltorito-platform efi -eltorito-boot efi.img -no-emul-boot -boot-load-size 4 \
  -append_partition 2 0xef efi.img -partition_offset 16 -isohybrid-gpt-basdat \
  "$ROOT"
rm -rf "$ROOT"
echo "已生成 $OUT"
