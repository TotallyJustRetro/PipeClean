#!/usr/bin/env python3
"""Create synthetic build-time ROM metadata for PipeClean's WebAssembly target.

This is not a playable game ROM and is never copied into the web package. It is
used only by tools/recomp.py to generate the emulator interpreter at build time.
"""
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: make_emscripten_build_stub.py OUTPUT.gb", file=sys.stderr)
        return 2
    target = Path(sys.argv[1])
    target.parent.mkdir(parents=True, exist_ok=True)
    rom = bytearray(512 * 1024)
    rom[0x134:0x134 + 10] = b"PIPECLSTUB"
    rom[0x147] = 0x01
    rom[0x148] = 0x04
    rom[0x149] = 0x00
    target.write_bytes(rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
