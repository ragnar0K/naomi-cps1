# Sega NAOMI Multi-Game CPS-1 Arcade Emulator & Net DIMM Server Makefile
# ----------------------------------------------------------------------
# Builds:
#   1. `naomi_cps1_server.bin`: ROM-free Net DIMM Mini-Server / Jukebox Loader
#   2. `build/naomi.bin`:       Core SH-4 engine binary used by `cps1_tool.py build-bin`
#                               to package any MAME CPS-1 .zip into a standalone NAOMI .bin
#                               in ~1 second without recompiling!
#
# Usage:
#   source ../toolchain/env.sh
#   make

all: naomi_cps1_server.bin

SERIAL  := BCPS

SRCS += main.c
SRCS += src/cps1_emu.c
SRCS += src/m68k/m68kcpu.c
SRCS += src/m68k/m68kops.c
SRCS += src/sound/z80.c
SRCS += src/sound/ym2151.c

FLAGS += -Isrc -Isrc/m68k -Isrc/sound
FLAGS += -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
FLAGS += -ffast-math -mfsrra -mfsca -mpretend-cmove
LIBS  += -lnaomimessage

include ${NAOMI_BASE}/tools/Makefile.base

# Compile large 68000 / Z80 opcode dispatch tables with -Os to minimize SH-4 8KB I-cache misses
build/src/m68k/m68kops.o: OPTIMIZATION_LEVEL := -Os
build/src/sound/z80.o: OPTIMIZATION_LEVEL := -Os

build/empty_romfs.bin: ${ROMFSGEN_FILE}
	@mkdir -p build/empty_romfs
	@touch build/empty_romfs/.keep
	${ROMFSGEN} $@ build/empty_romfs/

naomi_cps1_server.bin: ${MAKEROM_FILE} ${NAOMI_BIN_FILE} build/empty_romfs.bin
	${MAKEROM} $@ \
		--title "CAPCOM CPS1  SERVER NAOMI2" \
		--publisher "Capcom / Homebrew" \
		--serial "${SERIAL}" \
		--section ${NAOMI_BIN_FILE},${START_ADDR} \
		--entrypoint ${MAIN_ADDR} \
		--main-binary-includes-test-binary \
		--test-entrypoint ${TEST_ADDR} \
		--align-before-data 4 \
		--filedata build/empty_romfs.bin

.PHONY: clean
clean:
	rm -rf build
	rm -f naomi_cps1_server.bin
