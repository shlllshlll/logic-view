#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 V3F / V5F 两份 APP 固件合并成 IAP 升级用的 CH32H417_Logic_Analyzer_APP.bin。

布局（相对 Flash 0x00006000，IAP 的 FLASH_Base）：
    偏移 0x00000  V3F APP   (Flash 0x00006000)
    偏移 0x1A000  V5F APP   (Flash 0x00020000)   = 0x20000 - 0x6000
    空隙用 0xFF 填充，文件尾部补齐到 16 字节对齐

用法：
    python3 tools/make_app_bin.py
    python3 tools/make_app_bin.py --v3f <path> --v5f <path> --out <path>
"""

import argparse
import os

BASE_ADDR = 0x6000      # IAP 写入起始地址
V5F_ADDR = 0x20000      # V5F APP 的链接地址
FILL = 0xFF


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    app = os.path.join(root, "CH32H417_Logic_Analyzer_APP")

    ap = argparse.ArgumentParser()
    ap.add_argument("--v3f", default=os.path.join(app, "V3F", "obj",
                                                 "CH32H417_Logic_Analyzer_V3F.bin"))
    ap.add_argument("--v5f", default=os.path.join(app, "V5F", "obj",
                                                 "CH32H417_Logic_Analyzer_V5F.bin"))
    ap.add_argument("--out", default=os.path.join(app, "V5F", "obj",
                                                 "CH32H417_Logic_Analyzer_APP.bin"))
    args = ap.parse_args()

    for p in (args.v3f, args.v5f):
        if not os.path.exists(p):
            raise SystemExit("找不到输入文件: %s" % p)

    v3f = open(args.v3f, "rb").read()
    v5f = open(args.v5f, "rb").read()

    off = V5F_ADDR - BASE_ADDR
    size = off + len(v5f)
    size = (size + 15) & ~15                  # 16 字节对齐（与作者产物一致）

    buf = bytearray([FILL]) * size
    buf[0:len(v3f)] = v3f
    buf[off:off + len(v5f)] = v5f

    with open(args.out, "wb") as f:
        f.write(buf)

    print("V3F  : %6d 字节  -> 偏移 0x%05X (Flash 0x%05X)" % (len(v3f), 0, BASE_ADDR))
    print("V5F  : %6d 字节  -> 偏移 0x%05X (Flash 0x%05X)" % (len(v5f), off, V5F_ADDR))
    print("合并 : %6d 字节  -> %s" % (size, args.out))


if __name__ == "__main__":
    main()
