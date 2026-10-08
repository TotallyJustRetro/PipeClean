#!/usr/bin/env python3
"""Embed two TTF files into runtime/font_data.c.

Usage:
    python3 tools/make_font.py Poppins-Regular.ttf Poppins-Bold.ttf
"""
import pathlib
import sys


def c_array(name, data):
    lines = [f"const unsigned char {name}[] = {{"]
    for i in range(0, len(data), 24):
        lines.append("".join(f"{b}," for b in data[i:i + 24]))
    lines.append("};")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_font.py REGULAR.ttf BOLD.ttf")

    regular = pathlib.Path(sys.argv[1]).read_bytes()
    bold = pathlib.Path(sys.argv[2]).read_bytes()

    out = [
        "/* Poppins (SIL Open Font License 1.1) embedded for the launcher UI. */",
        '#include "font.h"',
        "",
        c_array("font_regular", regular),
        f"const unsigned int font_regular_len = {len(regular)};",
        "",
        c_array("font_bold", bold),
        f"const unsigned int font_bold_len = {len(bold)};",
        "",
    ]
    pathlib.Path("runtime/font_data.c").write_text("\n".join(out), encoding="utf-8")
    print(f"embedded Poppins: regular={len(regular)} bytes, bold={len(bold)} bytes")


if __name__ == "__main__":
    main()
