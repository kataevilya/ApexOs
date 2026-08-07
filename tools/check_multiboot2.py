#!/usr/bin/env python3
"""
check_multiboot2.py — регрессионный тест для NikiOS kernel.elf.

Проверяет пункты 1 и 2 из обязательного чек-листа boot path:
  1. Multiboot2 header найден и полностью помещается в первые 32 KiB файла.
  2. checksum корректен: magic + architecture + header_length + checksum == 0 (mod 2^32).

Не зависит от grub-file/grub-mkrescue — работает на голом файле ELF,
поэтому его можно гонять в любом CI без установленного GRUB.
"""

import struct
import sys

MB2_MAGIC = 0xE85250D6
SEARCH_LIMIT = 32768  # первые 32 KiB файла, требование спецификации


def find_and_check_header(data: bytes) -> None:
    limit = min(len(data), SEARCH_LIMIT)

    for offset in range(0, limit - 16 + 1, 8):  # header 8-byte aligned
        magic, arch, header_length, checksum = struct.unpack_from(
            "<IIII", data, offset
        )
        if magic != MB2_MAGIC:
            continue

        # Нашли кандидата — header должен ПОЛНОСТЬЮ помещаться в первые 32 KiB.
        if offset + header_length > SEARCH_LIMIT:
            print(
                f"FAIL: multiboot2 header at offset {offset} has length "
                f"{header_length}, ending at {offset + header_length}, "
                f"which exceeds the {SEARCH_LIMIT}-byte limit"
            )
            sys.exit(1)

        total = (magic + arch + header_length + checksum) & 0xFFFFFFFF
        if total != 0:
            print(
                f"FAIL: multiboot2 checksum invalid at offset {offset}: "
                f"(magic + arch + header_length + checksum) mod 2^32 = {total}, "
                f"expected 0"
            )
            sys.exit(1)

        print(f"OK: multiboot2 header found at file offset {offset}")
        print(f"    architecture    = {arch} (0 = i386/protected mode)")
        print(f"    header_length   = {header_length}")
        print(f"    checksum        = 0x{checksum:08x} (valid)")
        return

    print(f"FAIL: no valid multiboot2 header found in first {SEARCH_LIMIT} bytes")
    sys.exit(1)


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <kernel.elf>")
        sys.exit(2)

    with open(sys.argv[1], "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        print("FAIL: input is not an ELF file")
        sys.exit(1)

    # ELF64 sanity: EI_CLASS byte (offset 4) должен быть 2 (ELFCLASS64)
    if data[4] != 2:
        print("FAIL: not an ELF64 file (expected x86_64 kernel)")
        sys.exit(1)

    # entry point (e_entry) — offset 24, 8 bytes little-endian для ELF64
    (entry,) = struct.unpack_from("<Q", data, 24)
    print(f"INFO: ELF64 entry point = 0x{entry:x}")

    find_and_check_header(data)
    print("PASS: multiboot2 header checks OK")


if __name__ == "__main__":
    main()
