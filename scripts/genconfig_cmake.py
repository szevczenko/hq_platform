#!/usr/bin/env python3
"""Generate a CMake configuration fragment from a Kconfig defconfig."""

import argparse
from pathlib import Path

import kconfiglib


KCONFIG_BOOL = 3
KCONFIG_TRISTATE = 48


def cmake_quote(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kconfig", type=Path, help="Top-level Kconfig file")
    parser.add_argument("defconfig", type=Path, help="Configuration to load")
    parser.add_argument("output", type=Path, help="Generated CMake file")
    args = parser.parse_args()

    kconfig = kconfiglib.Kconfig(str(args.kconfig))
    kconfig.load_config(str(args.defconfig))

    lines = ["# Generated from Kconfig. Do not edit.", ""]
    for symbol in kconfig.unique_defined_syms:
        if symbol.type in (KCONFIG_BOOL, KCONFIG_TRISTATE):
            value = "ON" if symbol.str_value == "y" else "OFF"
        else:
            value = cmake_quote(symbol.str_value)
        lines.append(f'set(CONFIG_{symbol.name} "{value}")')

    args.output.parent.mkdir(parents=True, exist_ok=True)
    content = "\n".join(lines) + "\n"
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
