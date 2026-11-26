#!/usr/bin/env python3
"""Embed the Kawari PNG sizes used by the native window and toolbar."""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("icons", type=Path)
    parser.add_argument("header", type=Path)
    args = parser.parse_args()

    lines = ["#pragma once", "", "namespace studio { namespace app_icon {", ""]
    for size in (16, 32, 48, 64):
        data = (args.icons / f"{size}.png").read_bytes()
        lines.append(f"constexpr unsigned char png_{size}[] = {{")
        for offset in range(0, len(data), 16):
            values = ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16])
            lines.append(f"    {values},")
        lines.extend(["};", ""])
    lines.extend(["}}", ""])
    args.header.write_text("\n".join(lines), encoding="ascii")


if __name__ == "__main__":
    main()
