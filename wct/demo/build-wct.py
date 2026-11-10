#!/usr/bin/env python3
"""把 ELF/裸二进制封装成 .wct；结构见 kernel/wctmod.c 顶部注释"""
import struct, sys, zlib

def main():
    if len(sys.argv) not in (4, 5):
        print("用法: build-wct.py <code.bin> <entry_offset> <out.wct>")
        return 2
    code = open(sys.argv[1], "rb").read()
    entry_off = int(sys.argv[2], 0)
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x00400000
    hdr = bytearray(24)
    hdr[0:4] = b"WCT1"
    hdr[4] = 1
    hdr[5] = 0
    struct.pack_into("<H", hdr, 6, 1)          # 段数
    struct.pack_into("<I", hdr, 8, base)
    struct.pack_into("<I", hdr, 12, entry_off)
    struct.pack_into("<I", hdr, 16, len(code))
    struct.pack_into("<I", hdr, 20, zlib.crc32(code) & 0xFFFFFFFF)
    with open(sys.argv[3], "wb") as f:
        f.write(hdr); f.write(code)
    print("wct: %s  code=%d base=0x%08X entry=+0x%X" % (sys.argv[3], len(code), base, entry_off))
    return 0

sys.exit(main())
