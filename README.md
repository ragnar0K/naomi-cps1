# Capcom CPS-1 Multi-Game Emulator & Net DIMM Server for Sega NAOMI

PLEASE SUPPORT IF YOU USE IT :(

Some sats are always welcome!

![Bitcoin QR Code](assets/QRBTC.png)

**BTC**: `bc1qfmdwq9ykw4qc9fz4k3e87etjuhh3ruvrhkcsut`

---

A bare-metal **Capcom CPS-1** emulator and **Net DIMM Jukebox / Server** for **Sega NAOMI**, built with [`libnaomi`](https://github.com/DragonMinded/libnaomi). Hardware-accelerated tiles/sprites on **PowerVR2 (CLUT4)** + stereo FM/ADPCM on **Yamaha AICA**.
Tested with the roms I own: Final fight, Pang and SFII WW

> **No ROMs included.** Supply your own MAME CPS-1 `.zip` sets.

## What's Inside

- **2 Launch Modes**:
  1. **Net DIMM Jukebox / Server (`naomi_cps1_server.bin`)**: Boot once on the NAOMI, browse your ROM folder with the arcade stick, and hot-load games into Net DIMM RAM (`0x04010000`) with zero rebooting.
  2. **Standalone `.bin` (`cps1_tool.py build-bin`)**: Pack any `.zip` into a self-contained NAOMI `.bin` in ~0.1s (no C recompile).
- **Hardware Acceleration**: Dynamic 9 MB PowerVR2 `CLUT4` tile/sprite renderer + Musashi 68K (auto idle-loop skip) + Z80/YM2151/OKIM6295 audio.

## Controls

| Action | 2/3-Button Games | 6-Button *Street Fighter II* |
| :--- | :--- | :--- |
| **Actions** | `B1–B3` (`B3` = special / B1+B2) | `B1–B3`: Punch, `B4–B6`: Kick |
| **Insert Coin** | **`Start + B1`** *(or `Start + Down` / `Service` / `PSW`)* | **`Start + B1`** *(or `Start + Down` / `Service` / `PSW`)* |
| **Filter / Aspect** | `B5`: Bilinear / `B6`: Aspect | `P1 Start + B5` / `P1 Start + B6` |
| **OSD / Profiler** | `P1 Start + Up` | `P1 Start + Up` |
| **Jukebox Menu** | Hold **`P1 Start + P2 Start`** (1.5s) | Hold **`P1 Start + P2 Start`** (1.5s) |

## Quick Start

1. Download **`naomi_cps1_server.bin`** and **`cps1_tool.py`** from [Releases](https://github.com/ragnar0K/naomi-cps1/releases/tag/v1.0.0) *(plus `naomi.bin` into `build/` if using `build-bin`)*.
2. Boot & serve your ROM folder to your NAOMI cabinet:
```bash
python3 cps1_tool.py serve --boot-server <NAOMI_IP> /path/to/roms/
```
Or push one game live / build a standalone `.bin`:
```bash
python3 cps1_tool.py push <NAOMI_IP> /path/to/roms/pang3.zip
python3 cps1_tool.py build-bin /path/to/roms/ffight.zip -o dist/ffight_naomi.bin
```

## Build from Source

```bash
source /path/to/naomi/toolchain/env.sh
make clean && make
```
