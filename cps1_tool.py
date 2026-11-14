#!/usr/bin/env python3
"""
================================================================================
CPS-1 Multi-Game Packager, Net DIMM Live Pusher & Jukebox Server (`cps1_tool.py`)
================================================================================
Supports BOTH workflows for running any Capcom CPS-1 game on Sega NAOMI:

1. Standalone .bin Packager (No C recompile needed - ~1 second per game):
     python3 cps1_tool.py build-bin ~/Downloads/ROMS/ffight.zip
     python3 cps1_tool.py build-bin ~/Downloads/ROMS/ -o ~/NAOMIGAMES/

2. Net DIMM "Mini Server" Live Push & Interactive Jukebox:
   First boot `naomi_cps1_server.bin` on your NAOMI (or pass `--boot-server`), then:
     python3 cps1_tool.py push  <NAOMI_IP> ~/Downloads/ROMS/sf2ja.zip
     python3 cps1_tool.py serve <NAOMI_IP> ~/Downloads/ROMS/

3. Inspect / Scan a ROM directory:
     python3 cps1_tool.py list ~/Downloads/ROMS/
================================================================================
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLCHAIN_DIR = os.environ.get(
    "NAOMI_BASE",
    os.path.abspath(os.path.join(SCRIPT_DIR, "..", "toolchain")),
)
PYTHON_NETBOOT = os.path.join(TOOLCHAIN_DIR, "tools/pyenv/bin/python3")
if not os.path.exists(PYTHON_NETBOOT):
    PYTHON_NETBOOT = sys.executable
ROMFSGEN_PY = os.path.join(TOOLCHAIN_DIR, "tools/romfsgen.py")
MAKEROM_PY = os.path.join(TOOLCHAIN_DIR, "tools/makerom.py")
ENGINE_BIN = os.path.join(SCRIPT_DIR, "build/naomi.bin")
SERVER_BIN = os.path.join(SCRIPT_DIR, "naomi_cps1_server.bin")

# ==============================================================================
# CPS-1 Hardware Definitions (matching src/cps1_emu.h)
# ==============================================================================
CPS1_BUNDLE_MAGIC_V2 = b"CPSM"
CPS1_BUNDLE_VER_V2   = 2  # V2 = Raw planar GFX in bundle (decoded on SH-4 in ~0.35s)

CPS1_INPUT_STD      = 0  # Standard 2/3-button CPS-1 (matches cps1_emu.h)
CPS1_INPUT_SF2      = 1  # Street Fighter II 6-button (extra kicks at 0x176/0x1FC)
CPS1_INPUT_STRIDER  = 2  # Strider (Coin 1/2 on bits 0x04/0x08)
CPS1_INPUT_FORGOTTN = 3  # Forgotten Worlds (analog dial at 0x52..0x5D)
CPS1_INPUT_STD2     = CPS1_INPUT_STD
CPS1_INPUT_STD3     = CPS1_INPUT_STD
CPS1_INPUT_4P       = CPS1_INPUT_STD
CPS1_INPUT_DIAL     = CPS1_INPUT_FORGOTTN

CPS1_FLAG_FFIGHT_IDLE = 0x01

# CPS-B Chip Profiles:
# (cpsb_addr, cpsb_val, mult1, mult2, m_lo, m_hi, layer_ctrl, pri0, pri1, pri2, pri3, ctrl_reg, s1_m, s2_m, s3_m)
CPSB_PROFILES = {
    "STANDARD":   (0x60, 0x0004, 0x00, 0x00, 0x00, 0x00, 0x6E, 0x66, 0x70, 0x68, 0x72, 0x6A, 0x02, 0x0C, 0x0C),
    "CPSB_01":    (0x00, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x66, 0x68, 0x6A, 0x6C, 0x6E, 0x70, 0x02, 0x04, 0x08),
    "CPSB_02":    (0x60, 0x0002, 0x00, 0x00, 0x00, 0x00, 0x6C, 0x6A, 0x68, 0x66, 0x64, 0x62, 0x02, 0x04, 0x08),
    "CPSB_03":    (0x00, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x70, 0x6E, 0x6C, 0x6A, 0x68, 0x66, 0x02, 0x04, 0x08),
    "CPSB_04":    (0x60, 0x0004, 0x00, 0x00, 0x00, 0x00, 0x6E, 0x66, 0x70, 0x68, 0x72, 0x6A, 0x02, 0x04, 0x08),
    "CPSB_05":    (0x60, 0x0005, 0x00, 0x00, 0x00, 0x00, 0x68, 0x6A, 0x6C, 0x6E, 0x70, 0x72, 0x02, 0x04, 0x08),
    "CPSB_11":    (0x72, 0x0401, 0x60, 0x62, 0x64, 0x66, 0x68, 0x6A, 0x6C, 0x6E, 0x70, 0x50, 0x02, 0x04, 0x08),
    "CPSB_12":    (0x60, 0x0402, 0x68, 0x6A, 0x6C, 0x6E, 0x70, 0x48, 0x4A, 0x4C, 0x4E, 0x66, 0x02, 0x04, 0x08),
    "CPSB_13":    (0x6E, 0x0403, 0x60, 0x62, 0x64, 0x66, 0x68, 0x6A, 0x6C, 0x54, 0x56, 0x70, 0x02, 0x04, 0x08),
    "CPSB_14":    (0x5E, 0x0404, 0x52, 0x54, 0x56, 0x58, 0x5A, 0x4A, 0x4C, 0x4E, 0x50, 0x48, 0x02, 0x04, 0x08),
    "CPSB_15":    (0x4E, 0x0405, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x52, 0x54, 0x56, 0x58, 0x4C, 0x02, 0x04, 0x08),
    "CPSB_16":    (0x40, 0x0406, 0x4C, 0x4E, 0x50, 0x52, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x54, 0x02, 0x04, 0x08),
    "CPSB_17":    (0x48, 0x0407, 0x00, 0x00, 0x00, 0x00, 0x54, 0x52, 0x50, 0x4E, 0x4C, 0x4A, 0x08, 0x12, 0x12),
    "CPSB_18":    (0x50, 0x0408, 0x5C, 0x5E, 0x60, 0x62, 0x52, 0x54, 0x56, 0x58, 0x5A, 0x4A, 0x02, 0x04, 0x08),
    "CPSB_21_DEF":(0x72, 0x0800, 0x4E, 0x4C, 0x4A, 0x48, 0x68, 0x66, 0x64, 0x62, 0x60, 0x70, 0x20, 0x04, 0x08),
    "SF2JA":      (0x6E, 0x0403, 0x60, 0x62, 0x64, 0x66, 0x68, 0x6A, 0x6C, 0x54, 0x56, 0x70, 0x02, 0x04, 0x08),
    "SF2CE":      (0x5E, 0x0404, 0x52, 0x54, 0x56, 0x58, 0x5A, 0x4A, 0x4C, 0x4E, 0x50, 0x48, 0x02, 0x04, 0x08),
    "SF2HF":      (0x48, 0x0407, 0x00, 0x00, 0x00, 0x00, 0x54, 0x52, 0x50, 0x4E, 0x4C, 0x4A, 0x08, 0x12, 0x12),
    "PANG3":      (0x72, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x66, 0x68, 0x6A, 0x6C, 0x6E, 0x70, 0x02, 0x04, 0x08),
}

# CPS-1 Gfx Bank & Range Profiles (matching MAME CPS1gfxConfig):
# (bank_s1, bank_s2, bank_s3, space_s1, start_s2, end_s2, start_s3, end_s3, kludge)
GFX_PROFILES = {
    "GFXB01":   (0, 0, 0,   -1, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "FFIGHT":   (0, 0, 0, 0x20, 0x0001, 0xFFFF, 0x0001, 0xFFFF, 0),
    "GHOULS":   (0, 1, 2,   -1, 0x0400, 0x1BFF, 0x1000, 0x1FFF, 1),
    "STRIDER":  (1, 0, 1,   -1, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "DYNAWAR":  (1, 0, 1,   -1, 0x1400, 0x2FFF, 0x0400, 0x07FF, 0),
    "WILLOW":   (0, 1, 0,   -1, 0x0000, 0x1FFF, 0x0800, 0x0FFF, 0),
    "UNSQUAD":  (0, 0, 0,   -1, 0x0000, 0x3BFF, 0x0F00, 0x0FFF, 0),
    "TFIGHT":   (0, 0, 0,   -1, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "MERCS":    (0, 0, 0,   -1, 0x0600, 0x5BFF, 0x0700, 0x17FF, 4),
    "MSWORD":   (0, 0, 0,   -1, 0x2800, 0x3BFF, 0x0000, 0x0BFF, 0),
    "Cawing":   (0, 1, 0,   -1, 0x0000, 0x2FFF, 0x0C00, 0x0FFF, 0),
    "NEMO":     (0, 0, 0,   -1, 0x0000, 0xFFFF, 0x0800, 0x0FFF, 0),
    "MTWINS":   (0, 0, 0,   -1, 0x0000, 0x2FFF, 0x0C00, 0x0FFF, 0),
    "SF2":      (2, 2, 2,   -1, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "3WONDERS": (0, 1, 1,   -1, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 2),
    "KOD":      (0, 0, 0, 0x20, 0x0000, 0xFFFF, 0x0900, 0x1FFF, 0),
    "CAPCOMM":  (0, 0, 0, 0x20, 0x0000, 0xFFFF, 0x0B00, 0x1FFF, 0),
    "KNIGHTS":  (0, 0, 0, 0x20, 0x0000, 0xFFFF, 0x0B00, 0x1FFF, 0),
    "VARTH":    (0, 0, 0,   -1, 0x0600, 0x7BFF, 0x0F00, 0x1FFF, 0),
    "CWORLD2J": (1, 1, 1,   -1, 0x0000, 0xFFFF, 0x0800, 0x0FFF, 0),
    "WOF":      (0, 0, 0, 0x20, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "DINO":     (0, 0, 0, 0x20, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "PUNISHER": (0, 0, 0, 0x20, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "SLAMMASA": (0, 0, 0, 0x20, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "MEGAMAN":  (0, 0, 0,   -1, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0),
    "PANG3":    (0, 0, 0,   -1, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 5),
    "FORGOTTN": (0, 1, 1,   -1, 0x0000, 0xFFFF, 0x0400, 0x07FF, 7),
}

# Master CPS-1 Game Table:
# short_name -> {
#   "title": str, "parent": str, "cpsb": str, "gfx": str,
#   "m68k_hz": int, "oki_hz": int, "input": int, "flags": int,
#   "dsw": (dswa, dswb, dswc), "decrypt": Optional[str]
# }
CPS1_GAMES = {
    "ffight":   {"title": "Final Fight (World)",               "cpsb": "STANDARD", "gfx": "FFIGHT",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": CPS1_FLAG_FFIGHT_IDLE},
    "ffightu":  {"title": "Final Fight (USA)",                 "cpsb": "STANDARD", "gfx": "FFIGHT",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": CPS1_FLAG_FFIGHT_IDLE, "parent": "ffight"},
    "ffightj":  {"title": "Final Fight (Japan)",               "cpsb": "STANDARD", "gfx": "FFIGHT",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": CPS1_FLAG_FFIGHT_IDLE, "parent": "ffight"},
    "sf2":      {"title": "Street Fighter II (World)",         "cpsb": "CPSB_11",  "gfx": "SF2",      "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0},
    "sf2eb":    {"title": "Street Fighter II (World 910214)",  "cpsb": "CPSB_11",  "gfx": "SF2",      "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2"},
    "sf2ua":    {"title": "Street Fighter II (USA 910206)",    "cpsb": "CPSB_11",  "gfx": "SF2",      "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2"},
    "sf2ja":    {"title": "Street Fighter II (Japan 910214)",  "cpsb": "CPSB_17",  "gfx": "SF2",      "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2"},
    "sf2j":     {"title": "Street Fighter II (Japan 911210)",  "cpsb": "CPSB_13",  "gfx": "SF2",      "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2"},
    "sf2ce":    {"title": "Street Fighter II' Champion Ed.",   "cpsb": "SF2CE",    "gfx": "SF2",      "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0},
    "sf2ceea":  {"title": "Street Fighter II' CE (World)",     "cpsb": "SF2CE",    "gfx": "SF2",      "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2ce"},
    "sf2ceua":  {"title": "Street Fighter II' CE (USA)",       "cpsb": "SF2CE",    "gfx": "SF2",      "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2ce"},
    "sf2ceja":  {"title": "Street Fighter II' CE (Japan)",     "cpsb": "SF2CE",    "gfx": "SF2",      "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2ce"},
    "sf2hf":    {"title": "Street Fighter II' Hyper Fighting", "cpsb": "SF2HF",    "gfx": "SF2",      "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0},
    "sf2t":     {"title": "Street Fighter II' Turbo (USA)",    "cpsb": "SF2HF",    "gfx": "SF2",      "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2hf"},
    "sf2tj":    {"title": "Street Fighter II' Turbo (Japan)",  "cpsb": "SF2HF",    "gfx": "SF2",      "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_SF2,  "flags": 0, "parent": "sf2hf"},
    "pang3":    {"title": "Pang! 3 (Euro 950601)",             "cpsb": "PANG3",    "gfx": "PANG3",    "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "decrypt": "pang3"},
    "pang3r1":  {"title": "Pang! 3 (Euro 950511)",             "cpsb": "PANG3",    "gfx": "PANG3",    "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "decrypt": "pang3", "parent": "pang3"},
    "pang3j":   {"title": "Pang! 3 (Japan 950511)",            "cpsb": "PANG3",    "gfx": "PANG3",    "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "decrypt": "pang3", "parent": "pang3"},
    "pang3n":   {"title": "Pang! 3 (Euro 950511 No-Suicide)",  "cpsb": "PANG3",    "gfx": "PANG3",    "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "parent": "pang3"},
    "ghouls":   {"title": "Ghouls'n Ghosts (World)",           "cpsb": "CPSB_01",  "gfx": "GHOULS",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "ghoulsu":  {"title": "Ghouls'n Ghosts (USA)",             "cpsb": "CPSB_01",  "gfx": "GHOULS",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "parent": "ghouls"},
    "daimakai": {"title": "Daimakaimura (Japan)",              "cpsb": "CPSB_01",  "gfx": "GHOULS",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "parent": "ghouls"},
    "strider":  {"title": "Strider (USA)",                     "cpsb": "CPSB_01",  "gfx": "STRIDER",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STRIDER, "flags": 0},
    "striderua":{"title": "Strider (USA, B-Board 89624B-2)",   "cpsb": "CPSB_01",  "gfx": "STRIDER",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STRIDER, "flags": 0, "parent": "strider"},
    "strideruc":{"title": "Strider (USA, B-Board 90629B-3)",   "cpsb": "CPSB_01",  "gfx": "STRIDER",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STRIDER, "flags": 0, "parent": "strider"},
    "striderj": {"title": "Strider Hiryu (Japan)",             "cpsb": "CPSB_01",  "gfx": "STRIDER",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STRIDER, "flags": 0, "parent": "strider"},
    "dynwar":   {"title": "Dynasty Wars (USA)",                "cpsb": "CPSB_02",  "gfx": "DYNAWAR",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD3, "flags": 0},
    "willow":   {"title": "Willow (World)",                    "cpsb": "CPSB_03",  "gfx": "WILLOW",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "unsquad":  {"title": "U.N. Squadron (USA)",               "cpsb": "CPSB_04",  "gfx": "UNSQUAD",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "area88":   {"title": "Area 88 (Japan)",                   "cpsb": "CPSB_04",  "gfx": "UNSQUAD",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "parent": "unsquad"},
    "mercs":    {"title": "Mercs (World)",                     "cpsb": "CPSB_04",  "gfx": "MERCS",    "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "msword":   {"title": "Magic Sword (World)",               "cpsb": "CPSB_12",  "gfx": "MSWORD",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "cawing":   {"title": "Carrier Air Wing (World)",          "cpsb": "CPSB_16",  "gfx": "Cawing",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "nemo":     {"title": "Nemo (World)",                      "cpsb": "CPSB_15",  "gfx": "NEMO",     "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "mtwins":   {"title": "Mega Twins (World)",                "cpsb": "CPSB_14",  "gfx": "MTWINS",   "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "3wonders": {"title": "Three Wonders (World)",             "cpsb": "CPSB_21_DEF","gfx": "3WONDERS","m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "wonder3":  {"title": "Wonder 3 (Japan 910520)",           "cpsb": "CPSB_21_DEF","gfx": "3WONDERS","m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0, "parent": "3wonders"},
    "kod":      {"title": "The King of Dragons (World)",       "cpsb": "CPSB_17",  "gfx": "KOD",      "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "captcomm": {"title": "Captain Commando (World)",          "cpsb": "CPSB_13",  "gfx": "CAPCOMM",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "knights":  {"title": "Knights of the Round (World)",      "cpsb": "CPSB_18",  "gfx": "KNIGHTS",  "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "varth":    {"title": "Varth: Operation Thunderstorm",     "cpsb": "CPSB_04",  "gfx": "VARTH",    "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_STD2, "flags": 0},
    "megaman":  {"title": "Mega Man: The Power Battle",        "cpsb": "CPSB_21_DEF","gfx": "MEGAMAN", "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_STD3, "flags": 0},
    "rockmanj": {"title": "Rockman: The Power Battle (Japan)", "cpsb": "CPSB_21_DEF","gfx": "MEGAMAN", "m68k_hz": 12000000, "oki_hz": 7576, "input": CPS1_INPUT_STD3, "flags": 0, "parent": "megaman"},
    "forgottn": {"title": "Forgotten Worlds (World)",          "cpsb": "CPSB_01",  "gfx": "FORGOTTN", "m68k_hz": 10000000, "oki_hz": 7576, "input": CPS1_INPUT_FORGOTTN, "flags": 0},
}


# ==============================================================================
# Explicit Known ROM Layouts & Pang! 3 Decryption
# ==============================================================================
def decrypt_pang3_68k(rom_bytes: bytearray):
    """
    Applies Pang! 3 (Euro/Japan) 68000 ROM decryption to 0x080000..0x0FFFFF:
    D15..D8 is unencrypted; D7..D0 (big-endian odd bytes) uses an 8x8 GF(2)
    linear transform on (enc ^ 0x88).
    """
    masks = (0x06, 0x20, 0x21, 0xC0, 0x08, 0x02, 0x18, 0x80)
    lut = bytearray(256)
    for c in range(256):
        x = c ^ 0x88
        p = 0
        for k in range(8):
            if (x & masks[k]).bit_count() & 1:
                p |= (1 << k)
        lut[c] = p

    n = len(rom_bytes)
    for addr in range(0x080001, min(n, 0x100000), 2):
        rom_bytes[addr] = lut[rom_bytes[addr]]


def interleave_16bit_pair(even_high: bytes, odd_low: bytes) -> bytes:
    """Interleaves two 8-bit ROMs into big-endian 16-bit 68000 words (even=high, odd=low)."""
    assert len(even_high) == len(odd_low)
    out = bytearray(len(even_high) * 2)
    out[0::2] = even_high
    out[1::2] = odd_low
    return bytes(out)


def byteswap_16bit_words(raw: bytes) -> bytes:
    """Swaps adjacent bytes in 16-bit words (converting little-endian 16-bit mask ROMs to big-endian 68000)."""
    out = bytearray(len(raw))
    out[0::2] = raw[1::2]
    out[1::2] = raw[0::2]
    return bytes(out)


def build_cps1_gfx_bank(b_1m: bytes, b_5m: bytes, b_3m: bytes, b_7m: bytes) -> bytes:
    """
    Packs 4 512 KB planar quarters in the exact sequential order expected by
    `cps1_decode_planar_bank()` in `src/cps1_emu.c`:
      [0..512KB): b_1m, [512KB..1MB): b_5m, [1MB..1.5MB): b_3m, [1.5MB..2MB): b_7m
    """
    assert len(b_1m) == 0x80000 and len(b_5m) == 0x80000 and len(b_3m) == 0x80000 and len(b_7m) == 0x80000
    return b_1m + b_5m + b_3m + b_7m


def split_2m_gfx_pair(r01m: bytes, r07m: bytes) -> bytes:
    """
    Splits two 2 MB 16-bit mask ROMs (used in Pang! 3: pa3-01m.2c and pa3-07m.2f)
    per MAME ROMX_LOAD(..., 0x100000, ROM_GROUPWORD | ROM_SKIP(6)) + ROM_CONTINUE(+4, 0x100000):
      r01m[0..1MB) -> b_5m (bank0 + bank1), r01m[1MB..2MB) -> b_1m (bank0 + bank1)
      r07m[0..1MB) -> b_7m (bank0 + bank1), r07m[1MB..2MB) -> b_3m (bank0 + bank1)
    """
    assert len(r01m) == 0x200000 and len(r07m) == 0x200000
    b5 = r01m[:0x100000]
    b1 = r01m[0x100000:]
    b7 = r07m[:0x100000]
    b3 = r07m[0x100000:]

    bank0 = build_cps1_gfx_bank(b1[:0x80000], b5[:0x80000], b3[:0x80000], b7[:0x80000])
    bank1 = build_cps1_gfx_bank(b1[0x80000:], b5[0x80000:], b3[0x80000:], b7[0x80000:])
    return bank0 + bank1


def find_zip_entry(zf_map: dict, candidates: list) -> bytes:
    for c in candidates:
        c_low = c.lower()
        if c_low in zf_map:
            return zf_map[c_low]
        for k, v in zf_map.items():
            if os.path.basename(k) == c_low or k.endswith("_" + c_low):
                return v
    raise KeyError(f"Missing ROM chip from candidates: {candidates}")


def try_pack_explicit_game(short_name: str, zf_map: dict):
    """
    Explicitly Verified MAME ROM Layouts for Key CPS-1 Sets (`ffight`, `sf2ja`, `pang3`, etc.)
    Returns (m68k_bytes, z80_bytes, oki_bytes, gfx_planar_bytes) or None if not an explicit match.
    """
    # 1. Final Fight (World / USA / Japan)
    if short_name in ("ffight", "ffightu", "ffightj"):
        try:
            p0 = interleave_16bit_pair(
                find_zip_entry(zf_map, ["ff_36.11f", "ffu_36.11f", "ffj_36.11f"]),
                find_zip_entry(zf_map, ["ff_42.11h", "ffu_42.11h", "ffj_42.11h"]),
            )
            p1 = interleave_16bit_pair(
                find_zip_entry(zf_map, ["ff_37.12f", "ffu_37.12f", "ffj_37.12f"]),
                find_zip_entry(zf_map, ["ffe_43.12h", "ff_43.12h", "ffu_43.12h", "ffj_43.12h"]),
            )
            p2 = byteswap_16bit_words(find_zip_entry(zf_map, ["ff-32m.8h", "ff_32m.8h"]))
            m68k = p0 + p1 + p2
            z80 = find_zip_entry(zf_map, ["ff_09.12b"])[:0x8000]
            oki = (
                find_zip_entry(zf_map, ["ff_18.11c"])
                + find_zip_entry(zf_map, ["ff_19.12c"])
            )
            gfx = build_cps1_gfx_bank(
                find_zip_entry(zf_map, ["ff-1m.3a", "ff_1m.3a"]),
                find_zip_entry(zf_map, ["ff-5m.7a", "ff_5m.7a"]),
                find_zip_entry(zf_map, ["ff-3m.5a", "ff_3m.5a"]),
                find_zip_entry(zf_map, ["ff-7m.9a", "ff_7m.9a"]),
            )
            return m68k, z80, oki, gfx
        except KeyError:
            pass

    # 2. Street Fighter II (World / USA / Japan `sf2`, `sf2ja`, `sf2eb`, `sf2ua`)
    if short_name in ("sf2", "sf2ja", "sf2eb", "sf2ua", "sf2j"):
        try:
            chunks_68k = []
            for hi_c, lo_c in [
                (["sf2j_30a.11e", "sf2e_30g.11e", "sf2_30a.11e", "sf2_30.11e", "sf2u_30a.11e"],
                 ["sf2j_37a.11f", "sf2e_37g.11f", "sf2_37a.11f", "sf2_37.11f", "sf2u_37a.11f"]),
                (["sf2j_31a.12e", "sf2e_31g.12e", "sf2_31a.12e", "sf2_31.12e", "sf2u_31a.12e"],
                 ["sf2j_38a.12f", "sf2e_38g.12f", "sf2_38a.12f", "sf2_38.12f", "sf2u_38a.12f"]),
                (["sf2j_28a.9e",  "sf2e_28g.9e",  "sf2_28a.9e",  "sf2_28.9e",  "sf2u_28a.9e"],
                 ["sf2j_35a.9f",  "sf2e_35g.9f",  "sf2_35a.9f",  "sf2_35.9f",  "sf2u_35a.9f"]),
                (["sf2j_29a.10e", "sf2e_29g.10e", "sf2_29a.10e", "sf2_29.10e", "sf2u_29a.10e"],
                 ["sf2j_36a.10f", "sf2e_36g.10f", "sf2_36a.10f", "sf2_36.10f", "sf2u_36a.10f"]),
            ]:
                chunks_68k.append(interleave_16bit_pair(
                    find_zip_entry(zf_map, hi_c),
                    find_zip_entry(zf_map, lo_c),
                ))
            try:
                ext_hi = find_zip_entry(zf_map, ["sf2j_27a.8e", "sf2_27a.8e", "sf2_27.8e"])
                ext_lo = find_zip_entry(zf_map, ["sf2j_34a.8f", "sf2_34a.8f", "sf2_34.8f"])
                chunks_68k.append(interleave_16bit_pair(ext_hi, ext_lo))
            except KeyError:
                pass

            m68k = b"".join(chunks_68k)
            z80 = find_zip_entry(zf_map, ["sf2j_09.12a", "sf2_9.12a", "sf2_09.12a"])[:0x8000]
            oki = (
                find_zip_entry(zf_map, ["sf2j_18.11c", "sf2_18.11c"])
                + find_zip_entry(zf_map, ["sf2j_19.12c", "sf2_19.12c"])
            )
            # 6 MB GFX (three 2 MB banks):
            # Support both EPROM sets (sf2_05.7a..sf2_27.10d) and mask ROM sets (sf2-1m..15m)
            if "sf2_05.7a" in zf_map:
                g_bank0 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["sf2_05.7a"]),
                    find_zip_entry(zf_map, ["sf2_06.8a"]),
                    find_zip_entry(zf_map, ["sf2_07.9a"]),
                    find_zip_entry(zf_map, ["sf2_08.10a"]),
                )
                g_bank1 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["sf2_14.7c"]),
                    find_zip_entry(zf_map, ["sf2_15.8c"]),
                    find_zip_entry(zf_map, ["sf2_16.9c"]),
                    find_zip_entry(zf_map, ["sf2_17.10c"]),
                )
                g_bank2 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["sf2_24.7d"]),
                    find_zip_entry(zf_map, ["sf2_25.8d"]),
                    find_zip_entry(zf_map, ["sf2_26.9d"]),
                    find_zip_entry(zf_map, ["sf2_27.10d"]),
                )
            else:
                g_bank0 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["sf2-1m.3a", "sf2_1m.3a"]),
                    find_zip_entry(zf_map, ["sf2-5m.4a", "sf2_5m.4a"]),
                    find_zip_entry(zf_map, ["sf2-3m.5a", "sf2_3m.5a"]),
                    find_zip_entry(zf_map, ["sf2-7m.6a", "sf2_7m.6a"]),
                )
                g_bank1 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["sf2-2m.3c", "sf2_2m.3c"]),
                    find_zip_entry(zf_map, ["sf2-6m.4c", "sf2_6m.4c"]),
                    find_zip_entry(zf_map, ["sf2-4m.5c", "sf2_4m.5c"]),
                    find_zip_entry(zf_map, ["sf2-8m.6c", "sf2_8m.6c"]),
                )
                g_bank2 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["sf2-9m.3d",  "sf2_9m.3d"]),
                    find_zip_entry(zf_map, ["sf2-13m.4d", "sf2_13m.4d"]),
                    find_zip_entry(zf_map, ["sf2-11m.5d", "sf2_11m.5d"]),
                    find_zip_entry(zf_map, ["sf2-15m.6d", "sf2_15m.6d"]),
                )
            return m68k, z80, oki, (g_bank0 + g_bank1 + g_bank2)
        except KeyError:
            pass

    # 3. Pang! 3 (Euro / Japan `pang3`, `pang3r1`, `pang3j`, `pang3n`)
    if short_name in ("pang3", "pang3r1", "pang3j", "pang3n"):
        try:
            p0 = byteswap_16bit_words(find_zip_entry(zf_map, ["pa3e_17a.11l", "pa3e_17.11l", "pa3j_17a.11l", "pa3n_17a.11l"]))
            p1 = byteswap_16bit_words(find_zip_entry(zf_map, ["pa3e_16a.10l", "pa3e_16.10l", "pa3j_16a.10l", "pa3n_16a.10l"]))
            m68k_ba = bytearray(p0 + p1)
            if short_name != "pang3n":
                decrypt_pang3_68k(m68k_ba)
            m68k = bytes(m68k_ba)
            z80 = find_zip_entry(zf_map, ["pa3_11.11f", "pa3_09.12b", "pa3_9.12b"])[:0x8000]
            oki = (
                find_zip_entry(zf_map, ["pa3_05.10d", "pa3_14.11c"])
                + find_zip_entry(zf_map, ["pa3_06.11d", "pa3_15.12c"])
            )
            gfx = split_2m_gfx_pair(
                find_zip_entry(zf_map, ["pa3-01m.2c", "pa3_01m.2c"]),
                find_zip_entry(zf_map, ["pa3-07m.2f", "pa3_07m.2f"]),
            )
            return m68k, z80, oki, gfx
        except KeyError:
            pass

    if short_name in ("strider", "striderj", "striderua", "strideruc"):
        try:
            p0 = interleave_16bit_pair(
                find_zip_entry(zf_map, ["st_36a.11f", "st_36.11f", "stj_36a.11f", "stj_36.11f", "st_36b.11f", "30.11f"]),
                find_zip_entry(zf_map, ["st_42a.11h", "st_42.11h", "stj_42a.11h", "stj_42.11h", "st_42b.11h", "35.11h"]),
            )
            p1 = interleave_16bit_pair(
                find_zip_entry(zf_map, ["st_37a.12f", "st_37.12f", "stj_37a.12f", "stj_37.12f", "st_37b.12f", "31.12f"]),
                find_zip_entry(zf_map, ["st_43a.12h", "st_43.12h", "stj_43a.12h", "stj_43.12h", "st_43b.12h", "36.12h"]),
            )
            try:
                p2 = byteswap_16bit_words(find_zip_entry(zf_map, ["st-32m.8h", "st_32m.8h", "st-14.8h"]))
            except KeyError:
                p2a = interleave_16bit_pair(
                    find_zip_entry(zf_map, ["st_34.9f", "stj_34.9f"]),
                    find_zip_entry(zf_map, ["st_40.9h", "stj_40.9h"]),
                )
                p2b = interleave_16bit_pair(
                    find_zip_entry(zf_map, ["st_35.10f", "stj_35.10f"]),
                    find_zip_entry(zf_map, ["st_41.10h", "stj_41.10h"]),
                )
                p2 = p2a + p2b
            m68k = p0 + p1 + p2
            z80 = find_zip_entry(zf_map, ["st_09.12b", "stj_09.12b", "09.12b"])[:0x8000]
            oki = (
                find_zip_entry(zf_map, ["st_18.11c", "stj_18.11c", "18.11c"])
                + find_zip_entry(zf_map, ["st_19.12c", "stj_19.12c", "19.12c"])
            )
            if "st-4.3a" in zf_map:
                g0 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["st-5.4a"]),
                    find_zip_entry(zf_map, ["st-2.8a"]),
                    find_zip_entry(zf_map, ["st-9.6a"]),
                    find_zip_entry(zf_map, ["st-11.10a"]),
                )
                g1 = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["st-4.3a"]),
                    find_zip_entry(zf_map, ["st-1.7a"]),
                    find_zip_entry(zf_map, ["st-8.5a"]),
                    find_zip_entry(zf_map, ["st-10.9a"]),
                )
                gfx = g0 + g1
            else:
                gfx = build_cps1_gfx_bank(
                    find_zip_entry(zf_map, ["st-1m.3a", "st_1m.3a"]),
                    find_zip_entry(zf_map, ["st-5m.7a", "st_5m.7a"]),
                    find_zip_entry(zf_map, ["st-3m.5a", "st_3m.5a"]),
                    find_zip_entry(zf_map, ["st-7m.9a", "st_7m.9a"]),
                )
            return m68k, z80, oki, gfx
        except KeyError:
            pass

    if short_name in ("3wonders", "wonder3", "3wondersu", "3wondersh"):
        try:
            if short_name == "wonder3" and "rtj_36.12f" in zf_map:
                chunks = [
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rtj_36.12f"]), find_zip_entry(zf_map, ["rtj_42.12h"])),
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rtj_37.13f"]), find_zip_entry(zf_map, ["rtj_43.13h"])),
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rt_34.10f"]),  find_zip_entry(zf_map, ["rt_40.10h"])),
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rtj_35.11f"]), find_zip_entry(zf_map, ["rtj_41.11h"])),
                ]
            else:
                chunks = [
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rte_30a.11f", "rtj_36.12f"]), find_zip_entry(zf_map, ["rte_35a.11h", "rtj_42.12h"])),
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rte_31a.12f", "rtj_37.13f"]), find_zip_entry(zf_map, ["rte_36a.12h", "rtj_43.13h"])),
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rt_34.10f"]),                  find_zip_entry(zf_map, ["rt_40.10h"])),
                    interleave_16bit_pair(find_zip_entry(zf_map, ["rte_29a.10f", "rtj_35.11f"]), find_zip_entry(zf_map, ["rte_34a.10h", "rtj_41.11h"])),
                ]
            m68k = b"".join(chunks)
            z80 = find_zip_entry(zf_map, ["rt_23.13b"])[:0x8000]
            oki = find_zip_entry(zf_map, ["rt_30.12c"]) + find_zip_entry(zf_map, ["rt_31.13c"])
            g0 = build_cps1_gfx_bank(
                find_zip_entry(zf_map, ["rt-1m.3a"]),
                find_zip_entry(zf_map, ["rt-5m.7a"]),
                find_zip_entry(zf_map, ["rt-3m.5a"]),
                find_zip_entry(zf_map, ["rt-7m.9a"]),
            )
            g1 = build_cps1_gfx_bank(
                find_zip_entry(zf_map, ["rt-2m.4a"]),
                find_zip_entry(zf_map, ["rt-6m.8a"]),
                find_zip_entry(zf_map, ["rt-4m.6a"]),
                find_zip_entry(zf_map, ["rt-8m.10a"]),
            )
            return m68k, z80, oki, g0 + g1
        except KeyError:
            pass

    return None


def detect_and_pack_cps1_zip(zip_path: str):
    """
    Inspects a MAME .zip file, verifies whether it is a Capcom CPS-1 game, and
    returns (short_name, game_meta, v2_bundle_bytes).
    Raises ValueError if the .zip is not a CPS-1 ROM (e.g. Sega System 16 `altbeast.zip`).
    """
    if not zipfile.is_zipfile(zip_path):
        raise ValueError(f"Not a valid .zip archive: {zip_path}")

    stem = os.path.splitext(os.path.basename(zip_path))[0].lower()
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf_map = {}
        for info in zf.infolist():
            if not info.is_dir():
                zf_map[info.filename.lower()] = zf.read(info.filename)

    # Reject non-CPS-1 ROM sets cleanly:
    # Check if stem is in CPS1_GAMES or has CPS-1 signature ROM filenames
    game_meta = CPS1_GAMES.get(stem)
    if game_meta is None:
        # Check if filenames inside match a known CPS-1 prefix
        all_names = " ".join(zf_map.keys())
        if "epr-" in all_names or "opr-" in all_names or "mpr-" in all_names:
            raise ValueError(f"'{os.path.basename(zip_path)}' is a Sega arcade ROM (EPR/OPR/MPR), not a Capcom CPS-1 ROM.")
        for k, meta in CPS1_GAMES.items():
            if any(fn.startswith(k + "_") or fn.startswith(k + "-") for fn in zf_map):
                stem = k
                game_meta = meta
                break
        if game_meta is None:
            raise ValueError(f"'{os.path.basename(zip_path)}' is not a recognized Capcom CPS-1 ROM set.")

    packed = try_pack_explicit_game(stem, zf_map)
    if packed is None:
        raise ValueError(f"'{os.path.basename(zip_path)}' ({stem}) is missing required CPS-1 ROM files.")

    m68k_bytes, z80_bytes, oki_bytes, gfx_planar_bytes = packed

    # Pad/align sections:
    if len(z80_bytes) < 0x8000:
        z80_bytes = z80_bytes + b"\x00" * (0x8000 - len(z80_bytes))
    else:
        z80_bytes = z80_bytes[:0x8000]

    if len(oki_bytes) < 0x40000:
        oki_bytes = oki_bytes + b"\x00" * (0x40000 - len(oki_bytes))

    # Build 168-byte cps1_game_config_t + 24-byte V2 header = 192-byte header
    cpsb = CPSB_PROFILES[game_meta["cpsb"]]
    gfxp = GFX_PROFILES[game_meta["gfx"]]
    dswa, dswb, dswc = game_meta.get("dsw", (0xFF, 0xFF, 0xFF))

    cfg_bytes = struct.pack(
        "<16s48s16H9i2I6B18x",
        stem.encode("ascii")[:15],
        game_meta["title"].encode("ascii")[:47],
        cpsb[0], cpsb[1], cpsb[2], cpsb[3], cpsb[4], cpsb[5],
        cpsb[6], cpsb[7], cpsb[8], cpsb[9], cpsb[10], cpsb[11],
        cpsb[12], cpsb[13], cpsb[14], 0,
        gfxp[0], gfxp[1], gfxp[2], gfxp[3],
        gfxp[4], gfxp[5], gfxp[6], gfxp[7], gfxp[8],
        game_meta["m68k_hz"],
        game_meta["oki_hz"],
        game_meta["input"],
        0,  # horizontal orientation
        dswa, dswb, dswc,
        game_meta.get("flags", 0),
    )
    assert len(cfg_bytes) == 164, f"Expected 164-byte config, got {len(cfg_bytes)}"

    hdr_prefix = struct.pack(
        "<4sIIIII",
        CPS1_BUNDLE_MAGIC_V2,
        CPS1_BUNDLE_VER_V2,
        len(m68k_bytes),
        len(z80_bytes),
        len(oki_bytes),
        len(gfx_planar_bytes),
    )
    assert len(hdr_prefix) == 24
    bundle = hdr_prefix + cfg_bytes + m68k_bytes + z80_bytes + oki_bytes + gfx_planar_bytes
    return stem, game_meta, bundle


# ==============================================================================
# Subcommand 1: `build-bin` (Standalone NAOMI .bin Packager)
# ==============================================================================
def build_standalone_naomi_bin(zip_path: str, out_path: str) -> str:
    if not os.path.exists(ENGINE_BIN):
        raise RuntimeError(f"Engine binary not found at {ENGINE_BIN}. Run 'make' first.")

    stem, meta, bundle = detect_and_pack_cps1_zip(zip_path)
    t0 = time.time()

    with tempfile.TemporaryDirectory(prefix=f"cps1_{stem}_") as tmpdir:
        romfs_dir = os.path.join(tmpdir, "romfs")
        os.makedirs(romfs_dir, exist_ok=True)
        cps1_file = os.path.join(romfs_dir, "game.cps1")
        with open(cps1_file, "wb") as f:
            f.write(bundle)

        romfs_bin = os.path.join(tmpdir, "romfs.bin")
        subprocess.run(
            [PYTHON_NETBOOT, ROMFSGEN_PY, romfs_bin, romfs_dir],
            check=True,
            stdout=subprocess.DEVNULL,
        )

        # Clean 31-char NAOMI header title
        naomi_title = f"CPS1 {meta['title'].upper()}"[:31]
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        subprocess.run(
            [
                PYTHON_NETBOOT,
                MAKEROM_PY,
                out_path,
                "--title", naomi_title,
                "--publisher", "Capcom / Homebrew",
                "--serial", "BCPS",
                "--section", f"{ENGINE_BIN},0xc021000",
                "--entrypoint", "0xc021000",
                "--main-binary-includes-test-binary",
                "--test-entrypoint", "0xc021004",
                "--align-before-data", "4",
                "--filedata", romfs_bin,
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )

    dt = time.time() - t0
    sz_mb = os.path.getsize(out_path) / (1024 * 1024)
    print(f"  [OK] Packaged {stem:10s} -> {out_path} ({sz_mb:.2f} MB in {dt:.2f}s) | {meta['title']}")
    return out_path


def cmd_build_bin(args):
    target = os.path.expanduser(args.target)
    if os.path.isfile(target):
        out_path = args.output
        if not out_path:
            stem = os.path.splitext(os.path.basename(target))[0].lower()
            out_path = os.path.join(SCRIPT_DIR, "dist", f"{stem}_naomi.bin")
        elif os.path.isdir(out_path) or out_path.endswith("/"):
            stem = os.path.splitext(os.path.basename(target))[0].lower()
            out_path = os.path.join(out_path, f"{stem}_naomi.bin")
        build_standalone_naomi_bin(target, out_path)
    elif os.path.isdir(target):
        out_dir = os.path.expanduser(args.output) if args.output else os.path.join(SCRIPT_DIR, "dist")
        os.makedirs(out_dir, exist_ok=True)
        zips = sorted(f for f in os.listdir(target) if f.lower().endswith(".zip"))
        built = 0
        skipped = 0
        for zname in zips:
            zpath = os.path.join(target, zname)
            stem = os.path.splitext(zname)[0].lower()
            out_path = os.path.join(out_dir, f"{stem}_naomi.bin")
            try:
                build_standalone_naomi_bin(zpath, out_path)
                built += 1
            except ValueError as e:
                print(f"  [SKIP] {zname:14s}: {e}")
                skipped += 1
        print(f"\nDone! Packaged {built} CPS-1 NAOMI .bin ROM(s) into {out_dir} (skipped {skipped} non-CPS-1 zip(s)).")
    else:
        sys.exit(f"Error: path not found: {target}")


# ==============================================================================
# Subcommand 2 & 3: Net DIMM Live Push (`push`) & Jukebox Server (`serve`)
# ==============================================================================
CART_CATALOG_OFFSET = 0x04000000
CART_MAILBOX_OFFSET = 0x0400FF00
CART_PAYLOAD_OFFSET = 0x04010000

MAILBOX_MAGIC_IDLE = 0x43505330
MAILBOX_MAGIC_UPLD = 0x43505355
MAILBOX_MAGIC_BOOT = 0x43505342
MAILBOX_MAGIC_RUN  = 0x43505352
MAILBOX_MAGIC_REQ  = 0x43505351
CATALOG_MAGIC      = 0x43415431


def get_netdimm_class():
    netboot_dir = os.path.expanduser("~/netboot")
    if netboot_dir not in sys.path:
        sys.path.insert(0, netboot_dir)
    from netdimm import NetDimm
    return NetDimm


def write_mailbox(nd, magic: int, bundle_size: int, progress: int, req_idx: int, name: str):
    name_b = name.encode("ascii", errors="replace")[:31].ljust(32, b"\x00")
    pkt = struct.pack("<IIII32s", magic, bundle_size, progress, req_idx, name_b)
    nd.send_chunk(CART_MAILBOX_OFFSET, pkt)


def push_bundle_to_netdimm(ip: str, stem: str, title: str, bundle: bytes, boot_server_first: bool = False):
    NetDimm = get_netdimm_class()
    nd = NetDimm(ip)

    if boot_server_first:
        if not os.path.exists(SERVER_BIN):
            raise RuntimeError(f"Server binary not found: {SERVER_BIN}")
        print(f"[*] Booting naomi_cps1_server.bin on {ip}...")
        with open(SERVER_BIN, "rb") as sf:
            nd.send(sf.read(), disable_crc_check=True)
        nd.reboot()
        time.sleep(4.0)

    total = len(bundle)
    display_name = f"{stem}: {title}"[:31]
    print(f"[*] Pushing {display_name} ({total / 1024:.1f} KB) to Net DIMM {ip} @ 0x04010000...")

    with nd.connection():
        write_mailbox(nd, MAILBOX_MAGIC_UPLD, total, 0, 0xFFFFFFFF, display_name)
        t0 = time.time()
        nd.send_chunk(CART_PAYLOAD_OFFSET, bundle)
        dt = time.time() - t0
        print(f"[*] Upload finished in {dt:.2f}s ({total / 1024:.1f} KB)! Signaling NAOMI to unpack & boot {stem}...")
        write_mailbox(nd, MAILBOX_MAGIC_BOOT, total, total, 0xFFFFFFFF, display_name)


def cmd_push(args):
    target = os.path.expanduser(args.zip_file)
    if os.path.isdir(target):
        print(f"[*] '{target}' is a directory — starting Jukebox Server (`serve`) for the whole folder...")
        args.roms_dir = target
        return cmd_serve(args)
    stem, meta, bundle = detect_and_pack_cps1_zip(target)
    push_bundle_to_netdimm(args.ip, stem, meta["title"], bundle, boot_server_first=args.boot_server)


def cmd_serve(args):
    roms_dir = os.path.expanduser(args.roms_dir)
    if not os.path.isdir(roms_dir):
        sys.exit(f"Error: directory not found: {roms_dir}")

    catalog = []
    for zname in sorted(os.listdir(roms_dir)):
        if not zname.lower().endswith(".zip"):
            continue
        zpath = os.path.join(roms_dir, zname)
        try:
            stem, meta, bundle = detect_and_pack_cps1_zip(zpath)
            catalog.append((stem, meta["title"], zpath, bundle))
            print(f"  [+] Indexed {stem:10s} | {meta['title']} ({len(bundle)//1024} KB)")
        except ValueError as e:
            print(f"  [-] Skipped {zname:14s}: {e}")

    if not catalog:
        sys.exit("No valid CPS-1 ROMs found to serve.")

    NetDimm = get_netdimm_class()
    nd = NetDimm(args.ip)

    if args.boot_server:
        if not os.path.exists(SERVER_BIN):
            raise RuntimeError(f"Server binary not found: {SERVER_BIN}")
        print(f"[*] Booting naomi_cps1_server.bin on {args.ip}...")
        with open(SERVER_BIN, "rb") as sf:
            nd.send(sf.read(), disable_crc_check=True)
        nd.reboot()
        time.sleep(4.0)

    # Build binary Jukebox catalog at 0x04000000
    entries_bin = bytearray()
    for stem, title, _, bundle in catalog[:64]:
        s_b = stem.encode("ascii")[:15].ljust(16, b"\x00")
        t_b = title.encode("ascii")[:43].ljust(44, b"\x00")
        entries_bin += struct.pack("<16s44sI", s_b, t_b, len(bundle) // 1024)

    cat_pkt = struct.pack("<II", CATALOG_MAGIC, min(len(catalog), 64)) + bytes(entries_bin)
    print(f"[*] Publishing Jukebox Catalog ({len(catalog)} CPS-1 games) to NAOMI {args.ip}...")
    with nd.connection():
        nd.send_chunk(CART_CATALOG_OFFSET, cat_pkt)
        write_mailbox(nd, MAILBOX_MAGIC_IDLE, 0, 0, 0xFFFFFFFF, f"{len(catalog)} Games Ready")

    from netdimm import PeekPokeTypeEnum
    print("[*] Jukebox Server active! Select any game on the NAOMI cabinet (Ctrl+C to stop)...")
    try:
        while True:
            time.sleep(0.35)
            req_idx = None
            with nd.connection():
                magic = nd.peek(0xC0DE10, PeekPokeTypeEnum.TYPE_LONG)
                if magic == MAILBOX_MAGIC_REQ:
                    idx = nd.peek(0xC0DE40, PeekPokeTypeEnum.TYPE_LONG)
                    if 0 <= idx < len(catalog):
                        req_idx = idx
            if req_idx is not None:
                stem, title, _, bundle = catalog[req_idx]
                print(f"\n[*] NAOMI requested game #{req_idx}: {title} [{stem}]")
                push_bundle_to_netdimm(args.ip, stem, title, bundle, boot_server_first=False)
    except KeyboardInterrupt:
        print("\n[*] Jukebox Server stopped.")


def cmd_list(args):
    if args.roms_dir:
        roms_dir = os.path.expanduser(args.roms_dir)
        print(f"Scanning {roms_dir} for Capcom CPS-1 ROM sets:\n")
        for zname in sorted(os.listdir(roms_dir)):
            if not zname.lower().endswith(".zip"):
                continue
            zpath = os.path.join(roms_dir, zname)
            try:
                stem, meta, bundle = detect_and_pack_cps1_zip(zpath)
                print(f"  [CPS-1 OK] {zname:14s} -> {stem:10s} | {meta['title']:36s} | Bundle: {len(bundle)/1024:6.1f} KB")
            except ValueError as e:
                print(f"  [SKIP]     {zname:14s} -> {e}")
    else:
        print(f"Supported CPS-1 ROM Sets ({len(CPS1_GAMES)} sets configured):\n")
        for k, meta in sorted(CPS1_GAMES.items()):
            print(f"  {k:10s} : {meta['title']} ({meta['m68k_hz']//1000000} MHz 68000, CPS-B={meta['cpsb']})")


def main():
    parser = argparse.ArgumentParser(description="Capcom CPS-1 Multi-Game Packager & Net DIMM Live Server Tool")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="List supported CPS-1 sets or scan a directory of .zip ROMs")
    p_list.add_argument("roms_dir", nargs="?", help="Optional directory of .zip ROMs to scan")
    p_list.set_defaults(func=cmd_list)

    p_build = sub.add_parser("build-bin", help="Package MAME .zip ROM(s) into standalone NAOMI .bin files")
    p_build.add_argument("target", help="Path to a MAME .zip file or directory of .zip files")
    p_build.add_argument("-o", "--output", help="Output .bin file or directory (default: ./dist/)")
    p_build.set_defaults(func=cmd_build_bin)

    p_push = sub.add_parser("push", help="Pack and live-push a MAME .zip ROM (or serve a folder) over Net DIMM")
    p_push.add_argument("ip", help="NAOMI Net DIMM IP address (e.g. 192.168.1.50)")
    p_push.add_argument("zip_file", help="Path to MAME CPS-1 .zip ROM (or directory to serve)")
    p_push.add_argument("--boot-server", action="store_true", help="Send & boot naomi_cps1_server.bin first")
    p_push.set_defaults(func=cmd_push)

    p_serve = sub.add_parser("serve", help="Serve a directory of CPS-1 .zip ROMs to the on-screen NAOMI Jukebox menu")
    p_serve.add_argument("ip", help="NAOMI Net DIMM IP address (e.g. 192.168.1.50)")
    p_serve.add_argument("roms_dir", help="Directory containing MAME CPS-1 .zip ROMs")
    p_serve.add_argument("--boot-server", action="store_true", help="Send & boot naomi_cps1_server.bin first")
    p_serve.set_defaults(func=cmd_serve)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
