#!/bin/bash
# 构建 wcteos Pro 安装 ISO：ESP(FAT32) + El Torito EFI 引导 + \wct\instmode 标记
set -e
cd "$(dirname "$0")/.."
KERNEL=${1:-prebuilt/wcteos-x64.efi}
OUT=${2:-dist/wcteos-pro-installer.iso}
mkdir -p build dist
rm -f build/esp.img
truncate -s 48M build/esp.img
mkfs.fat -F32 build/esp.img >/dev/null
mmd -i build/esp.img ::/EFI ::/EFI/BOOT ::/wct
mcopy -i build/esp.img "$KERNEL" ::/EFI/BOOT/BOOTX64.EFI
printf 'installer\n' > build/instmode
mcopy -i build/esp.img build/instmode ::/wct/instmode
rm -rf build/iso_root
mkdir -p build/iso_root
cp build/esp.img build/iso_root/efi.img
printf 'wcteos Pro installer 1.0\nUEFI only\n' > build/iso_root/README.txt
xorriso -as mkisofs -o "$OUT" -V WCTEOS_P \
  -eltorito-platform efi -eltorito-boot efi.img -no-emul-boot -boot-load-size 4 \
  -iso-level 3 build/iso_root
echo "built: $OUT ($(stat -c%s "$OUT") B)"
