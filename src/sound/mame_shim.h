#ifndef MAME_SHIM_H
#define MAME_SHIM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef INLINE
#define INLINE inline
#endif

/* Little-endian PAIR union (for SH-4 and x86_64) */
typedef union {
    struct { uint8_t l, h, h2, h3; } b;
    struct { uint16_t l, h; } w;
    uint32_t d;
} PAIR;

#define CLEAR_LINE  0
#define ASSERT_LINE 1

#define Z80_MAXDAISY 4
#define Z80_INT_REQ  0x01
#define Z80_INT_IEO  0x02

typedef struct {
    void (*reset)(int);
    int  (*interrupt_entry)(int);
    void (*interrupt_reti)(int);
    int  irq_param;
} Z80_DaisyChain;

#define REG_PREVIOUSPC  -1
#define REG_SP_CONTENTS -2

enum {
    CPU_INFO_NAME,
    CPU_INFO_FAMILY,
    CPU_INFO_VERSION,
    CPU_INFO_FILE,
    CPU_INFO_CREDITS
};

typedef void (*mem_write_handler)(int offset, int data);

#define logerror(...) do {} while (0)
#define change_pc(pc) do {} while (0)
#define change_pc16(pc) do {} while (0)

#define cpu_getactivecpu() 0
#define state_save_UINT16(f, m, c, n, v, s) do {} while (0)
#define state_save_UINT8(f, m, c, n, v, s)  do {} while (0)
#define state_save_INT8(f, m, c, n, v, s)   do {} while (0)
#define state_load_UINT16(f, m, c, n, v, s) do {} while (0)
#define state_load_UINT8(f, m, c, n, v, s)  do {} while (0)
#define state_load_INT8(f, m, c, n, v, s)   do {} while (0)

extern uint8_t *g_z80_rom;
extern uint8_t  g_z80_ram[0x0800];

uint8_t cps1_z80_read(uint16_t addr);
void    cps1_z80_write(uint16_t addr, uint8_t val);

static inline uint8_t cps1_z80_readop_fast(uint16_t addr)
{
    if (__builtin_expect(addr < 0x8000u, 1))
        return g_z80_rom[addr];
    return cps1_z80_read(addr);
}

static inline uint8_t cps1_z80_read_fast(uint16_t addr)
{
    if (__builtin_expect(addr < 0x8000u, 1))
        return g_z80_rom[addr];
    if (__builtin_expect((addr & 0xF800u) == 0xD000u, 1))
        return g_z80_ram[addr & 0x7FFu];
    return cps1_z80_read(addr);
}

static inline void cps1_z80_write_fast(uint16_t addr, uint8_t val)
{
    if (__builtin_expect((addr & 0xF800u) == 0xD000u, 1)) {
        g_z80_ram[addr & 0x7FFu] = val;
        return;
    }
    cps1_z80_write(addr, val);
}

#define cpu_readmem16(addr)        cps1_z80_read_fast(addr)
#define cpu_writemem16(addr, val)  cps1_z80_write_fast(addr, val)
#define cpu_readop(addr)           cps1_z80_readop_fast(addr)
#define cpu_readop_arg(addr)       cps1_z80_readop_fast(addr)
#define cpu_readport(port)         (0xff)
#define cpu_writeport(port, val)   do {} while (0)

#endif /* MAME_SHIM_H */
