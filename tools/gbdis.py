#!/usr/bin/env python3
"""Small SM83 disassembly helper.

Usage:
    python3 tools/gbdis.py ROM START END [BANK]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import argparse  # noqa: E402
import sm83  # noqa: E402


def addr(a, bank):
    return a if a < 0x4000 else bank * 0x4000 + (a - 0x4000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("start")
    ap.add_argument("end")
    ap.add_argument("bank", nargs="?", default="1")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    s = int(args.start, 16)
    e = int(args.end, 16)
    bank = int(args.bank, 16)
    pc = s
    while pc < e:
        o = addr(pc, bank)
        op = rom[o]
        b1 = rom[o + 1] if o + 1 < len(rom) else 0
        b2 = rom[o + 2] if o + 2 < len(rom) else 0
        if op == 0xCB:
            i = sm83.decode(0xCB, b1, 0, pc)
        else:
            i = sm83.decode(op, b1, b2, pc)
        print(f"{pc:04X}: {' '.join('%02X' % rom[o + k] for k in range(i.length)):9} {i.text}")
        pc += i.length


if __name__ == "__main__":
    main()
