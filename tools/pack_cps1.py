#!/usr/bin/env python3
"""
CPS-1 ROM Packager for Sega NAOMI (Final Fight / ffight.zip)
------------------------------------------------------------
Unpacks a MAME CPS-1 ffight.zip ROM archive, interleaves the 68000 program ROMs,
pre-decodes the 2 MB planar CPS-1 graphics ROMs into 32-bit packed 4bpp words
plus pen-usage bitmasks, and writes a single flat bundle `romfs/ffight.cps1`.
"""

import os
import sys
import struct
import zipfile

CPS1_MAGIC = b"CPS1"


def load_member(zf: zipfile.ZipFile, name: str, expected_size: int) -> bytes:
    for info in zf.infolist():
        if os.path.basename(info.filename).lower() == name.lower():
            data = zf.read(info.filename)
            if len(data) != expected_size:
                raise ValueError(f"{name}: expected {expected_size} bytes, got {len(data)}")
            return data
    raise FileNotFoundError(f"Missing required ROM '{name}' in zip archive")


def interleave_16bit(high_bytes: bytes, low_bytes: bytes) -> bytearray:
    assert len(high_bytes) == len(low_bytes)
    out = bytearray(len(high_bytes) * 2)
    out[0::2] = high_bytes
    out[1::2] = low_bytes
    return out


def byteswap_16bit(data: bytes) -> bytearray:
    out = bytearray(len(data))
    out[0::2] = data[1::2]
    out[1::2] = data[0::2]
    return out


def decode_cps1_gfx(b_1m: bytes, b_5m: bytes, b_3m: bytes, b_7m: bytes):
    """
    Exact implementation of MAME's cps1_gfx_start() planar-to-packed 4bpp converter
    plus pen-usage tables for 8x8 chars, 16x16 tiles, and 32x32 tiles.
    """
    # Build lookup table for a single byte across 8 pixels (each pixel shifted into 4-bit position)
    # For a byte B: bit (7-j) goes to nibble (7-j), i.e. shift (28 - j*4)
    byte_to_dw = [0] * 256
    for b in range(256):
        val = 0
        for j in range(8):
            if b & (0x80 >> j):
                val |= 1 << (28 - j * 4)
        byte_to_dw[b] = val

    # Pen usage of a 32-bit dwval (8 pixels of 4 bits each)
    def dw_penusage(dw: int) -> int:
        pu = 0
        for j in range(8):
            n = (dw >> (28 - j * 4)) & 0x0F
            pu |= 1 << n
        return pu

    quarter = 0x80000  # 512 KB
    num_pairs = quarter // 2  # 262,144 pairs = 524,288 uint32 words
    max_char = num_pairs // 8     # 32,768
    max_tile16 = num_pairs // 16  # 16,384
    max_tile32 = num_pairs // 64  # 4,096

    gfx_words = [0] * (num_pairs * 2)
    char_pu = [0] * max_char
    tile16_pu = [0] * max_tile16
    tile32_pu = [0] * max_tile32

    for i in range(num_pairs):
        off = i * 2
        nchar = i >> 3

        # Word 2*i uses b_5m (planes 0,1) and b_7m (planes 2,3)
        dw0 = (
            byte_to_dw[b_5m[off]]
            | (byte_to_dw[b_5m[off + 1]] << 1)
            | (byte_to_dw[b_7m[off]] << 2)
            | (byte_to_dw[b_7m[off + 1]] << 3)
        )
        # Word 2*i+1 uses b_1m (planes 0,1) and b_3m (planes 2,3)
        dw1 = (
            byte_to_dw[b_1m[off]]
            | (byte_to_dw[b_1m[off + 1]] << 1)
            | (byte_to_dw[b_3m[off]] << 2)
            | (byte_to_dw[b_3m[off + 1]] << 3)
        )

        gfx_words[2 * i] = dw0
        gfx_words[2 * i + 1] = dw1

        pu = dw_penusage(dw0) | dw_penusage(dw1)
        char_pu[nchar] |= pu
        tile16_pu[nchar >> 1] |= pu
        tile32_pu[nchar >> 3] |= pu

    return gfx_words, char_pu, tile16_pu, tile32_pu


def main():
    zip_path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads/ffight.zip")
    out_path = sys.argv[2] if len(sys.argv) > 2 else "romfs/ffight.cps1"

    print(f"[1/4] Reading {zip_path}...")
    with zipfile.ZipFile(zip_path, "r") as zf:
        # 1. 68000 Program ROM (1 MB)
        ff_36 = load_member(zf, "ff_36.11f", 0x20000)
        ff_42 = load_member(zf, "ff_42.11h", 0x20000)
        ff_37 = load_member(zf, "ff_37.12f", 0x20000)
        ffe_43 = load_member(zf, "ffe_43.12h", 0x20000)
        ff_32m = load_member(zf, "ff-32m.8h", 0x80000)

        m68k_rom = bytearray()
        m68k_rom.extend(interleave_16bit(ff_36, ff_42))
        m68k_rom.extend(interleave_16bit(ff_37, ffe_43))
        m68k_rom.extend(byteswap_16bit(ff_32m))
        assert len(m68k_rom) == 0x100000

        ssp, pc = struct.unpack(">II", m68k_rom[:8])
        print(f"      68000 ROM: 1024 KB (Reset SSP=0x{ssp:08X}, PC=0x{pc:08X})")

        # 2. CPS-1 Graphics ROMs (2 MB)
        b_1m = load_member(zf, "ff-1m.3a", 0x80000)
        b_5m = load_member(zf, "ff-5m.7a", 0x80000)
        b_3m = load_member(zf, "ff-3m.5a", 0x80000)
        b_7m = load_member(zf, "ff-7m.9a", 0x80000)

        # 3. Z80 Sound ROM (64 KB)
        z80_rom = load_member(zf, "ff_09.12b", 0x10000)

        # 4. OKIM6295 ADPCM ROM (256 KB)
        oki_18 = load_member(zf, "ff_18.11c", 0x20000)
        oki_19 = load_member(zf, "ff_19.12c", 0x20000)
        oki_rom = oki_18 + oki_19
        assert len(oki_rom) == 0x40000

    print("[2/4] Decoding 2 MB CPS-1 planar graphics + pen usage tables...")
    gfx_words, char_pu, tile16_pu, tile32_pu = decode_cps1_gfx(b_1m, b_5m, b_3m, b_7m)

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    print(f"[3/4] Writing {out_path}...")
    header = struct.pack(
        "<4sIIIIIII",
        CPS1_MAGIC,
        len(m68k_rom),
        len(gfx_words) * 4,
        len(z80_rom),
        len(oki_rom),
        len(char_pu),
        len(tile16_pu),
        len(tile32_pu),
    )

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(m68k_rom)
        f.write(struct.pack(f"<{len(gfx_words)}I", *gfx_words))
        f.write(struct.pack(f"<{len(char_pu)}H", *char_pu))
        f.write(struct.pack(f"<{len(tile16_pu)}H", *tile16_pu))
        f.write(struct.pack(f"<{len(tile32_pu)}H", *tile32_pu))
        f.write(z80_rom)
        f.write(oki_rom)

    total_bytes = os.path.getsize(out_path)
    print(f"[4/4] Done! Wrote {total_bytes:,} bytes ({total_bytes / (1024*1024):.2f} MB) to {out_path}")


if __name__ == "__main__":
    main()
