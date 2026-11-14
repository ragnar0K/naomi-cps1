#include "cps1_emu.h"
#include "m68k/m68k.h"
#include "sound/z80.h"
#include "sound/ym2151.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __sh__
#include <naomi/timer.h>
#include <naomi/video.h>
#include <naomi/ta.h>
#include <naomi/cart.h>
#define HW_VRAM_UNCACHED(p) ((uint16_t *)((uint32_t)(p) | 0xA0000000u))
#elif defined(HOST_TA_SIM)
#include "host_ta_shim.h"
#define HW_VRAM_UNCACHED(p) ((uint16_t *)(p))
#endif

static cps1_prof_t *g_prof = NULL;

void cps1_emu_set_profiler(cps1_prof_t *prof)
{
    g_prof = prof;
}

/* ======================================================================== */
/*                        CPS-1 ROM & RAM BUFFERS                           */
/* ======================================================================== */

/* V1 bundle header (backward-compatible with romfs/ffight.cps1) */
typedef struct __attribute__((packed)) {
    char     magic[4];      /* "CPS1" */
    uint32_t m68k_size;
    uint32_t gfx_bytes;
    uint32_t z80_size;
    uint32_t oki_size;
    uint32_t num_char_pu;
    uint32_t num_tile16_pu;
    uint32_t num_tile32_pu;
} cps1_bundle_hdr_v1_t;

/* V2 multi-game bundle header ("CPSM", 192 bytes) */
typedef struct __attribute__((packed)) {
    char               magic[4];    /* "CPSM" */
    uint32_t           version;     /* 2 */
    uint32_t           m68k_size;
    uint32_t           z80_size;
    uint32_t           oki_size;
    uint32_t           gfx_size;    /* Raw planar GFX size (e.g. 2MB, 4MB, 6MB) */
    cps1_game_config_t cfg;         /* 168 bytes */
} cps1_bundle_hdr_v2_t;

#define M68K_MAX_ROM_SIZE   0x200000u   /* Up to 2 MB 68000 ROM */
#define Z80_MAX_ROM_SIZE    0x018000u   /* Up to 96 KB Z80 ROM */
#define OKI_MAX_ROM_SIZE    0x080000u   /* Up to 512 KB OKI6295 ROM */

uint8_t  *g_m68k_rom   = NULL;
static uint32_t  g_m68k_rom_size = 0x100000u;
static uint32_t *g_cps1_gfx   = NULL;
static uint16_t *g_char_pu    = NULL;
static uint16_t *g_tile16_pu  = NULL;
static uint16_t *g_tile32_pu  = NULL;
static uint32_t  g_max_char   = 0x008000u;
static uint32_t  g_max_tile16 = 0x004000u;
static uint32_t  g_max_tile32 = 0x001000u;
static uint8_t  *g_z80_rom    = NULL;
static uint32_t  g_z80_rom_size = 0x010000u;
static uint8_t  *g_oki_rom    = NULL;
static uint32_t  g_oki_rom_size = 0x040000u;

/* Active per-game hardware config */
static cps1_game_config_t g_cfg;

/* Auto-detected Z80 sound driver idle loop PC / PPC (used in src/sound/z80.c) */
uint16_t g_z80_idle_pc  = 0x0091u;
uint16_t g_z80_idle_ppc = 0x00AAu;

uint8_t         g_gfxram[0x40000]      __attribute__((aligned(32)));
uint8_t         g_mainram[0x10000]     __attribute__((aligned(32)));
static uint8_t  g_output[0x100]        __attribute__((aligned(32)));
static uint8_t  g_buffered_obj[0x0800] __attribute__((aligned(32)));
static uint8_t  g_z80_ram[0x0800]      __attribute__((aligned(32)));

static uint16_t g_canvas[CPS1_BUF_HEIGHT][CPS1_BUF_STRIDE]  __attribute__((aligned(32)));
static uint8_t  g_pri_buf[CPS1_BUF_HEIGHT][CPS1_BUF_STRIDE] __attribute__((aligned(32)));
static uint16_t g_rgb565_pal[2048]                          __attribute__((aligned(32)));
static int      g_has_pri_layer = 0;

uint16_t *cps1_emu_get_vis_canvas(void)
{
    return &g_canvas[48][32];
}

const cps1_game_config_t *cps1_emu_get_config(void)
{
    return &g_cfg;
}

/* Active-low CPS-1 input latches */
static uint8_t  g_in_p1   = 0xFF;
static uint8_t  g_in_p2   = 0xFF;
static uint16_t g_in_p3   = 0xFFFF; /* SF2 6-button Kick harness (0x800176 / 0x8001FC) */
static uint8_t  g_in_sys  = 0xFF;
static uint8_t  g_in_dswa = 0xFF;
static uint8_t  g_in_dswb = 0xF4;
static uint8_t  g_in_dswc = 0x9F;
static int16_t  g_dial_val[2] = { 0, 0 };

/* Sound latches & Z80 bank */
static uint8_t  g_soundlatch       = 0;
static uint8_t  g_sound_fade_timer = 0;
static uint8_t  g_z80_bank         = 0;
static uint8_t  g_ym2151_reg       = 0;
static uint32_t g_oki_bank_base    = 0;
static int      g_ym2151_inited    = 0;

/* Big-endian 16-bit / 32-bit helpers (using SH-4 hardware swap.b / swap.w) */
static inline uint16_t read_be16(const uint8_t *p)
{
    return __builtin_bswap16(*(const uint16_t *)p);
}

static inline void write_be16(uint8_t *p, uint16_t v)
{
    *(uint16_t *)p = __builtin_bswap16(v);
}

static inline uint32_t read_be32(const uint8_t *p)
{
    const uint16_t *p16 = (const uint16_t *)p;
    return ((uint32_t)__builtin_bswap16(p16[0]) << 16) | __builtin_bswap16(p16[1]);
}

static inline void write_be32(uint8_t *p, uint32_t v)
{
    uint16_t *p16 = (uint16_t *)p;
    p16[0] = __builtin_bswap16((uint16_t)(v >> 16));
    p16[1] = __builtin_bswap16((uint16_t)v);
}

/* ======================================================================== */
/*              93C46 SERIAL EEPROM (PANG! 3 / CPS-1.5 AT 0x80017A)         */
/* ======================================================================== */

typedef struct {
    uint16_t data[64];
    uint8_t  cs;
    uint8_t  clk;
    uint8_t  sending;
    uint8_t  write_enabled;
    uint16_t cmd_bits;
    uint8_t  cmd_len;
    uint16_t shift_out;
    int8_t   write_addr;
    uint16_t write_buf;
    uint8_t  write_bits;
} eeprom_93c46_t;

static eeprom_93c46_t g_eeprom;

static void eeprom_init(void)
{
    memset(&g_eeprom, 0, sizeof(g_eeprom));
    memset(g_eeprom.data, 0xFF, sizeof(g_eeprom.data));
    g_eeprom.write_addr = -1;
}

static uint8_t eeprom_read_bit(void)
{
    if (!g_eeprom.cs)
        return 1;
    if (g_eeprom.sending == 1)
        return 0; /* 93C46 dummy 0 bit between 9th and 10th clock */
    if (g_eeprom.sending >= 2)
        return (g_eeprom.shift_out >> 15) & 1;
    return 1;
}

static void eeprom_write(uint8_t cs, uint8_t clk, uint8_t di)
{
    if (!cs) {
        g_eeprom.cs = 0;
        g_eeprom.clk = clk;
        g_eeprom.sending = 0;
        g_eeprom.cmd_bits = 0;
        g_eeprom.cmd_len = 0;
        g_eeprom.write_addr = -1;
        g_eeprom.write_bits = 0;
        return;
    }
    if (cs && !g_eeprom.clk && clk) {
        if (g_eeprom.sending == 1) {
            g_eeprom.sending = 2; /* 10th clock: present bit 15 */
        } else if (g_eeprom.sending >= 2) {
            g_eeprom.shift_out <<= 1;
        } else if (g_eeprom.write_addr >= 0) {
            g_eeprom.write_buf = (uint16_t)((g_eeprom.write_buf << 1) | (di & 1));
            g_eeprom.write_bits++;
            if (g_eeprom.write_bits == 16) {
                if (g_eeprom.write_enabled) {
                    if (g_eeprom.write_addr == 64) {
                        for (int i = 0; i < 64; i++)
                            g_eeprom.data[i] = g_eeprom.write_buf;
                    } else {
                        g_eeprom.data[g_eeprom.write_addr & 63] = g_eeprom.write_buf;
                    }
                }
                g_eeprom.write_addr = -1;
                g_eeprom.write_bits = 0;
            }
        } else {
            if (g_eeprom.cmd_len == 0 && !(di & 1)) {
                /* Wait for start bit */
            } else {
                g_eeprom.cmd_bits = (uint16_t)((g_eeprom.cmd_bits << 1) | (di & 1));
                g_eeprom.cmd_len++;
                if (g_eeprom.cmd_len == 9) {
                    uint8_t op   = (g_eeprom.cmd_bits >> 6) & 3;
                    uint8_t addr = g_eeprom.cmd_bits & 0x3F;
                    if (op == 2) {
                        g_eeprom.shift_out = g_eeprom.data[addr];
                        g_eeprom.sending = 1;
                    } else if (op == 1) {
                        g_eeprom.write_addr = addr;
                        g_eeprom.write_buf = 0;
                        g_eeprom.write_bits = 0;
                    } else if (op == 3) {
                        if (g_eeprom.write_enabled)
                            g_eeprom.data[addr] = 0xFFFF;
                    } else if (op == 0) {
                        uint8_t sub = (addr >> 4) & 3;
                        if (sub == 3)      g_eeprom.write_enabled = 1;
                        else if (sub == 0) g_eeprom.write_enabled = 0;
                        else if (sub == 2 && g_eeprom.write_enabled)
                            memset(g_eeprom.data, 0xFF, sizeof(g_eeprom.data));
                        else if (sub == 1) {
                            g_eeprom.write_addr = 64;
                            g_eeprom.write_buf = 0;
                            g_eeprom.write_bits = 0;
                        }
                    }
                    g_eeprom.cmd_bits = 0;
                    g_eeprom.cmd_len = 0;
                }
            }
        }
    }
    g_eeprom.cs = cs;
    g_eeprom.clk = clk;
}

/* ======================================================================== */
/*                       OKIM6295 ADPCM SOUND CHIP                          */
/* ======================================================================== */

#define OKI_VOICES      4
#define OKI_FRAC_BITS   14
#define OKI_FRAC_ONE    (1 << OKI_FRAC_BITS)

typedef struct {
    uint8_t        playing;
    const uint8_t *base;
    uint32_t       sample;
    uint32_t       count;
    int32_t        signal;
    int32_t        step;
    uint32_t       volume;
    int16_t        last_sample;
    int16_t        curr_sample;
    uint32_t       source_step;
    uint32_t       source_pos;
} oki_voice_t;

static oki_voice_t g_oki_voices[OKI_VOICES];
static int         g_oki_command = -1;
static int         g_oki_diff_lookup[49 * 16];
static uint32_t    g_oki_volume_table[16];
static const int   g_oki_index_shift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static int         g_oki_tables_built = 0;

static void oki6295_init(void)
{
    static const int nbl2bit[16][4] = {
        { 1, 0, 0, 0}, { 1, 0, 0, 1}, { 1, 0, 1, 0}, { 1, 0, 1, 1},
        { 1, 1, 0, 0}, { 1, 1, 0, 1}, { 1, 1, 1, 0}, { 1, 1, 1, 1},
        {-1, 0, 0, 0}, {-1, 0, 0, 1}, {-1, 0, 1, 0}, {-1, 0, 1, 1},
        {-1, 1, 0, 0}, {-1, 1, 0, 1}, {-1, 1, 1, 0}, {-1, 1, 1, 1}
    };

    if (!g_oki_tables_built) {
        for (int step = 0; step <= 48; step++) {
            int stepval = (int)floor(16.0 * pow(11.0 / 10.0, (double)step));
            for (int nib = 0; nib < 16; nib++) {
                g_oki_diff_lookup[step * 16 + nib] = nbl2bit[nib][0] *
                    (stepval     * nbl2bit[nib][1] +
                     stepval / 2 * nbl2bit[nib][2] +
                     stepval / 4 * nbl2bit[nib][3] +
                     stepval / 8);
            }
        }

        for (int step = 0; step < 16; step++) {
            double out = 256.0;
            for (int v = 0; v < step; v++) {
                out /= 1.412537545;
            }
            g_oki_volume_table[step] = (uint32_t)out;
        }
        g_oki_tables_built = 1;
    }

    memset(g_oki_voices, 0, sizeof(g_oki_voices));
    g_oki_command   = -1;
    g_oki_bank_base = 0;
    uint32_t oki_hz = g_cfg.oki_hz ? g_cfg.oki_hz : 7576u;
    uint32_t step_val = (uint32_t)(((uint64_t)oki_hz * OKI_FRAC_ONE) / CPS1_AUDIO_RATE);
    for (int i = 0; i < OKI_VOICES; i++) {
        g_oki_voices[i].volume      = 255;
        g_oki_voices[i].signal      = -2;
        g_oki_voices[i].source_step = step_val;
    }
}

static uint8_t oki6295_status_r(void)
{
    uint8_t res = 0xF0;
    for (int i = 0; i < OKI_VOICES; i++) {
        if (g_oki_voices[i].playing) {
            res |= (1 << i);
        }
    }
    return res;
}

static void oki6295_data_w(uint8_t data)
{
    if (!g_oki_rom || g_oki_rom_size == 0) return;

    if (g_oki_command != -1) {
        int temp = data >> 4;
        for (int i = 0; i < OKI_VOICES; i++, temp >>= 1) {
            if (temp & 1) {
                oki_voice_t *v = &g_oki_voices[i];
                uint32_t tbl_off = (g_oki_bank_base + (uint32_t)g_oki_command * 8u) % g_oki_rom_size;
                const uint8_t *base = &g_oki_rom[tbl_off];
                uint32_t start = (((uint32_t)base[0] << 16) | ((uint32_t)base[1] << 8) | base[2]) & 0x3FFFFu;
                uint32_t stop  = (((uint32_t)base[3] << 16) | ((uint32_t)base[4] << 8) | base[5]) & 0x3FFFFu;
                uint32_t abs_start = g_oki_bank_base + start;
                uint32_t abs_stop  = g_oki_bank_base + stop;

                if (abs_start < g_oki_rom_size && abs_stop < g_oki_rom_size && start <= stop) {
                    v->playing = 1;
                    v->base    = &g_oki_rom[abs_start];
                    v->sample  = 0;
                    v->count   = 2 * (stop - start + 1);
                    v->signal  = -2;
                    v->step    = 0;
                    v->volume  = g_oki_volume_table[data & 0x0F];
                } else {
                    v->playing = 0;
                }
            }
        }
        g_oki_command = -1;
    } else if (data & 0x80) {
        g_oki_command = data & 0x7F;
    } else {
        int temp = data >> 3;
        for (int i = 0; i < OKI_VOICES; i++, temp >>= 1) {
            if (temp & 1) {
                g_oki_voices[i].playing = 0;
            }
        }
    }
}

static void oki6295_generate_raw(oki_voice_t *v, int16_t *buf, int samples)
{
    if (v->playing) {
        const uint8_t *base = v->base;
        uint32_t sample = v->sample;
        int32_t  signal = v->signal;
        uint32_t count  = v->count;
        int32_t  step   = v->step;

        while (samples > 0) {
            int val = base[sample >> 1] >> (((sample & 1) << 2) ^ 4);
            signal += g_oki_diff_lookup[step * 16 + (val & 15)];
            if (signal > 2047)       signal = 2047;
            else if (signal < -2048) signal = -2048;

            step += g_oki_index_shift[val & 7];
            if (step > 48)      step = 48;
            else if (step < 0)  step = 0;

            *buf++ = (int16_t)((signal * (int32_t)v->volume) / 16);
            samples--;

            if (++sample > count) {
                v->playing = 0;
                break;
            }
        }
        v->sample = sample;
        v->signal = signal;
        v->step   = step;
    }
    while (samples-- > 0) {
        *buf++ = 0;
    }
}

static void oki6295_update_voice(oki_voice_t *v, int16_t *buffer, int length)
{
    int16_t sample_data[256];
    int16_t *curr_data = sample_data;
    int16_t prev = v->last_sample;
    int16_t curr = v->curr_sample;

    if (v->source_pos > 0) {
        while (length > 0 && v->source_pos < OKI_FRAC_ONE) {
            *buffer++ = (int16_t)((((int32_t)prev * (int32_t)(OKI_FRAC_ONE - v->source_pos)) +
                                   ((int32_t)curr * (int32_t)v->source_pos)) >> OKI_FRAC_BITS);
            v->source_pos += v->source_step;
            length--;
        }
        if (v->source_pos >= OKI_FRAC_ONE)
            v->source_pos -= OKI_FRAC_ONE;
        else
            return;
    }

    uint32_t final_pos   = v->source_pos + (uint32_t)length * v->source_step;
    uint32_t new_samples = (final_pos + OKI_FRAC_ONE - 1) >> OKI_FRAC_BITS;
    if (new_samples > 256) new_samples = 256;

    oki6295_generate_raw(v, sample_data, (int)new_samples);
    prev = curr;
    curr = *curr_data++;

    while (length > 0) {
        while (length > 0 && v->source_pos < OKI_FRAC_ONE) {
            *buffer++ = (int16_t)((((int32_t)prev * (int32_t)(OKI_FRAC_ONE - v->source_pos)) +
                                   ((int32_t)curr * (int32_t)v->source_pos)) >> OKI_FRAC_BITS);
            v->source_pos += v->source_step;
            length--;
        }
        if (v->source_pos >= OKI_FRAC_ONE) {
            v->source_pos -= OKI_FRAC_ONE;
            prev = curr;
            curr = *curr_data++;
        }
    }

    v->last_sample = prev;
    v->curr_sample = curr;
}

/* ======================================================================== */
/*                        Z80 SOUND BUS HANDLERS                            */
/* ======================================================================== */

static int cps1_z80_irq_ack(int irqline)
{
    (void)irqline;
    return 0xFF;
}

static void cps1_ym2151_irq_handler(int irq)
{
    z80_set_irq_line(0, irq ? ASSERT_LINE : CLEAR_LINE);
}

static void cps1_oki_bankswitch(int offset, int data)
{
    (void)offset;
    uint32_t bank = (uint32_t)(data & 1) * 0x40000u;
    g_oki_bank_base = (bank < g_oki_rom_size) ? bank : 0;
}

uint8_t cps1_z80_read(uint16_t addr)
{
    if (addr < 0x8000) {
        return g_z80_rom[addr];
    }
    if (addr < 0xC000) {
        /* Support both 64KB single-ROM (bank at 0x8000..0xFFFF) and 64KB+32KB (bank at 0x10000..0x17FFF) */
        uint32_t base = (g_z80_rom_size > 0x10000u) ? 0x10000u : 0x8000u;
        uint32_t bank_off = base + ((uint32_t)(g_z80_bank & 1) * 0x4000u) + (addr & 0x3FFFu);
        if (bank_off < g_z80_rom_size)
            return g_z80_rom[bank_off];
        return g_z80_rom[addr];
    }
    if (addr >= 0xD000 && addr <= 0xD7FF) {
        return g_z80_ram[addr & 0x7FF];
    }
    switch (addr) {
        case 0xF001: return (uint8_t)YM2151ReadStatus(0);
        case 0xF002: return oki6295_status_r();
        case 0xF008: return g_soundlatch;
        case 0xF00A: return g_sound_fade_timer;
        default:     return 0xFF;
    }
}

void cps1_z80_write(uint16_t addr, uint8_t val)
{
    if (addr >= 0xD000 && addr <= 0xD7FF) {
        g_z80_ram[addr & 0x7FF] = val;
        return;
    }
    switch (addr) {
        case 0xF000:
            g_ym2151_reg = val;
            break;
        case 0xF001:
            YM2151WriteReg(0, g_ym2151_reg, val);
            break;
        case 0xF002:
            oki6295_data_w(val);
            break;
        case 0xF004:
            g_z80_bank = val & 1;
            break;
        default:
            break;
    }
}

/* ======================================================================== */
/*                       M68000 MEMORY BUS HANDLERS                         */
/* ======================================================================== */

static uint16_t cps1_read_cram16(uint32_t port)
{
    uint32_t reg = port & 0x7Eu;

    /* Street Fighter II 6-button Kick Harness (0x800176 / 0x8001FC) */
    if (g_cfg.input_type == CPS1_INPUT_SF2 && (reg == 0x76u || port == 0xFCu || port == 0x7Cu)) {
        return g_in_p3;
    }

    /* CPS-B chip ID register */
    if (g_cfg.cpsb_addr != 0 && reg == g_cfg.cpsb_addr) {
        return g_cfg.cpsb_value;
    }

    /* CPS-B 16x16 = 32-bit unsigned hardware multiplier protection */
    if (g_cfg.mult_factor1 != 0 && g_cfg.mult_factor2 != 0) {
        if (reg == g_cfg.mult_result_lo) {
            uint32_t f1 = read_be16(&g_output[g_cfg.mult_factor1 & 0xFEu]);
            uint32_t f2 = read_be16(&g_output[g_cfg.mult_factor2 & 0xFEu]);
            return (uint16_t)((f1 * f2) & 0xFFFFu);
        }
        if (reg == g_cfg.mult_result_hi) {
            uint32_t f1 = read_be16(&g_output[g_cfg.mult_factor1 & 0xFEu]);
            uint32_t f2 = read_be16(&g_output[g_cfg.mult_factor2 & 0xFEu]);
            return (uint16_t)((f1 * f2) >> 16);
        }
    }

    /* Pang! 3 (kludge == 5) EEPROM read at 0x80017A */
    if (g_cfg.kludge == 5 && reg == 0x7Au) {
        return eeprom_read_bit() & 0x01u;
    }

    return read_be16(&g_output[reg]);
}

unsigned int m68k_read_memory_8(unsigned int address)
{
    address &= 0xFFFFFF;
    if (address < M68K_MAX_ROM_SIZE) {
        return g_m68k_rom[address ^ 1];
    }
    if (address >= 0x900000 && address <= 0x92FFFF) {
        return g_gfxram[address - 0x900000];
    }
    if (address >= 0xFF0000) {
        return g_mainram[(address & 0xFFFF) ^ 1];
    }
    if (address >= 0x800000 && address <= 0x8001FF) {
        uint32_t off = address & 0x1FF;
        if (off < 0x04 || (off >= 0x10 && off < 0x14)) {
            return (off & 1) ? g_in_p1 : g_in_p2;
        }
        if (off == 0x18 || off == 0x19) return g_in_sys;
        if (off == 0x1A || off == 0x1B) return g_in_dswa;
        if (off == 0x1C || off == 0x1D) return g_in_dswb;
        if (off == 0x1E || off == 0x1F) return g_in_dswc;
        if (g_cfg.input_type == CPS1_INPUT_FORGOTTN && off >= 0x52 && off <= 0x5D) {
            int plr = (off >= 0x58) ? 1 : 0;
            uint16_t v = (uint16_t)(g_dial_val[plr] & 0x0FFF);
            if ((off & 6) == 2) return (off & 1) ? (v & 0xFF) : 0;
            if ((off & 6) == 4) return (off & 1) ? ((v >> 8) & 0x0F) : 0;
            return 0;
        }
        if (off >= 0x100) {
            uint32_t port = off - 0x100;
            uint16_t w = cps1_read_cram16(port & ~1u);
            return (port & 1) ? (w & 0xFF) : (w >> 8);
        }
    }
    if (address >= 0xF1C000 && address <= 0xF1C001) {
        return (address & 1) ? (g_in_p3 & 0xFF) : (g_in_p3 >> 8);
    }
    if (address >= 0xF1C006 && address <= 0xF1C007) {
        return (address & 1) ? (eeprom_read_bit() & 0x01) : 0xFF;
    }
    return 0xFF;
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    address &= 0xFFFFFF;
    if (address < M68K_MAX_ROM_SIZE) {
        return *(const uint16_t *)&g_m68k_rom[address & 0x1FFFFEu];
    }
    if (address >= 0x900000 && address <= 0x92FFFF) {
        return read_be16(&g_gfxram[address - 0x900000]);
    }
    if (address >= 0xFF0000) {
        return *(const uint16_t *)&g_mainram[address & 0xFFFE];
    }
    if (address >= 0x800000 && address <= 0x8001FF) {
        uint32_t off = address & 0x1FE;
        if (off < 0x04 || (off >= 0x10 && off < 0x14)) {
            return ((uint16_t)g_in_p2 << 8) | g_in_p1;
        }
        if (off == 0x18) return ((uint16_t)g_in_sys  << 8) | g_in_sys;
        if (off == 0x1A) return ((uint16_t)g_in_dswa << 8) | g_in_dswa;
        if (off == 0x1C) return ((uint16_t)g_in_dswb << 8) | g_in_dswb;
        if (off == 0x1E) return ((uint16_t)g_in_dswc << 8) | g_in_dswc;
        if (g_cfg.input_type == CPS1_INPUT_FORGOTTN && off >= 0x52 && off <= 0x5C) {
            int plr = (off >= 0x58) ? 1 : 0;
            uint16_t v = (uint16_t)(g_dial_val[plr] & 0x0FFF);
            if ((off & 6) == 2) return v & 0xFF;
            if ((off & 6) == 4) return (v >> 8) & 0x0F;
            return 0;
        }
        if (off >= 0x100) {
            return cps1_read_cram16(off - 0x100);
        }
    }
    if (address == 0xF1C000) return g_in_p3;
    if (address == 0xF1C006) return 0xFF00u | (eeprom_read_bit() & 0x01u);
    return 0xFFFF;
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    address &= 0xFFFFFF;
    if (address <= M68K_MAX_ROM_SIZE - 4) {
        const uint16_t *p = (const uint16_t *)&g_m68k_rom[address & 0x1FFFFEu];
        return ((uint32_t)p[0] << 16) | p[1];
    }
    if (address >= 0x900000 && address <= 0x92FFFC) {
        return read_be32(&g_gfxram[address - 0x900000]);
    }
    if (address >= 0xFF0000) {
        const uint16_t *p = (const uint16_t *)&g_mainram[address & 0xFFFE];
        return ((uint32_t)p[0] << 16) | p[1];
    }
    return (m68k_read_memory_16(address) << 16) | m68k_read_memory_16(address + 2);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    address &= 0xFFFFFF;
    uint8_t v = (uint8_t)(value & 0xFF);

    if (address >= 0x900000 && address <= 0x92FFFF) {
        g_gfxram[address - 0x900000] = v;
        return;
    }
    if (address >= 0xFF0000) {
        g_mainram[(address & 0xFFFF) ^ 1] = v;
        return;
    }
    if (g_cfg.input_type == CPS1_INPUT_FORGOTTN && address >= 0x800040 && address <= 0x800049) {
        g_dial_val[(address >= 0x800048) ? 1 : 0] = 0;
        return;
    }
    if (address >= 0x800100 && address <= 0x8001FF) {
        uint32_t port = address - 0x800100;
        if (g_cfg.kludge == 5 && (port == 0x7A || port == 0x7B)) {
            if (port == 0x7B)
                eeprom_write((v >> 7) & 1, (v >> 6) & 1, v & 1);
            return;
        }
        g_output[port] = v;
        if (port == 0x80 || port == 0x81) {
            if (port == 0x81 || v != 0xFF) {
                g_soundlatch = v;
            }
        } else if (port == 0x88 || port == 0x89) {
            if (port == 0x89 || v != 0xFF) {
                g_sound_fade_timer = v;
            }
        }
        return;
    }
    if (address == 0xF1C007) {
        eeprom_write((v >> 2) & 1, (v >> 1) & 1, v & 1);
    }
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    address &= 0xFFFFFF;
    uint16_t v = (uint16_t)(value & 0xFFFF);

    if (address >= 0x900000 && address <= 0x92FFFE) {
        write_be16(&g_gfxram[address - 0x900000], v);
        return;
    }
    if (address >= 0xFF0000) {
        *(uint16_t *)&g_mainram[address & 0xFFFE] = v;
        return;
    }
    if (g_cfg.input_type == CPS1_INPUT_FORGOTTN && address >= 0x800040 && address <= 0x800048) {
        g_dial_val[(address >= 0x800048) ? 1 : 0] = 0;
        return;
    }
    if (address >= 0x800100 && address <= 0x8001FE) {
        uint32_t port = address - 0x800100;
        if (g_cfg.kludge == 5 && (port & 0x7Eu) == 0x7Au) {
            eeprom_write((v >> 7) & 1, (v >> 6) & 1, v & 1);
            return;
        }
        write_be16(&g_output[port], v);
        if (port == 0x80) {
            g_soundlatch = (uint8_t)(v & 0xFF);
        } else if (port == 0x88) {
            g_sound_fade_timer = (uint8_t)(v & 0xFF);
        }
        /* Mirror 0x800180..0x8001FE into 0x00..0x7E when non-zero (used by SF2) */
        if (port >= 0x80 && v != 0 && port != 0x80 && port != 0x88) {
            write_be16(&g_output[port & 0x7Eu], v);
        }
        return;
    }
    if (address == 0xF1C006) {
        eeprom_write((v >> 2) & 1, (v >> 1) & 1, v & 1);
    }
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    address &= 0xFFFFFF;
    if (address >= 0x900000 && address <= 0x92FFFC) {
        write_be32(&g_gfxram[address - 0x900000], value);
        return;
    }
    if (address >= 0xFF0000) {
        uint16_t *p = (uint16_t *)&g_mainram[address & 0xFFFE];
        p[0] = (uint16_t)(value >> 16);
        p[1] = (uint16_t)value;
        return;
    }
    m68k_write_memory_16(address, (value >> 16) & 0xFFFF);
    m68k_write_memory_16(address + 2, value & 0xFFFF);
}

/* ======================================================================== */
/*                          CPS-1 VIDEO RENDERER                            */
/* ======================================================================== */

#define CPS1_OBJ_BASE           0x00
#define CPS1_SCROLL1_BASE       0x02
#define CPS1_SCROLL2_BASE       0x04
#define CPS1_SCROLL3_BASE       0x06
#define CPS1_OTHER_BASE         0x08
#define CPS1_PALETTE_BASE       0x0a
#define CPS1_SCROLL1_SCROLLX    0x0c
#define CPS1_SCROLL1_SCROLLY    0x0e
#define CPS1_SCROLL2_SCROLLX    0x10
#define CPS1_SCROLL2_SCROLLY    0x12
#define CPS1_SCROLL3_SCROLLX    0x14
#define CPS1_SCROLL3_SCROLLY    0x16
#define CPS1_ROWSCROLL_OFFS     0x20
#define CPS1_VIDEOCONTROL       0x22

#define CPS1_SCROLL1_SIZE       0x4000
#define CPS1_SCROLL2_SIZE       0x4000
#define CPS1_SCROLL3_SIZE       0x4000
#define CPS1_OBJ_SIZE           0x0800
#define CPS1_OTHER_SIZE         0x0800
#define CPS1_PALETTE_SIZE       (32 * 4 * 32)

static const uint8_t *cps1_scroll1;
static const uint8_t *cps1_scroll2;
static const uint8_t *cps1_scroll3;
static const uint8_t *cps1_obj;
static const uint8_t *cps1_palette;
static const uint8_t *cps1_other;

static int scroll1x, scroll1y, scroll2x, scroll2y, scroll3x, scroll3y;
static int cps1_transparency_scroll[4];
static int cps1_layer_enabled[4];
static int cps1_last_sprite_offset;

static inline int cps1_port(int offset)
{
    return read_be16(&g_output[offset & 0xFE]);
}

static inline const uint8_t *cps1_base(int offset, int boundary)
{
    int base = cps1_port(offset) * 256;
    base &= ~(boundary - 1);
    return &g_gfxram[base & 0x3FFFF];
}

static void cps1_get_video_base(void)
{
    cps1_scroll1 = cps1_base(CPS1_SCROLL1_BASE, CPS1_SCROLL1_SIZE);
    cps1_scroll2 = cps1_base(CPS1_SCROLL2_BASE, CPS1_SCROLL2_SIZE);
    cps1_scroll3 = cps1_base(CPS1_SCROLL3_BASE, CPS1_SCROLL3_SIZE);
    cps1_obj     = cps1_base(CPS1_OBJ_BASE,     CPS1_OBJ_SIZE);
    cps1_palette = cps1_base(CPS1_PALETTE_BASE, CPS1_PALETTE_SIZE);
    cps1_other   = cps1_base(CPS1_OTHER_BASE,   CPS1_OTHER_SIZE);

    scroll1x = cps1_port(CPS1_SCROLL1_SCROLLX);
    scroll1y = cps1_port(CPS1_SCROLL1_SCROLLY);
    scroll2x = cps1_port(CPS1_SCROLL2_SCROLLX);
    scroll2y = cps1_port(CPS1_SCROLL2_SCROLLY);
    scroll3x = cps1_port(CPS1_SCROLL3_SCROLLX);
    scroll3y = cps1_port(CPS1_SCROLL3_SCROLLY);

    if (g_cfg.priority1) {
        cps1_transparency_scroll[0] = cps1_port(g_cfg.priority0);
        cps1_transparency_scroll[1] = cps1_port(g_cfg.priority1);
        cps1_transparency_scroll[2] = cps1_port(g_cfg.priority2);
        cps1_transparency_scroll[3] = cps1_port(g_cfg.priority3);
    } else {
        cps1_transparency_scroll[0] = 0x7FFF;
        cps1_transparency_scroll[1] = 0x7FFF;
        cps1_transparency_scroll[2] = 0x7FFF;
        cps1_transparency_scroll[3] = 0x7FFF;
    }

    int layercontrol = cps1_port(g_cfg.layer_control);
    cps1_layer_enabled[0] = 1;
    cps1_layer_enabled[1] = layercontrol & g_cfg.scrl1_enable_mask;
    cps1_layer_enabled[2] = layercontrol & g_cfg.scrl2_enable_mask;
    cps1_layer_enabled[3] = layercontrol & g_cfg.scrl3_enable_mask;
}

#if defined(__sh__) || defined(HOST_TA_SIM)
static uint16_t g_argb1555_pal[32 * 4 * 16];
static uint32_t g_pal_gen[128];
static uint32_t s_hw_bank_key[64];
static uint32_t s_hw_bank_gen[64];
#endif
static uint16_t g_last_pal[32 * 4 * 16];
static int      g_pal_init = 0;

static void cps1_build_palette(void)
{
    const uint16_t *src16 = (const uint16_t *)cps1_palette;

    for (int offset = 0; offset < 32 * 4 * 16; offset++) {
        uint16_t raw = src16[offset];
        if (g_pal_init && g_last_pal[offset] == raw) {
            continue;
        }
        g_last_pal[offset] = raw;

        uint16_t palette = __builtin_bswap16(raw);
        int bright = (palette >> 12) & 0x0F;
        if (bright) bright += 2;

        int r = ((palette >> 8) & 0x0F) * bright;
        int g = ((palette >> 4) & 0x0F) * bright;
        int b = (palette & 0x0F) * bright;

        g_rgb565_pal[offset] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
#if defined(__sh__) || defined(HOST_TA_SIM)
        g_argb1555_pal[offset] = (uint16_t)(0x8000u | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        g_pal_gen[offset >> 4]++;
#endif
    }
    g_pal_init = 1;
}

/* Generic tile renderer for Scroll 1 (8x8), Scroll 2 (16x16), Scroll 3 (32x32) */
static void cps1_draw_tile_layer(
    uint32_t code,
    int color,
    int flipx,
    int flipy,
    int sx,
    int sy,
    int tpens,
    const uint16_t *pusage,
    int size,
    uint32_t max_code,
    int delta,
    int srcdelta,
    int pal_base,
    int to_pri_buf
)
{
    if (!g_cps1_gfx || code >= max_code || (tpens & pusage[code]) == 0) {
        return;
    }
    if (sx <= 32 - size || sx >= 416 || sy <= 48 - size || sy >= 272) {
        return;
    }

    const uint16_t *paldata = &g_rgb565_pal[(pal_base + (color & 0x1F)) * 16];
    const uint32_t *src     = &g_cps1_gfx[code * (uint32_t)delta];
    const int chunks        = size >> 3;

    if (flipy) sy += size - 1;

    /* Fast branchless path for completely opaque tiles (no pen 15 used or tpens == 0xFFFF) */
    if (!to_pri_buf && !flipx && (tpens == 0xFFFF || (tpens == 0x7FFF && (pusage[code] & 0x8000) == 0))) {
        if ((sx & 1) == 0) {
            for (int i = 0; i < size; i++) {
                int y = flipy ? (sy - i) : (sy + i);
                uint32_t *bm32 = (uint32_t *)&g_canvas[y][sx];
                for (int j = 0; j < chunks; j++) {
                    uint32_t dw = *src++;
                    bm32[0] = (uint32_t)paldata[(dw >> 28)]        | ((uint32_t)paldata[(dw >> 24) & 0x0F] << 16);
                    bm32[1] = (uint32_t)paldata[(dw >> 20) & 0x0F] | ((uint32_t)paldata[(dw >> 16) & 0x0F] << 16);
                    bm32[2] = (uint32_t)paldata[(dw >> 12) & 0x0F] | ((uint32_t)paldata[(dw >>  8) & 0x0F] << 16);
                    bm32[3] = (uint32_t)paldata[(dw >>  4) & 0x0F] | ((uint32_t)paldata[ dw        & 0x0F] << 16);
                    bm32 += 4;
                }
                src += srcdelta;
            }
        } else {
            for (int i = 0; i < size; i++) {
                int y = flipy ? (sy - i) : (sy + i);
                uint16_t *bm = &g_canvas[y][sx];
                for (int j = 0; j < chunks; j++) {
                    uint32_t dw = *src++;
                    bm[0] = paldata[(dw >> 28)];
                    bm[1] = paldata[(dw >> 24) & 0x0F];
                    bm[2] = paldata[(dw >> 20) & 0x0F];
                    bm[3] = paldata[(dw >> 16) & 0x0F];
                    bm[4] = paldata[(dw >> 12) & 0x0F];
                    bm[5] = paldata[(dw >>  8) & 0x0F];
                    bm[6] = paldata[(dw >>  4) & 0x0F];
                    bm[7] = paldata[ dw        & 0x0F];
                    bm += 8;
                }
                src += srcdelta;
            }
        }
        return;
    }

    if (!to_pri_buf && !flipx && tpens == 0x7FFF) {
        for (int i = 0; i < size; i++) {
            int y = flipy ? (sy - i) : (sy + i);
            uint16_t *bm = &g_canvas[y][sx];
            for (int j = 0; j < chunks; j++) {
                uint32_t dw = *src++;
                if (dw != 0xFFFFFFFFu) {
                    uint32_t n;
                    n = (dw >> 28);        if (n != 15) bm[0] = paldata[n];
                    n = (dw >> 24) & 0x0F; if (n != 15) bm[1] = paldata[n];
                    n = (dw >> 20) & 0x0F; if (n != 15) bm[2] = paldata[n];
                    n = (dw >> 16) & 0x0F; if (n != 15) bm[3] = paldata[n];
                    n = (dw >> 12) & 0x0F; if (n != 15) bm[4] = paldata[n];
                    n = (dw >>  8) & 0x0F; if (n != 15) bm[5] = paldata[n];
                    n = (dw >>  4) & 0x0F; if (n != 15) bm[6] = paldata[n];
                    n =  dw        & 0x0F; if (n != 15) bm[7] = paldata[n];
                }
                bm += 8;
            }
            src += srcdelta;
        }
        return;
    }

    if (flipx) {
        sx += size;
        for (int i = 0; i < size; i++) {
            int y = flipy ? (sy - i) : (sy + i);
            if (to_pri_buf) {
                uint8_t *pri = &g_pri_buf[y][sx];
                for (int j = 0; j < chunks; j++) {
                    uint32_t dw = *src++;
                    if (dw != 0xFFFFFFFFu) {
                        if (tpens & (1u << ((dw >> 28))))        pri[-1] = 1;
                        if (tpens & (1u << ((dw >> 24) & 0x0F))) pri[-2] = 1;
                        if (tpens & (1u << ((dw >> 20) & 0x0F))) pri[-3] = 1;
                        if (tpens & (1u << ((dw >> 16) & 0x0F))) pri[-4] = 1;
                        if (tpens & (1u << ((dw >> 12) & 0x0F))) pri[-5] = 1;
                        if (tpens & (1u << ((dw >>  8) & 0x0F))) pri[-6] = 1;
                        if (tpens & (1u << ((dw >>  4) & 0x0F))) pri[-7] = 1;
                        if (tpens & (1u << ( dw        & 0x0F))) pri[-8] = 1;
                    }
                    pri -= 8;
                }
            } else {
                uint16_t *bm = &g_canvas[y][sx];
                for (int j = 0; j < chunks; j++) {
                    uint32_t dw = *src++;
                    if (dw != 0xFFFFFFFFu || tpens == 0xFFFF) {
                        uint32_t n;
                        n = (dw >> 28);        if (tpens & (1 << n)) bm[-1] = paldata[n];
                        n = (dw >> 24) & 0x0F; if (tpens & (1 << n)) bm[-2] = paldata[n];
                        n = (dw >> 20) & 0x0F; if (tpens & (1 << n)) bm[-3] = paldata[n];
                        n = (dw >> 16) & 0x0F; if (tpens & (1 << n)) bm[-4] = paldata[n];
                        n = (dw >> 12) & 0x0F; if (tpens & (1 << n)) bm[-5] = paldata[n];
                        n = (dw >>  8) & 0x0F; if (tpens & (1 << n)) bm[-6] = paldata[n];
                        n = (dw >>  4) & 0x0F; if (tpens & (1 << n)) bm[-7] = paldata[n];
                        n =  dw        & 0x0F; if (tpens & (1 << n)) bm[-8] = paldata[n];
                    }
                    bm -= 8;
                }
            }
            src += srcdelta;
        }
    } else {
        for (int i = 0; i < size; i++) {
            int y = flipy ? (sy - i) : (sy + i);
            if (to_pri_buf) {
                uint8_t *pri = &g_pri_buf[y][sx];
                for (int j = 0; j < chunks; j++) {
                    uint32_t dw = *src++;
                    if (dw != 0xFFFFFFFFu) {
                        if (tpens & (1u << ((dw >> 28))))        pri[0] = 1;
                        if (tpens & (1u << ((dw >> 24) & 0x0F))) pri[1] = 1;
                        if (tpens & (1u << ((dw >> 20) & 0x0F))) pri[2] = 1;
                        if (tpens & (1u << ((dw >> 16) & 0x0F))) pri[3] = 1;
                        if (tpens & (1u << ((dw >> 12) & 0x0F))) pri[4] = 1;
                        if (tpens & (1u << ((dw >>  8) & 0x0F))) pri[5] = 1;
                        if (tpens & (1u << ((dw >>  4) & 0x0F))) pri[6] = 1;
                        if (tpens & (1u << ( dw        & 0x0F))) pri[7] = 1;
                    }
                    pri += 8;
                }
            } else {
                uint16_t *bm = &g_canvas[y][sx];
                for (int j = 0; j < chunks; j++) {
                    uint32_t dw = *src++;
                    if (dw != 0xFFFFFFFFu || tpens == 0xFFFF) {
                        uint32_t n;
                        n = (dw >> 28);        if (tpens & (1 << n)) bm[0] = paldata[n];
                        n = (dw >> 24) & 0x0F; if (tpens & (1 << n)) bm[1] = paldata[n];
                        n = (dw >> 20) & 0x0F; if (tpens & (1 << n)) bm[2] = paldata[n];
                        n = (dw >> 16) & 0x0F; if (tpens & (1 << n)) bm[3] = paldata[n];
                        n = (dw >> 12) & 0x0F; if (tpens & (1 << n)) bm[4] = paldata[n];
                        n = (dw >>  8) & 0x0F; if (tpens & (1 << n)) bm[5] = paldata[n];
                        n = (dw >>  4) & 0x0F; if (tpens & (1 << n)) bm[6] = paldata[n];
                        n =  dw        & 0x0F; if (tpens & (1 << n)) bm[7] = paldata[n];
                    }
                    bm += 8;
                }
            }
            src += srcdelta;
        }
    }
}

/* Sprite 16x16 renderer with priority mask check */
static void cps1_draw_sprite_tile16(
    uint32_t code,
    int color,
    int flipx,
    int flipy,
    int sx,
    int sy
)
{
    const int tpens = 0x7FFF;
    if (!g_cps1_gfx || code >= g_max_tile16 || (tpens & g_tile16_pu[code]) == 0) {
        return;
    }
    if (sx <= 16 || sx >= 416 || sy <= 32 || sy >= 272) {
        return;
    }

    const uint16_t *paldata = &g_rgb565_pal[(color & 0x1F) * 16];
    const uint32_t *src     = &g_cps1_gfx[code * 32];

    if (flipy) sy += 15;

    if (!g_has_pri_layer) {
        if (flipx) {
            sx += 16;
            for (int i = 0; i < 16; i++) {
                int y = flipy ? (sy - i) : (sy + i);
                uint16_t *bm = &g_canvas[y][sx];
                for (int j = 0; j < 2; j++) {
                    uint32_t dw = *src++;
                    if (dw != 0xFFFFFFFFu) {
                        uint32_t n;
                        n = (dw >> 28);        if (n != 15) bm[-1] = paldata[n];
                        n = (dw >> 24) & 0x0F; if (n != 15) bm[-2] = paldata[n];
                        n = (dw >> 20) & 0x0F; if (n != 15) bm[-3] = paldata[n];
                        n = (dw >> 16) & 0x0F; if (n != 15) bm[-4] = paldata[n];
                        n = (dw >> 12) & 0x0F; if (n != 15) bm[-5] = paldata[n];
                        n = (dw >>  8) & 0x0F; if (n != 15) bm[-6] = paldata[n];
                        n = (dw >>  4) & 0x0F; if (n != 15) bm[-7] = paldata[n];
                        n =  dw        & 0x0F; if (n != 15) bm[-8] = paldata[n];
                    }
                    bm -= 8;
                }
            }
        } else {
            for (int i = 0; i < 16; i++) {
                int y = flipy ? (sy - i) : (sy + i);
                uint16_t *bm = &g_canvas[y][sx];
                for (int j = 0; j < 2; j++) {
                    uint32_t dw = *src++;
                    if (dw != 0xFFFFFFFFu) {
                        uint32_t n;
                        n = (dw >> 28);        if (n != 15) bm[0] = paldata[n];
                        n = (dw >> 24) & 0x0F; if (n != 15) bm[1] = paldata[n];
                        n = (dw >> 20) & 0x0F; if (n != 15) bm[2] = paldata[n];
                        n = (dw >> 16) & 0x0F; if (n != 15) bm[3] = paldata[n];
                        n = (dw >> 12) & 0x0F; if (n != 15) bm[4] = paldata[n];
                        n = (dw >>  8) & 0x0F; if (n != 15) bm[5] = paldata[n];
                        n = (dw >>  4) & 0x0F; if (n != 15) bm[6] = paldata[n];
                        n =  dw        & 0x0F; if (n != 15) bm[7] = paldata[n];
                    }
                    bm += 8;
                }
            }
        }
        return;
    }

    if (flipx) {
        sx += 16;
        for (int i = 0; i < 16; i++) {
            int y = flipy ? (sy - i) : (sy + i);
            uint16_t *bm  = &g_canvas[y][sx];
            const uint8_t *pri = &g_pri_buf[y][sx];
            for (int j = 0; j < 2; j++) {
                uint32_t dw = *src++;
                if (dw != 0xFFFFFFFFu) {
                    uint32_t n;
                    n = (dw >> 28);        if (n != 15 && pri[-1] == 0) bm[-1] = paldata[n];
                    n = (dw >> 24) & 0x0F; if (n != 15 && pri[-2] == 0) bm[-2] = paldata[n];
                    n = (dw >> 20) & 0x0F; if (n != 15 && pri[-3] == 0) bm[-3] = paldata[n];
                    n = (dw >> 16) & 0x0F; if (n != 15 && pri[-4] == 0) bm[-4] = paldata[n];
                    n = (dw >> 12) & 0x0F; if (n != 15 && pri[-5] == 0) bm[-5] = paldata[n];
                    n = (dw >>  8) & 0x0F; if (n != 15 && pri[-6] == 0) bm[-6] = paldata[n];
                    n = (dw >>  4) & 0x0F; if (n != 15 && pri[-7] == 0) bm[-7] = paldata[n];
                    n =  dw        & 0x0F; if (n != 15 && pri[-8] == 0) bm[-8] = paldata[n];
                }
                bm  -= 8;
                pri -= 8;
            }
        }
    } else {
        for (int i = 0; i < 16; i++) {
            int y = flipy ? (sy - i) : (sy + i);
            uint16_t *bm  = &g_canvas[y][sx];
            const uint8_t *pri = &g_pri_buf[y][sx];
            for (int j = 0; j < 2; j++) {
                uint32_t dw = *src++;
                if (dw != 0xFFFFFFFFu) {
                    uint32_t n;
                    n = (dw >> 28);        if (n != 15 && pri[0] == 0) bm[0] = paldata[n];
                    n = (dw >> 24) & 0x0F; if (n != 15 && pri[1] == 0) bm[1] = paldata[n];
                    n = (dw >> 20) & 0x0F; if (n != 15 && pri[2] == 0) bm[2] = paldata[n];
                    n = (dw >> 16) & 0x0F; if (n != 15 && pri[3] == 0) bm[3] = paldata[n];
                    n = (dw >> 12) & 0x0F; if (n != 15 && pri[4] == 0) bm[4] = paldata[n];
                    n = (dw >>  8) & 0x0F; if (n != 15 && pri[5] == 0) bm[5] = paldata[n];
                    n = (dw >>  4) & 0x0F; if (n != 15 && pri[6] == 0) bm[6] = paldata[n];
                    n =  dw        & 0x0F; if (n != 15 && pri[7] == 0) bm[7] = paldata[n];
                }
                bm  += 8;
                pri += 8;
            }
        }
    }
}

static inline uint32_t cps1_map_sprite_code(uint32_t code)
{
    if (g_cfg.kludge == 7)
        return code + 0x4000u;
    if (g_cfg.kludge == 1 && code >= 0x01000u)
        return code + 0x4000u;
    if (g_cfg.kludge == 2 && code >= 0x02A00u)
        return code + 0x4000u;
    return code;
}

static void cps1_render_scroll1(int priority)
{
    int scrlxrough = (scroll1x >> 3) + 4;
    int scrlyrough = (scroll1y >> 3);
    int sx   = -(scroll1x & 0x07);
    int ytop = -(scroll1y & 0x07) + 32;
    int32_t base = g_cfg.bank_scroll1 * 0x08000;

    for (int x = 0; x < 0x35; x++) {
        if (sx <= 24 || sx >= 416) {
            sx += 8;
            continue;
        }
        int sy    = ytop;
        int offsx = ((scrlxrough + x) * 0x80) & 0x1FFF;

        for (int y = 0; y < 0x20; y++) {
            if (sy > 40 && sy < 272) {
                int n     = scrlyrough + y;
                int offsy = (((n & 0x1F) * 4) | ((n & 0x20) * 0x100)) & 0x3FFF;
                int offs  = (offsy + offsx) & 0x3FFF;

                int32_t raw_code = read_be16(&cps1_scroll1[offs]);
                int     colour   = read_be16(&cps1_scroll1[offs + 2]);

                if (raw_code != 0x20 && raw_code != g_cfg.space_scroll1) {
                    int32_t code = raw_code + base;
                    int transp = priority ? cps1_transparency_scroll[(colour & 0x0180) >> 7] : 0x7FFF;
                    if (transp && code >= 0) {
                        cps1_draw_tile_layer(
                            (uint32_t)code,
                            colour & 0x1F,
                            colour & 0x20,
                            colour & 0x40,
                            sx, sy,
                            transp,
                            g_char_pu,
                            8, g_max_char, 16, 1,
                            32, /* Scroll 1 palette base */
                            priority
                        );
                    }
                }
            }
            sy += 8;
        }
        sx += 8;
    }
}

static void cps1_render_scroll2(int priority)
{
    int nyoffset = (scroll2y & 0x0F);
    int ny = (scroll2y >> 4) - 4;
    int32_t base = g_cfg.bank_scroll2 * 0x04000;
    int videocontrol = cps1_port(CPS1_VIDEOCONTROL);
    int otheroffs    = cps1_port(CPS1_ROWSCROLL_OFFS);

    for (int sy = 0; sy < 0x09 * 2; sy++) {
        int py = 16 * sy - nyoffset;
        if (py <= 48 - 16 || py >= 272) continue;

        int eff_scroll2x = scroll2x;
        if (videocontrol & 0x01) {
            int line = (py - 24) & 0xFF;
            eff_scroll2x += (int16_t)read_be16(&cps1_other[(2 * (line + otheroffs)) & 0x7FE]);
        }
        int nxoffset = (eff_scroll2x & 0x0F) + 32;
        int nx       = (eff_scroll2x >> 4);
        int n        = ny + sy + 2;
        int offsy    = (((n & 0x0F) * 4) | ((n & 0x30) * 0x100)) & 0x3FFF;

        for (int sx = 0; sx < 0x32 / 2 + 4; sx++) {
            int offsx = ((nx + sx) * 0x040) & 0x0FFF;
            int offs  = (offsy + offsx) & 0x3FFF;

            int32_t raw_code = read_be16(&cps1_scroll2[offs]);
            int     colour   = read_be16(&cps1_scroll2[offs + 2]);

            if (raw_code >= g_cfg.start_scroll2 && raw_code <= g_cfg.end_scroll2 &&
                !(g_cfg.kludge == 4 && raw_code >= 0x01E00 && raw_code < 0x05400))
            {
                int32_t code = (raw_code & 0x3FFF) + base;
                int transp = priority ? cps1_transparency_scroll[(colour & 0x0180) >> 7] : 0x7FFF;
                if (transp && code >= 0) {
                    cps1_draw_tile_layer(
                        (uint32_t)code,
                        colour & 0x1F,
                        colour & 0x20,
                        colour & 0x40,
                        16 * sx - nxoffset,
                        py,
                        transp,
                        g_tile16_pu,
                        16, g_max_tile16, 32, 0,
                        64, /* Scroll 2 palette base */
                        priority
                    );
                }
            }
        }
    }
}

static void cps1_render_scroll3(int priority)
{
    int nxoffset = scroll3x & 0x1F;
    int nyoffset = scroll3y & 0x1F;
    int nx = (scroll3x >> 5) + 1;
    int ny = (scroll3y >> 5) - 1;
    int32_t base = g_cfg.bank_scroll3 * 0x01000;

    for (int sx = 1; sx < 0x32 / 4 + 2; sx++) {
        for (int sy = 1; sy < 0x20 / 4 + 2; sy++) {
            int n     = ny + sy;
            int offsy = (((n & 0x07) * 4) | ((n & 0xF8) * 0x0100)) & 0x3FFF;
            int offsx = ((nx + sx) * 0x020) & 0x07FF;
            int offs  = (offsy + offsx) & 0x3FFF;

            int32_t raw_code = read_be16(&cps1_scroll3[offs]);
            int     colour   = read_be16(&cps1_scroll3[offs + 2]);

            if (raw_code >= g_cfg.start_scroll3 && raw_code <= g_cfg.end_scroll3) {
                int32_t code = raw_code + base;
                if (g_cfg.kludge == 2 && code >= 0x01500)
                    code -= 0x1000;
                int transp = priority ? cps1_transparency_scroll[(colour & 0x0180) >> 7] : 0x7FFF;
                if (transp && code >= 0) {
                    cps1_draw_tile_layer(
                        (uint32_t)code,
                        colour & 0x1F,
                        colour & 0x20,
                        colour & 0x40,
                        32 * sx - nxoffset,
                        32 * sy - nyoffset,
                        transp,
                        g_tile32_pu,
                        32, g_max_tile32, 128, 0,
                        96, /* Scroll 3 palette base */
                        priority
                    );
                }
            }
        }
    }
}

static void cps1_find_last_sprite(void)
{
    int offset = 6;
    while (offset < CPS1_OBJ_SIZE) {
        uint16_t colour = read_be16(&g_buffered_obj[offset]);
        if (colour >= 0xFF00) {
            cps1_last_sprite_offset = offset - 6 - 8;
            return;
        }
        offset += 8;
    }
    cps1_last_sprite_offset = CPS1_OBJ_SIZE - 8;
}

static void cps1_render_sprites(void)
{
    for (int i = cps1_last_sprite_offset; i >= 0; i -= 8) {
        int x = read_be16(&g_buffered_obj[i]);
        int y = read_be16(&g_buffered_obj[i + 2]);
        if (x && y) {
            uint32_t code   = cps1_map_sprite_code(read_be16(&g_buffered_obj[i + 4]));
            int      colour = read_be16(&g_buffered_obj[i + 6]);
            int      col    = colour & 0x1F;

            y &= 0x1FF;
            if (y > 450) y -= 0x200;
            x &= 0x1FF;
            if (x > 450) x -= 0x200;

            x -= 0x20;
            y += 0x20;

            if (colour & 0xFF00) {
                int nx = ((colour & 0x0F00) >> 8) + 1;
                int ny = ((colour & 0xF000) >> 12) + 1;

                for (int nys = 0; nys < ny; nys++) {
                    for (int nxs = 0; nxs < nx; nxs++) {
                        int sx = x + nxs * 16;
                        int sy = y + nys * 16;
                        if (sx > 450) sx -= 0x200;
                        if (sy > 450) sy -= 0x200;

                        int cx = (colour & 0x20) ? (nx - 1 - nxs) : nxs;
                        int cy = (colour & 0x40) ? (ny - 1 - nys) : nys;
                        uint32_t tile_code = code + (uint32_t)cx + 0x10u * (uint32_t)cy;

                        cps1_draw_sprite_tile16(
                            tile_code,
                            col,
                            colour & 0x20,
                            colour & 0x40,
                            sx, sy
                        );
                    }
                }
            } else {
                cps1_draw_sprite_tile16(
                    code,
                    col,
                    colour & 0x20,
                    colour & 0x40,
                    x, y
                );
            }
        }
    }
}

static void cps1_render_layer(int layer)
{
    if (!cps1_layer_enabled[layer]) return;
#ifdef __sh__
    int pt = g_prof ? profile_start() : -1;
#endif
    switch (layer) {
        case 0: cps1_render_sprites();   break;
        case 1: cps1_render_scroll1(0);  break;
        case 2: cps1_render_scroll2(0);  break;
        case 3: cps1_render_scroll3(0);  break;
    }
#ifdef __sh__
    if (pt >= 0) {
        uint32_t dt = profile_end(pt);
        if (layer == 0) g_prof->gfx_spr_us += dt;
        else if (layer == 1) g_prof->gfx_s1_us += dt;
        else if (layer == 2) g_prof->gfx_s2_us += dt;
        else if (layer == 3) g_prof->gfx_s3_us += dt;
    }
#endif
}

static void cps1_render_high_layer(int layer)
{
    if (!cps1_layer_enabled[layer]) return;
    switch (layer) {
        case 1: cps1_render_scroll1(1);  break;
        case 2: cps1_render_scroll2(1);  break;
        case 3: cps1_render_scroll3(1);  break;
    }
}

static void cps1_render_frame(uint16_t *vis_fb_out)
{
#ifdef __sh__
    if (g_prof) {
        g_prof->gfx_clr_us = 0;
        g_prof->gfx_s1_us  = 0;
        g_prof->gfx_s2_us  = 0;
        g_prof->gfx_s3_us  = 0;
        g_prof->gfx_spr_us = 0;
    }
    int pc = g_prof ? profile_start() : -1;
#endif
    cps1_get_video_base();
    cps1_find_last_sprite();
    cps1_build_palette();

    int layercontrol = cps1_port(g_cfg.layer_control);
    int l0 = (layercontrol >> 0x06) & 3;
    int l1 = (layercontrol >> 0x08) & 3;
    int l2 = (layercontrol >> 0x0A) & 3;
    int l3 = (layercontrol >> 0x0C) & 3;

    g_has_pri_layer = (l1 == 0 || l2 == 0 || l3 == 0);

    uint16_t bg_col = g_rgb565_pal[0x0F];
    uint32_t bg32   = ((uint32_t)bg_col << 16) | bg_col;
#ifdef __sh__
    for (int y = 48; y < 48 + CPS1_VIS_HEIGHT; y++) {
        uint32_t *row32 = (uint32_t *)&g_canvas[y][32];
        for (int c = 0; c < (CPS1_VIS_WIDTH * 2) / 32; c++) {
            __asm__ volatile (
                "movca.l %0, @%1\n\t"
                "mov.l %0, @(4,%1)\n\t"
                "mov.l %0, @(8,%1)\n\t"
                "mov.l %0, @(12,%1)\n\t"
                "mov.l %0, @(16,%1)\n\t"
                "mov.l %0, @(20,%1)\n\t"
                "mov.l %0, @(24,%1)\n\t"
                "mov.l %0, @(28,%1)"
                :
                : "z"(bg32), "r"(row32)
                : "memory"
            );
            row32 += 8;
        }
        if (g_has_pri_layer) {
            uint32_t *pri32 = (uint32_t *)&g_pri_buf[y][32];
            uint32_t zero = 0;
            for (int c = 0; c < CPS1_VIS_WIDTH / 32; c++) {
                __asm__ volatile (
                    "movca.l %0, @%1\n\t"
                    "mov.l %0, @(4,%1)\n\t"
                    "mov.l %0, @(8,%1)\n\t"
                    "mov.l %0, @(12,%1)\n\t"
                    "mov.l %0, @(16,%1)\n\t"
                    "mov.l %0, @(20,%1)\n\t"
                    "mov.l %0, @(24,%1)\n\t"
                    "mov.l %0, @(28,%1)"
                    :
                    : "z"(zero), "r"(pri32)
                    : "memory"
                );
                pri32 += 8;
            }
        }
    }
#else
    for (int y = 48; y < 48 + CPS1_VIS_HEIGHT; y++) {
        uint32_t *row32 = (uint32_t *)&g_canvas[y][32];
        for (int x = 0; x < CPS1_VIS_WIDTH / 2; x++) {
            row32[x] = bg32;
        }
        if (g_has_pri_layer) {
            memset(&g_pri_buf[y][32], 0, CPS1_VIS_WIDTH);
        }
    }
#endif
#ifdef __sh__
    if (pc >= 0) g_prof->gfx_clr_us = profile_end(pc);
#endif

    cps1_render_layer(l0);
    if (l1 == 0) cps1_render_high_layer(l0);
    cps1_render_layer(l1);
    if (l2 == 0) cps1_render_high_layer(l1);
    cps1_render_layer(l2);
    if (l3 == 0) cps1_render_high_layer(l2);
    cps1_render_layer(l3);

    if (vis_fb_out && vis_fb_out != &g_canvas[48][32]) {
        for (int y = 0; y < CPS1_VIS_HEIGHT; y++) {
            memcpy(
                &vis_fb_out[y * CPS1_VIS_WIDTH],
                &g_canvas[48 + y][32],
                CPS1_VIS_WIDTH * sizeof(uint16_t)
            );
        }
    }

    cps1_get_video_base();
    memcpy(g_buffered_obj, cps1_obj, CPS1_OBJ_SIZE);
}

#define HW_MAX_CHAR_SLOTS       0x010000u   /* Up to 65,536 * 32B  = 2.0 MB */
#define HW_MAX_TILE16_CODES     0x020000u   /* Up to 131,072 16x16 tiles (12 MB linear / 6+ MB planar GFX) */
#define HW_MAX_TILE16_SLOTS     0x00FFFEu   /* Max 65,534 slots (0xFFFF reserved as empty sentinel) */
#define HW_MAX_TILE32_SLOTS     0x002000u   /* Up to  8,192 * 512B = 4.0 MB */

typedef struct {
    uint32_t char_slots;
    uint32_t tile16_slots;
    uint32_t tile32_slots;
    uint32_t char_bytes;
    uint32_t tile16_bytes;
    uint32_t tile32_bytes;
    uint32_t total_bytes;
} cps1_hw_vram_layout_t;

static void cps1_compute_hw_vram_layout(
    uint32_t vram_bytes,
    int32_t  kludge,
    uint32_t max_tile16,
    cps1_hw_vram_layout_t *out)
{
    /*
     * Pang! 3 (kludge == 5) addresses Scroll 1 chars 0xA020..0xFFFF and
     * Scroll 3 tiles 0x1800..0x1FFF, so allocate 64K Char + 8K Tile32 slots
     * (6.0 MB) and give remaining VRAM (3.0 MB = 24,576 slots) to Tile16.
     * Standard games address <32K Char and <4K Tile32 (3.0 MB), leaving up
     * to 6.0 MB (49,152 slots) for 16x16 tiles/sprites.
     */
    uint32_t char_slots   = (kludge == 5) ? 0x010000u : 0x008000u;
    uint32_t tile32_slots = (kludge == 5) ? 0x002000u : 0x001000u;
    uint32_t fixed_bytes  = char_slots * 32u + tile32_slots * 512u;

    if (vram_bytes <= fixed_bytes) {
        char_slots   = 0x008000u;
        tile32_slots = 0x001000u;
        fixed_bytes  = char_slots * 32u + tile32_slots * 512u;
    }

    uint32_t rem_bytes    = (vram_bytes > fixed_bytes) ? (vram_bytes - fixed_bytes) : 0u;
    uint32_t tile16_slots = rem_bytes / 128u;
    if (max_tile16 > 0u && tile16_slots > max_tile16)
        tile16_slots = max_tile16;
    if (tile16_slots > HW_MAX_TILE16_SLOTS)
        tile16_slots = HW_MAX_TILE16_SLOTS;

    out->char_slots   = char_slots;
    out->tile16_slots = tile16_slots;
    out->tile32_slots = tile32_slots;
    out->char_bytes   = char_slots * 32u;
    out->tile16_bytes = tile16_slots * 128u;
    out->tile32_bytes = tile32_slots * 512u;
    out->total_bytes  = out->char_bytes + out->tile16_bytes + out->tile32_bytes;
}

#if defined(__sh__) || defined(HOST_TA_SIM)
/* ======================================================================== */
/*              POWERVR2 HARDWARE TILE & SPRITE RENDERER (CLUT4)            */
/* ======================================================================== */

static uint8_t *g_hw_char_vram   = NULL;
static uint8_t *g_hw_tile16_vram = NULL;
static uint8_t *g_hw_tile32_vram = NULL;
static uint32_t g_hw_char_slots   = 0x008000u;
static uint32_t g_hw_tile16_slots = 0x005000u;
static uint32_t g_hw_tile32_slots = 0x001000u;
static uint16_t g_twiddle_tab[32];

/* O(1) 16x16 VRAM slot table supporting up to 8 MB planar GFX (SF2/Pang! 3) */
static uint16_t g_tile16_vram_slot[HW_MAX_TILE16_CODES];
static uint32_t g_tile16_slot_owner[HW_MAX_TILE16_SLOTS];
static uint16_t g_tile16_next_slot = 0;

static void hw_twiddle_tile_clut4(const uint32_t *src, int row_words, int size, uint16_t *dst_uncached)
{
    uint8_t px[32 * 32];
    const int chunks = size >> 3;
    for (int y = 0; y < size; y++) {
        const uint32_t *row = &src[y * row_words];
        for (int c = 0; c < chunks; c++) {
            uint32_t dw = row[c];
            uint8_t *p = &px[y * size + c * 8];
            p[0] = (uint8_t)((dw >> 28) & 0x0F);
            p[1] = (uint8_t)((dw >> 24) & 0x0F);
            p[2] = (uint8_t)((dw >> 20) & 0x0F);
            p[3] = (uint8_t)((dw >> 16) & 0x0F);
            p[4] = (uint8_t)((dw >> 12) & 0x0F);
            p[5] = (uint8_t)((dw >>  8) & 0x0F);
            p[6] = (uint8_t)((dw >>  4) & 0x0F);
            p[7] = (uint8_t)( dw        & 0x0F);
        }
    }
    for (int v = 0; v < size; v += 2) {
        uint32_t tv0 = g_twiddle_tab[v];
        for (int u = 0; u < size; u += 2) {
            uint32_t tw = tv0 | (g_twiddle_tab[u] << 1);
            dst_uncached[tw >> 2] = (uint16_t)(
                px[v * size + u] |
                (px[(v + 1) * size + u] << 4) |
                (px[v * size + (u + 1)] << 8) |
                (px[(v + 1) * size + (u + 1)] << 12)
            );
        }
    }
}

static inline const uint8_t *hw_get_tile16_vram_ptr(uint32_t code)
{
    if (__builtin_expect(code >= HW_MAX_TILE16_CODES, 0))
        return g_hw_tile16_vram;
    uint16_t slot = g_tile16_vram_slot[code];
    if (__builtin_expect(slot == 0xFFFFu, 0)) {
        slot = g_tile16_next_slot;
        if (++g_tile16_next_slot >= g_hw_tile16_slots)
            g_tile16_next_slot = 0;
        uint32_t old_code = g_tile16_slot_owner[slot];
        if (old_code != 0xFFFFFFFFu)
            g_tile16_vram_slot[old_code] = 0xFFFFu;
        g_tile16_slot_owner[slot] = code;
        g_tile16_vram_slot[code]  = slot;

        if (g_cps1_gfx && code < g_max_tile16) {
            uint16_t *dst = HW_VRAM_UNCACHED(g_hw_tile16_vram + (uint32_t)slot * 128u);
            hw_twiddle_tile_clut4(&g_cps1_gfx[code * 32u], 2, 16, dst);
        }
    }
    return g_hw_tile16_vram + (uint32_t)slot * 128u;
}

void cps1_hw_gfx_init(void)
{
    for (int i = 0; i < 32; i++) {
        g_twiddle_tab[i] = (uint16_t)(
            (i & 1) | ((i & 2) << 1) | ((i & 4) << 2) | ((i & 8) << 3) | ((i & 16) << 4)
        );
    }

    cps1_hw_vram_layout_t layout;
    cps1_compute_hw_vram_layout((uint32_t)ta_texture_size(), g_cfg.kludge, g_max_tile16, &layout);
    g_hw_char_slots   = layout.char_slots;
    g_hw_tile16_slots = layout.tile16_slots;
    g_hw_tile32_slots = layout.tile32_slots;

    uint8_t *base = (uint8_t *)ta_texture_base();
    g_hw_char_vram   = base;
    g_hw_tile16_vram = g_hw_char_vram   + layout.char_bytes;
    g_hw_tile32_vram = g_hw_tile16_vram + layout.tile16_bytes;

    /* 1. Pre-twiddle Scroll 1 8x8 chars (applying bank_scroll1 offset) */
    int32_t char_base = g_cfg.bank_scroll1 * 0x08000;
    for (uint32_t raw_c = 0; raw_c < g_hw_char_slots; raw_c++) {
        int32_t c = (int32_t)raw_c + char_base;
        if (c >= 0 && (uint32_t)c < g_max_char && g_char_pu[c]) {
            uint16_t *dst = HW_VRAM_UNCACHED(g_hw_char_vram + raw_c * 32u);
            hw_twiddle_tile_clut4(&g_cps1_gfx[(uint32_t)c * 16u], 2, 8, dst);
        }
    }

    /* 2. Initialize 16x16 VRAM slot table (pre-twiddle 1:1 if all fit in pool) */
    memset(g_tile16_vram_slot,  0xFF, sizeof(g_tile16_vram_slot));
    memset(g_tile16_slot_owner, 0xFF, sizeof(g_tile16_slot_owner));
    g_tile16_next_slot = 0;

    if (g_max_tile16 <= g_hw_tile16_slots) {
        for (uint32_t c = 0; c < g_max_tile16; c++) {
            g_tile16_vram_slot[c]  = (uint16_t)c;
            g_tile16_slot_owner[c] = c;
            if (g_tile16_pu[c]) {
                uint16_t *dst = HW_VRAM_UNCACHED(g_hw_tile16_vram + c * 128u);
                hw_twiddle_tile_clut4(&g_cps1_gfx[c * 32u], 2, 16, dst);
            }
        }
    }

    /* 3. Pre-twiddle Scroll 3 32x32 tiles (applying bank_scroll3 & kludge==2) */
    int32_t tile32_base = g_cfg.bank_scroll3 * 0x01000;
    for (uint32_t raw_c = 0; raw_c < g_hw_tile32_slots; raw_c++) {
        int32_t c = (int32_t)raw_c + tile32_base;
        if (g_cfg.kludge == 2 && c >= 0x01500)
            c -= 0x1000;
        if (c >= 0 && (uint32_t)c < g_max_tile32 && g_tile32_pu[c]) {
            uint16_t *dst = HW_VRAM_UNCACHED(g_hw_tile32_vram + raw_c * 512u);
            hw_twiddle_tile_clut4(&g_cps1_gfx[(uint32_t)c * 128u], 4, 32, dst);
        }
    }

    /* Free planar GFX buffer if all 16x16 tiles are permanently resident in VRAM */
    if (g_max_tile16 <= g_hw_tile16_slots && g_cps1_gfx) {
        free(g_cps1_gfx);
        g_cps1_gfx = NULL;
    }
}

typedef struct {
    float scale_x, scale_y;
    float off_x, off_y;
    float vw;
    int   vert;
    uint32_t base_mode2;
    uint32_t list_type;
    float    px_lut[640];
    float    py_lut[512];
    uint8_t  bank_7fff[128];
    uint32_t bank_keys[64];
    int      bank_count;
} hw_ta_ctx_t;

static inline int hw_alloc_bank(hw_ta_ctx_t *ctx, int pal_idx, int tpens)
{
    uint32_t key = ((uint32_t)pal_idx << 16) | (uint32_t)(tpens & 0xFFFF);
    if (tpens == 0x7FFF) {
        uint8_t b = ctx->bank_7fff[pal_idx];
        if (b != 0xFF) return b;
    } else {
        for (int i = 0; i < ctx->bank_count; i++) {
            if (ctx->bank_keys[i] == key) return i;
        }
    }
    int b = ctx->bank_count;
    if (b >= 64) return 63;
    ctx->bank_count = b + 1;
    ctx->bank_keys[b] = key;
    if (tpens == 0x7FFF) ctx->bank_7fff[pal_idx] = (uint8_t)b;

    uint32_t gen = g_pal_gen[pal_idx];
    if (s_hw_bank_key[b] != key || s_hw_bank_gen[b] != gen) {
        s_hw_bank_key[b] = key;
        s_hw_bank_gen[b] = gen;
        uint32_t *hw_pal = ta_palette_bank(TA_PALETTE_CLUT4, b);
        const uint16_t *src_pal = &g_argb1555_pal[pal_idx * 16];
        for (int p = 0; p < 16; p++) {
            hw_pal[p] = (tpens & (1 << p)) ? (uint32_t)src_pal[p] : 0u;
        }
    }
    return b;
}

static inline void hw_sq_commit_quad(
    const struct polygon_list_quad *poly,
    const struct vertex_list_quad *vert)
{
#ifdef HOST_TA_SIM
    host_ta_sim_submit_quad(poly, vert);
#else
    const uint32_t *p = (const uint32_t *)poly;
    const uint32_t *v = (const uint32_t *)vert;
    volatile uint32_t *sq0 = (volatile uint32_t *)0xE0000000u;
    volatile uint32_t *sq1 = (volatile uint32_t *)0xE0000020u;

    sq0[0] = p[0]; sq0[1] = p[1]; sq0[2] = p[2]; sq0[3] = p[3];
    sq0[4] = p[4]; sq0[5] = p[5]; sq0[6] = 0;    sq0[7] = 0;
    __asm__ __volatile__("pref @%0" : : "r"(sq0) : "memory");

    sq1[0] = v[0]; sq1[1] = v[1]; sq1[2] = v[2]; sq1[3] = v[3];
    sq1[4] = v[4]; sq1[5] = v[5]; sq1[6] = v[6]; sq1[7] = v[7];
    __asm__ __volatile__("pref @%0" : : "r"(sq1) : "memory");

    sq0[0] = v[8];  sq0[1] = v[9];  sq0[2] = v[10]; sq0[3] = v[11];
    sq0[4] = 0;     sq0[5] = v[13]; sq0[6] = v[14]; sq0[7] = v[15];
    __asm__ __volatile__("pref @%0" : : "r"(sq0) : "memory");
#endif
}

static const struct { uint32_t au_av, bu_bv, cu_cv, pad; } g_uv_lut[4] = {
    { 0x00003F80u, 0x00000000u, 0x3F800000u, 0u }, /* flipx=0, flipy=0 */
    { 0x3F803F80u, 0x3F800000u, 0x00000000u, 0u }, /* flipx=1, flipy=0 */
    { 0x00000000u, 0x00003F80u, 0x3F803F80u, 0u }, /* flipx=0, flipy=1 */
    { 0x3F800000u, 0x3F803F80u, 0x00003F80u, 0u }, /* flipx=1, flipy=1 */
};

static inline void hw_emit_quad(
    hw_ta_ctx_t *ctx,
    const void *vram_ptr,
    uint32_t uvsize_bits,
    int bank,
    int sx, int sy, int size,
    float z,
    int flipx, int flipy)
{
    struct polygon_list_quad mypoly;
    struct vertex_list_quad myvertex;

    mypoly.cmd =
        TA_CMD_SPRITE |
        TA_CMD_POLYGON_TYPE_PUNCHTHRU |
        TA_CMD_POLYGON_SUBLIST |
        TA_CMD_POLYGON_PACKED_COLOR |
        TA_CMD_POLYGON_16BIT_UV |
        TA_CMD_POLYGON_TEXTURED;
    mypoly.mode1 =
        TA_POLYMODE1_Z_GREATEREQUAL |
        TA_POLYMODE1_CULL_DISABLED |
        TA_POLYMODE1_NO_Z_UPDATE;
    mypoly.mode2 = ctx->base_mode2 | uvsize_bits;
    mypoly.texture =
        TA_TEXTUREMODE_CLUT4 |
        TA_TEXTUREMODE_CLUTBANK4(bank) |
        TA_TEXTUREMODE_ADDRESS(vram_ptr);
    mypoly.mult_color = 0xFFFFFFFFu;
    mypoly.add_color = 0u;

    float qx0 = ctx->px_lut[sx + 64];
    float qy0 = ctx->py_lut[sy + 64];
    float qx1 = ctx->px_lut[sx + 64 + size];
    float qy1 = ctx->py_lut[sy + 64 + size];

    myvertex.cmd = TA_CMD_VERTEX | TA_CMD_VERTEX_END_OF_STRIP;
    if (ctx->vert) {
        float vw = ctx->vw;
        myvertex.ax = vw - qy1; myvertex.ay = qx0; myvertex.az = z;
        myvertex.bx = vw - qy0; myvertex.by = qx0; myvertex.bz = z;
        myvertex.cx = vw - qy0; myvertex.cy = qx1; myvertex.cz = z;
        myvertex.dx = vw - qy1; myvertex.dy = qx1;
    } else {
        myvertex.ax = qx0; myvertex.ay = qy1; myvertex.az = z;
        myvertex.bx = qx0; myvertex.by = qy0; myvertex.bz = z;
        myvertex.cx = qx1; myvertex.cy = qy0; myvertex.cz = z;
        myvertex.dx = qx1; myvertex.dy = qy1;
    }

    int flip_idx = (flipx ? 1 : 0) | (flipy ? 2 : 0);
    myvertex.au_av = g_uv_lut[flip_idx].au_av;
    myvertex.bu_bv = g_uv_lut[flip_idx].bu_bv;
    myvertex.cu_cv = g_uv_lut[flip_idx].cu_cv;

    if (ctx->list_type) {
        ctx->list_type = 0;
        ta_commit_list(&mypoly, TA_LIST_SHORT);
        ta_commit_list(&myvertex, TA_LIST_LONG);
    } else {
        hw_sq_commit_quad(&mypoly, &myvertex);
    }
}

static void hw_render_scroll1(hw_ta_ctx_t *ctx, int priority, float z)
{
    int scrlxrough = (scroll1x >> 3) + 4;
    int scrlyrough = (scroll1y >> 3);
    int sx   = -(scroll1x & 0x07);
    int ytop = -(scroll1y & 0x07) + 32;
    int32_t char_base = g_cfg.bank_scroll1 * 0x08000;

    for (int x = 0; x < 0x35; x++) {
        if (sx <= 24 || sx >= 416) {
            sx += 8;
            continue;
        }
        int sy    = ytop;
        int offsx = ((scrlxrough + x) * 0x80) & 0x1FFF;

        for (int y = 0; y < 0x20; y++) {
            if (sy > 40 && sy < 272) {
                int n     = scrlyrough + y;
                int offsy = (((n & 0x1F) * 4) | ((n & 0x20) * 0x100)) & 0x3FFF;
                int offs  = (offsy + offsx) & 0x3FFF;

                int32_t raw_code = read_be16(&cps1_scroll1[offs]);
                if (raw_code != 0x20 && raw_code != g_cfg.space_scroll1) {
                    uint32_t raw_idx = (uint32_t)raw_code;
                    if (raw_idx < g_hw_char_slots) {
                        int32_t code = (int32_t)raw_idx + char_base;
                        if (code >= 0 && (uint32_t)code < g_max_char) {
                            int colour = read_be16(&cps1_scroll1[offs + 2]);
                            int transp = priority ? cps1_transparency_scroll[(colour & 0x0180) >> 7] : 0x7FFF;
                            if (transp && (g_char_pu[code] & transp)) {
                                int bank = hw_alloc_bank(ctx, 32 + (colour & 0x1F), transp);
                                hw_emit_quad(
                                    ctx,
                                    g_hw_char_vram + raw_idx * 32u,
                                    TA_POLYMODE2_U_SIZE_8 | TA_POLYMODE2_V_SIZE_8,
                                    bank, sx, sy, 8, z,
                                    colour & 0x20, colour & 0x40
                                );
                            }
                        }
                    }
                }
            }
            sy += 8;
        }
        sx += 8;
    }
}

static void hw_render_scroll2(hw_ta_ctx_t *ctx, int priority, float z)
{
    int nyoffset     = (scroll2y & 0x0F);
    int ny           = (scroll2y >> 4) - 4;
    int32_t base     = g_cfg.bank_scroll2 * 0x04000;
    int videocontrol = cps1_port(CPS1_VIDEOCONTROL);
    int otheroffs    = cps1_port(CPS1_ROWSCROLL_OFFS);

    for (int sy = 0; sy < 0x09 * 2; sy++) {
        int py = 16 * sy - nyoffset;
        if (py <= 48 - 16 || py >= 272) continue;

        int eff_scroll2x = scroll2x;
        if (videocontrol & 0x01) {
            int line = (py - 24) & 0xFF;
            eff_scroll2x += (int16_t)read_be16(&cps1_other[(2 * (line + otheroffs)) & 0x7FE]);
        }
        int nxoffset = (eff_scroll2x & 0x0F) + 32;
        int nx       = (eff_scroll2x >> 4);
        int n        = ny + sy + 2;
        int offsy    = (((n & 0x0F) * 4) | ((n & 0x30) * 0x100)) & 0x3FFF;

        for (int sx = 0; sx < 0x32 / 2 + 4; sx++) {
            int px = 16 * sx - nxoffset;
            if (px <= 32 - 16 || px >= 416) continue;

            int offsx = ((nx + sx) * 0x040) & 0x0FFF;
            int offs  = (offsy + offsx) & 0x3FFF;

            int32_t raw_code = read_be16(&cps1_scroll2[offs]);
            if (raw_code >= g_cfg.start_scroll2 && raw_code <= g_cfg.end_scroll2 &&
                !(g_cfg.kludge == 4 && raw_code >= 0x01E00 && raw_code < 0x05400))
            {
                int32_t code = (raw_code & 0x3FFF) + base;
                if (code >= 0 && (uint32_t)code < g_max_tile16) {
                    int colour = read_be16(&cps1_scroll2[offs + 2]);
                    int transp = priority ? cps1_transparency_scroll[(colour & 0x0180) >> 7] : 0x7FFF;
                    if (transp && (g_tile16_pu[code] & transp)) {
                        int bank = hw_alloc_bank(ctx, 64 + (colour & 0x1F), transp);
                        const uint8_t *vram_ptr = hw_get_tile16_vram_ptr((uint32_t)code);
                        hw_emit_quad(
                            ctx,
                            vram_ptr,
                            TA_POLYMODE2_U_SIZE_16 | TA_POLYMODE2_V_SIZE_16,
                            bank, px, py, 16, z,
                            colour & 0x20, colour & 0x40
                        );
                    }
                }
            }
        }
    }
}

static void hw_render_scroll3(hw_ta_ctx_t *ctx, int priority, float z)
{
    int nxoffset = scroll3x & 0x1F;
    int nyoffset = scroll3y & 0x1F;
    int nx = (scroll3x >> 5) + 1;
    int ny = (scroll3y >> 5) - 1;
    int32_t base = g_cfg.bank_scroll3 * 0x01000;

    for (int sx = 1; sx < 0x32 / 4 + 2; sx++) {
        for (int sy = 1; sy < 0x20 / 4 + 2; sy++) {
            int n     = ny + sy;
            int offsy = (((n & 0x07) * 4) | ((n & 0xF8) * 0x0100)) & 0x3FFF;
            int offsx = ((nx + sx) * 0x020) & 0x07FF;
            int offs  = (offsy + offsx) & 0x3FFF;

            int32_t raw_code = read_be16(&cps1_scroll3[offs]);
            if (raw_code >= g_cfg.start_scroll3 && raw_code <= g_cfg.end_scroll3) {
                uint32_t raw_idx = (uint32_t)raw_code;
                if (raw_idx < g_hw_tile32_slots) {
                    int32_t code = (int32_t)raw_idx + base;
                    if (g_cfg.kludge == 2 && code >= 0x01500)
                        code -= 0x1000;
                    if (code >= 0 && (uint32_t)code < g_max_tile32) {
                        int colour = read_be16(&cps1_scroll3[offs + 2]);
                        int transp = priority ? cps1_transparency_scroll[(colour & 0x0180) >> 7] : 0x7FFF;
                        if (transp && (g_tile32_pu[code] & transp)) {
                            int px = 32 * sx - nxoffset;
                            int py = 32 * sy - nyoffset;
                            if (px > 32 - 32 && px < 416 && py > 48 - 32 && py < 272) {
                                int bank = hw_alloc_bank(ctx, 96 + (colour & 0x1F), transp);
                                hw_emit_quad(
                                    ctx,
                                    g_hw_tile32_vram + raw_idx * 512u,
                                    TA_POLYMODE2_U_SIZE_32 | TA_POLYMODE2_V_SIZE_32,
                                    bank, px, py, 32, z,
                                    colour & 0x20, colour & 0x40
                                );
                            }
                        }
                    }
                }
            }
        }
    }
}

static void hw_render_sprites(hw_ta_ctx_t *ctx, float base_z)
{
    float z = base_z;
    for (int i = cps1_last_sprite_offset; i >= 0; i -= 8) {
        int x = read_be16(&g_buffered_obj[i]);
        int y = read_be16(&g_buffered_obj[i + 2]);
        if (x && y) {
            uint32_t code   = cps1_map_sprite_code(read_be16(&g_buffered_obj[i + 4]));
            int      colour = read_be16(&g_buffered_obj[i + 6]);
            int      col    = colour & 0x1F;

            y &= 0x1FF;
            if (y > 450) y -= 0x200;
            x &= 0x1FF;
            if (x > 450) x -= 0x200;

            x -= 0x20;
            y += 0x20;

            if (colour & 0xFF00) {
                int nx = ((colour & 0x0F00) >> 8) + 1;
                int ny = ((colour & 0xF000) >> 12) + 1;

                for (int nys = 0; nys < ny; nys++) {
                    for (int nxs = 0; nxs < nx; nxs++) {
                        int sx = x + nxs * 16;
                        int sy = y + nys * 16;
                        if (sx > 450) sx -= 0x200;
                        if (sy > 450) sy -= 0x200;
                        if (sx > 32 - 16 && sx < 416 && sy > 48 - 16 && sy < 272) {
                            int cx = (colour & 0x20) ? (nx - 1 - nxs) : nxs;
                            int cy = (colour & 0x40) ? (ny - 1 - nys) : nys;
                            uint32_t tile_code = code + (uint32_t)cx + 0x10u * (uint32_t)cy;
                            if (tile_code < g_max_tile16 && (g_tile16_pu[tile_code] & 0x7FFF)) {
                                int bank = hw_alloc_bank(ctx, col, 0x7FFF);
                                const uint8_t *vram_ptr = hw_get_tile16_vram_ptr(tile_code);
                                hw_emit_quad(
                                    ctx,
                                    vram_ptr,
                                    TA_POLYMODE2_U_SIZE_16 | TA_POLYMODE2_V_SIZE_16,
                                    bank, sx, sy, 16, z,
                                    colour & 0x20, colour & 0x40
                                );
                            }
                        }
                    }
                }
            } else {
                if (x > 32 - 16 && x < 416 && y > 48 - 16 && y < 272) {
                    if (code < g_max_tile16 && (g_tile16_pu[code] & 0x7FFF)) {
                        int bank = hw_alloc_bank(ctx, col, 0x7FFF);
                        const uint8_t *vram_ptr = hw_get_tile16_vram_ptr(code);
                        hw_emit_quad(
                            ctx,
                            vram_ptr,
                            TA_POLYMODE2_U_SIZE_16 | TA_POLYMODE2_V_SIZE_16,
                            bank, x, y, 16, z,
                            colour & 0x20, colour & 0x40
                        );
                    }
                }
            }
            z += 0.0002f;
        }
    }
}

static void hw_render_layer(hw_ta_ctx_t *ctx, int layer, float z)
{
    if (!cps1_layer_enabled[layer]) return;
    int pt = g_prof ? profile_start() : -1;
    switch (layer) {
        case 0: hw_render_sprites(ctx, z);   break;
        case 1: hw_render_scroll1(ctx, 0, z); break;
        case 2: hw_render_scroll2(ctx, 0, z); break;
        case 3: hw_render_scroll3(ctx, 0, z); break;
    }
    if (pt >= 0) {
        uint32_t dt = profile_end(pt);
        if (layer == 0) g_prof->gfx_spr_us += dt;
        else if (layer == 1) g_prof->gfx_s1_us += dt;
        else if (layer == 2) g_prof->gfx_s2_us += dt;
        else if (layer == 3) g_prof->gfx_s3_us += dt;
    }
}

static void hw_render_high_layer(hw_ta_ctx_t *ctx, int layer, float z)
{
    if (!cps1_layer_enabled[layer]) return;
    switch (layer) {
        case 1: hw_render_scroll1(ctx, 1, z); break;
        case 2: hw_render_scroll2(ctx, 1, z); break;
        case 3: hw_render_scroll3(ctx, 1, z); break;
    }
}

void cps1_hw_render_ta(float x0, float y0, float x1, float y1, int bilinear)
{
    int pg = g_prof ? profile_start() : -1;
    if (g_prof) {
        g_prof->gfx_clr_us = 0;
        g_prof->gfx_s1_us  = 0;
        g_prof->gfx_s2_us  = 0;
        g_prof->gfx_s3_us  = 0;
        g_prof->gfx_spr_us = 0;
    }

    cps1_find_last_sprite();
    cps1_build_palette();

    uint16_t bg1555 = g_argb1555_pal[0x0F];
    ta_set_background_color(rgb(
        ((bg1555 >> 10) & 0x1F) << 3,
        ((bg1555 >>  5) & 0x1F) << 3,
        ( bg1555        & 0x1F) << 3
    ));

    hw_ta_ctx_t ctx;
    ctx.scale_x = (x1 - x0) / 384.0f;
    ctx.scale_y = (y1 - y0) / 224.0f;
    ctx.off_x   = x0 - 32.0f * ctx.scale_x;
    ctx.off_y   = y0 - 48.0f * ctx.scale_y;
    ctx.vert    = video_is_vertical();
    ctx.vw      = (float)video_width() - 1.0f;
    {
        float cur_x = ctx.off_x - 64.0f * ctx.scale_x;
        for (int i = 0; i < 640; i++) {
            ctx.px_lut[i] = cur_x;
            cur_x += ctx.scale_x;
        }
        float cur_y = ctx.off_y - 64.0f * ctx.scale_y;
        for (int i = 0; i < 512; i++) {
            ctx.py_lut[i] = cur_y;
            cur_y += ctx.scale_y;
        }
    }
    ctx.base_mode2 =
        TA_POLYMODE2_MIPMAP_D_1_00 |
        TA_POLYMODE2_TEXTURE_DECAL |
        TA_POLYMODE2_TEXTURE_CLAMP_U |
        TA_POLYMODE2_TEXTURE_CLAMP_V |
        TA_POLYMODE2_FOG_DISABLED |
        TA_POLYMODE2_ENABLE_ALPHA |
        TA_POLYMODE2_SRC_BLEND_SRC_ALPHA |
        TA_POLYMODE2_DST_BLEND_INV_SRC_ALPHA |
        (bilinear ? (TA_POLYMODE2_BILINEAR_FILTER | TA_POLYMODE2_ENABLE_FILTER) : 0);
    memset(ctx.bank_7fff, 0xFF, sizeof(ctx.bank_7fff));
    ctx.bank_count = 0;
    ctx.list_type = TA_CMD_POLYGON_TYPE_PUNCHTHRU;

    int layercontrol = cps1_port(g_cfg.layer_control);
    int l0 = (layercontrol >> 0x06) & 3;
    int l1 = (layercontrol >> 0x08) & 3;
    int l2 = (layercontrol >> 0x0A) & 3;
    int l3 = (layercontrol >> 0x0C) & 3;

    hw_render_layer(&ctx, l0, 1.0f);
    hw_render_layer(&ctx, l1, 2.0f);
    if (l1 == 0) hw_render_high_layer(&ctx, l0, 2.5f);
    hw_render_layer(&ctx, l2, 3.0f);
    if (l2 == 0) hw_render_high_layer(&ctx, l1, 3.5f);
    hw_render_layer(&ctx, l3, 4.0f);
    if (l3 == 0) hw_render_high_layer(&ctx, l2, 4.5f);

    if (pg >= 0) {
        g_prof->gfx_us = profile_end(pg);
    }
}
#endif

/* ======================================================================== */
/*                  DEFAULT CONFIG & PLANAR GFX DECODER                     */
/* ======================================================================== */

static void cps1_set_default_ffight_config(cps1_game_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->game_name,  "ffight");
    strcpy(cfg->game_title, "Final Fight (World)");
    cfg->cpsb_addr         = 0x60;
    cfg->cpsb_value        = 0x0004;
    cfg->mult_factor1      = 0x00;
    cfg->mult_factor2      = 0x00;
    cfg->mult_result_lo    = 0x00;
    cfg->mult_result_hi    = 0x00;
    cfg->layer_control     = 0x6e;
    cfg->priority0         = 0x66;
    cfg->priority1         = 0x70;
    cfg->priority2         = 0x68;
    cfg->priority3         = 0x72;
    cfg->control_reg       = 0x6a;
    cfg->scrl1_enable_mask = 0x02;
    cfg->scrl2_enable_mask = 0x0c;
    cfg->scrl3_enable_mask = 0x0c;
    cfg->bank_scroll1      = 0;
    cfg->bank_scroll2      = 0;
    cfg->bank_scroll3      = 0;
    cfg->space_scroll1     = 0x0020;
    cfg->start_scroll2     = 0x0001;
    cfg->end_scroll2       = 0xFFFF;
    cfg->start_scroll3     = 0x0001;
    cfg->end_scroll3       = 0xFFFF;
    cfg->kludge            = 0;
    cfg->m68k_hz           = 10000000;
    cfg->oki_hz            = 7576;
    cfg->input_type        = CPS1_INPUT_STD;
    cfg->orientation       = 0;
    cfg->dswa              = 0xFF;
    cfg->dswb              = 0xF4;
    cfg->dswc              = 0x9F;
    cfg->flags             = CPS1_FLAG_FFIGHT_IDLE;
}

static uint32_t g_m68k_idle_start = 0u;
static uint32_t g_m68k_idle_end   = 0u;

/*
 * Scans 68000 ROM for Capcom's cooperative 16-slot task scheduler wait loop:
 *   move.w #$2600, sr ... move.w #$2000, sr; lea disp(a0), a0; dbra d0, ...
 * Skipping the remaining frame slices once the 68000 reaches this idle spin
 * avoids burning 70-85% of SH-4 CPU on VBlank busy-waits across all CPS-1 titles.
 */
static void cps1_detect_m68k_idle_loop(void)
{
    g_m68k_idle_start = 0u;
    g_m68k_idle_end   = 0u;
    if (!g_m68k_rom || g_m68k_rom_size < 0x0800u)
        return;

    uint32_t limit = g_m68k_rom_size;
    if (limit > 0x060000u) limit = 0x060000u;
    uint32_t words = limit >> 1;
    const uint16_t *rom16 = (const uint16_t *)g_m68k_rom;

    for (uint32_t i = 0x0100u; i + 28u < words; i++) {
        if (rom16[i] == 0x46FCu && rom16[i + 1] == 0x2600u) {
            for (uint32_t j = 2u; j <= 22u; j++) {
                if (rom16[i + j] == 0x46FCu && rom16[i + j + 1] == 0x2000u &&
                    (rom16[i + j + 2] & 0xFFFEu) == 0x41E8u &&
                    rom16[i + j + 4] == 0x51C8u)
                {
                    uint32_t byte_i = i * 2u;
                    g_m68k_idle_start = (byte_i >= 12u) ? (byte_i - 12u) : byte_i;
                    g_m68k_idle_end   = (i + j + 9u) * 2u;
                    return;
                }
            }
        }
    }
}

static void cps1_detect_z80_idle_loop(void)
{
    g_z80_idle_pc  = 0xFFFFu;
    g_z80_idle_ppc = 0xFFFFu;
    if (!g_z80_rom || g_z80_rom_size < 0x0200u)
        return;

    for (uint32_t i = 0x0040; i < 0x0160; i++) {
        if (g_z80_rom[i + 0] == 0x3A && g_z80_rom[i + 1] == 0x01 && g_z80_rom[i + 2] == 0xD0 &&
            g_z80_rom[i + 3] == 0x47 &&
            g_z80_rom[i + 4] == 0x3A && g_z80_rom[i + 5] == 0x00 && g_z80_rom[i + 6] == 0xD0 &&
            g_z80_rom[i + 7] == 0xB8 && g_z80_rom[i + 8] == 0xC4)
        {
            for (uint32_t j = i + 16; j < i + 36; j++) {
                if (g_z80_rom[j] == 0xBE && g_z80_rom[j + 1] == 0x28) {
                    int8_t rel = (int8_t)g_z80_rom[j + 2];
                    uint32_t target = (uint32_t)((int32_t)(j + 3) + rel);
                    if (target == i) {
                        g_z80_idle_pc  = (uint16_t)i;
                        g_z80_idle_ppc = (uint16_t)(j + 1);
                        return;
                    }
                }
            }
        }
    }
}

/* Fast LUT-based planar-to-packed 4bpp decoder (bit-for-bit identical to pack_cps1.py) */
static void cps1_build_planar_luts(uint32_t byte_to_dw[256], uint16_t byte_pu[256])
{
    for (int b = 0; b < 256; b++) {
        uint32_t val = 0;
        for (int j = 0; j < 8; j++) {
            if (b & (0x80 >> j))
                val |= 1u << (28 - j * 4);
        }
        byte_to_dw[b] = val;
        byte_pu[b] = (uint16_t)((1u << (b >> 4)) | (1u << (b & 0x0F)));
    }
}

static void cps1_decode_planar_bank(
    const uint8_t  *bank_buf,
    uint32_t        bank_len,
    uint32_t        bank_off,
    const uint32_t  byte_to_dw[256],
    const uint16_t  byte_pu[256])
{
    uint32_t quarter   = bank_len >> 2;
    uint32_t num_pairs = quarter >> 1;
    uint32_t base_pair = bank_off >> 3;

    const uint8_t *b_1m = bank_buf;
    const uint8_t *b_5m = bank_buf + quarter;
    const uint8_t *b_3m = bank_buf + quarter * 2;
    const uint8_t *b_7m = bank_buf + quarter * 3;

    for (uint32_t i = 0; i < num_pairs; i++) {
        uint32_t off   = i * 2;
        uint32_t p_idx = base_pair + i;
        uint32_t nchar = p_idx >> 3;

        uint32_t dw0 = byte_to_dw[b_5m[off]]
                     | (byte_to_dw[b_5m[off + 1]] << 1)
                     | (byte_to_dw[b_7m[off]] << 2)
                     | (byte_to_dw[b_7m[off + 1]] << 3);
        uint32_t dw1 = byte_to_dw[b_1m[off]]
                     | (byte_to_dw[b_1m[off + 1]] << 1)
                     | (byte_to_dw[b_3m[off]] << 2)
                     | (byte_to_dw[b_3m[off + 1]] << 3);

        g_cps1_gfx[2 * p_idx]     = dw0;
        g_cps1_gfx[2 * p_idx + 1] = dw1;

        uint16_t pu = byte_pu[dw0 & 0xFF] | byte_pu[(dw0 >> 8) & 0xFF]
                    | byte_pu[(dw0 >> 16) & 0xFF] | byte_pu[dw0 >> 24]
                    | byte_pu[dw1 & 0xFF] | byte_pu[(dw1 >> 8) & 0xFF]
                    | byte_pu[(dw1 >> 16) & 0xFF] | byte_pu[dw1 >> 24];

        g_char_pu[nchar]       |= pu;
        g_tile16_pu[nchar >> 1] |= pu;
        g_tile32_pu[nchar >> 3] |= pu;
    }
}

/* ======================================================================== */
/*                       PUBLIC EMULATOR INTERFACE                          */
/* ======================================================================== */

int cps1_emu_init(const char *cps1_rom_path)
{
    FILE *fp = fopen(cps1_rom_path, "rb");
    if (!fp) {
        return -1;
    }

    char magic[4];
    if (fread(magic, 1, 4, fp) != 4) {
        fclose(fp);
        return -2;
    }
    fseek(fp, 0, SEEK_SET);

    if (memcmp(magic, "CPS1", 4) == 0) {
        /* V1 pre-decoded Final Fight bundle */
        cps1_bundle_hdr_v1_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
            fclose(fp);
            return -2;
        }
        cps1_set_default_ffight_config(&g_cfg);

        g_m68k_rom_size = hdr.m68k_size;
        g_z80_rom_size  = hdr.z80_size;
        g_oki_rom_size  = hdr.oki_size;
        g_max_char      = hdr.num_char_pu;
        g_max_tile16    = hdr.num_tile16_pu;
        g_max_tile32    = hdr.num_tile32_pu;
        uint32_t gfx_words = hdr.gfx_bytes / 4u;

        if (!g_m68k_rom) g_m68k_rom = (uint8_t *)malloc(M68K_MAX_ROM_SIZE);
        if (!g_z80_rom)  g_z80_rom  = (uint8_t *)malloc(Z80_MAX_ROM_SIZE);
        if (g_oki_rom)   { free(g_oki_rom);   g_oki_rom   = NULL; }
        if (g_cps1_gfx)  { free(g_cps1_gfx);  g_cps1_gfx  = NULL; }
        if (g_char_pu)   { free(g_char_pu);   g_char_pu   = NULL; }
        if (g_tile16_pu) { free(g_tile16_pu); g_tile16_pu = NULL; }
        if (g_tile32_pu) { free(g_tile32_pu); g_tile32_pu = NULL; }

        g_oki_rom   = (uint8_t  *)calloc(1, g_oki_rom_size ? g_oki_rom_size : 0x40000u);
        g_cps1_gfx  = (uint32_t *)malloc(gfx_words * sizeof(uint32_t));
        g_char_pu   = (uint16_t *)malloc(g_max_char   * sizeof(uint16_t));
        g_tile16_pu = (uint16_t *)malloc(g_max_tile16 * sizeof(uint16_t));
        g_tile32_pu = (uint16_t *)malloc(g_max_tile32 * sizeof(uint16_t));

        if (!g_m68k_rom || !g_cps1_gfx || !g_char_pu || !g_tile16_pu ||
            !g_tile32_pu || !g_z80_rom || !g_oki_rom) {
            fclose(fp);
            return -3;
        }
        memset(g_m68k_rom, 0, M68K_MAX_ROM_SIZE);
        memset(g_z80_rom,  0, Z80_MAX_ROM_SIZE);

        size_t r = 0;
        r += fread(g_m68k_rom,  1, g_m68k_rom_size, fp);
        r += fread(g_cps1_gfx,  sizeof(uint32_t), gfx_words, fp);
        r += fread(g_char_pu,   sizeof(uint16_t), g_max_char, fp);
        r += fread(g_tile16_pu, sizeof(uint16_t), g_max_tile16, fp);
        r += fread(g_tile32_pu, sizeof(uint16_t), g_max_tile32, fp);
        r += fread(g_z80_rom,   1, g_z80_rom_size, fp);
        r += fread(g_oki_rom,   1, g_oki_rom_size, fp);
        (void)r;
        fclose(fp);
    } else if (memcmp(magic, "CPSM", 4) == 0) {
        /* V2 multi-game bundle with embedded config + raw planar GFX */
        cps1_bundle_hdr_v2_t hdr2;
        if (fread(&hdr2, sizeof(hdr2), 1, fp) != 1 || hdr2.version != 2) {
            fclose(fp);
            return -2;
        }
        g_cfg           = hdr2.cfg;
        g_m68k_rom_size = hdr2.m68k_size;
        g_z80_rom_size  = hdr2.z80_size;
        g_oki_rom_size  = hdr2.oki_size;
        uint32_t raw_gfx_size = hdr2.gfx_size;

        g_max_char   = raw_gfx_size / 32u;
        g_max_tile16 = g_max_char / 2u;
        g_max_tile32 = g_max_char / 8u;
        uint32_t gfx_words = raw_gfx_size / 2u;

        if (!g_m68k_rom) g_m68k_rom = (uint8_t *)malloc(M68K_MAX_ROM_SIZE);
        if (!g_z80_rom)  g_z80_rom  = (uint8_t *)malloc(Z80_MAX_ROM_SIZE);
        if (g_oki_rom)   { free(g_oki_rom);   g_oki_rom   = NULL; }
        if (g_cps1_gfx)  { free(g_cps1_gfx);  g_cps1_gfx  = NULL; }
        if (g_char_pu)   { free(g_char_pu);   g_char_pu   = NULL; }
        if (g_tile16_pu) { free(g_tile16_pu); g_tile16_pu = NULL; }
        if (g_tile32_pu) { free(g_tile32_pu); g_tile32_pu = NULL; }

        g_oki_rom   = (uint8_t  *)calloc(1, g_oki_rom_size ? g_oki_rom_size : 0x40000u);
        g_cps1_gfx  = (uint32_t *)malloc(gfx_words * sizeof(uint32_t));
        g_char_pu   = (uint16_t *)calloc(g_max_char,   sizeof(uint16_t));
        g_tile16_pu = (uint16_t *)calloc(g_max_tile16, sizeof(uint16_t));
        g_tile32_pu = (uint16_t *)calloc(g_max_tile32, sizeof(uint16_t));

        uint8_t *bank_buf = (uint8_t *)malloc(0x200000u);
        if (!g_m68k_rom || !g_cps1_gfx || !g_char_pu || !g_tile16_pu ||
            !g_tile32_pu || !g_z80_rom || !g_oki_rom || !bank_buf) {
            if (bank_buf) free(bank_buf);
            fclose(fp);
            return -3;
        }
        memset(g_m68k_rom, 0, M68K_MAX_ROM_SIZE);
        memset(g_z80_rom,  0, Z80_MAX_ROM_SIZE);

        size_t r = 0;
        r += fread(g_m68k_rom, 1, g_m68k_rom_size, fp);
        r += fread(g_z80_rom,  1, g_z80_rom_size,  fp);
        if (g_oki_rom_size > 0)
            r += fread(g_oki_rom, 1, g_oki_rom_size, fp);
        (void)r;

        uint32_t byte_to_dw[256];
        uint16_t byte_pu[256];
        cps1_build_planar_luts(byte_to_dw, byte_pu);

        for (uint32_t bank_off = 0; bank_off < raw_gfx_size; bank_off += 0x200000u) {
            uint32_t bank_len = (raw_gfx_size - bank_off < 0x200000u)
                              ? (raw_gfx_size - bank_off) : 0x200000u;
            if (fread(bank_buf, 1, bank_len, fp) != bank_len) {
                free(bank_buf);
                fclose(fp);
                return -4;
            }
            cps1_decode_planar_bank(bank_buf, bank_len, bank_off, byte_to_dw, byte_pu);
        }
        free(bank_buf);
        fclose(fp);
    } else {
        fclose(fp);
        return -2;
    }

    /* Pre-byteswap 68000 ROM into 16-bit host-endian words for zero-overhead SH-4 instruction fetch */
    uint16_t *rom16 = (uint16_t *)g_m68k_rom;
    for (uint32_t i = 0; i < g_m68k_rom_size / 2; i++) {
        uint16_t w = rom16[i];
        rom16[i] = (uint16_t)((w >> 8) | (w << 8));
    }

    if (!g_ym2151_inited) {
        YM2151Init(1, 3579580, CPS1_AUDIO_RATE);
        YM2151SetIrqHandler(0, cps1_ym2151_irq_handler);
        YM2151SetPortWriteHandler(0, cps1_oki_bankswitch);
        g_ym2151_inited = 1;
    }

    cps1_emu_reset();
    return 0;
}

int cps1_emu_init_mem(uint32_t cart_offset, void (*progress_cb)(int pct))
{
#ifdef __sh__
    char magic[4];
    cart_read(magic, cart_offset, 4);
    if (memcmp(magic, "CPSM", 4) != 0)
        return -2;

    cps1_bundle_hdr_v2_t hdr2;
    cart_read(&hdr2, cart_offset, sizeof(hdr2));
    if (hdr2.version != 2 || hdr2.m68k_size == 0 || hdr2.m68k_size > M68K_MAX_ROM_SIZE ||
        hdr2.gfx_size == 0 || hdr2.gfx_size > 0x800000u)
    {
        return -2;
    }

    if (progress_cb) progress_cb(10);

    g_cfg           = hdr2.cfg;
    g_m68k_rom_size = hdr2.m68k_size;
    g_z80_rom_size  = hdr2.z80_size;
    g_oki_rom_size  = hdr2.oki_size;
    uint32_t raw_gfx_size = hdr2.gfx_size;

    g_max_char   = raw_gfx_size / 32u;
    g_max_tile16 = g_max_char / 2u;
    g_max_tile32 = g_max_char / 8u;
    uint32_t gfx_words = raw_gfx_size / 2u;

    if (!g_m68k_rom) g_m68k_rom = (uint8_t *)malloc(M68K_MAX_ROM_SIZE);
    if (!g_z80_rom)  g_z80_rom  = (uint8_t *)malloc(Z80_MAX_ROM_SIZE);
    if (g_oki_rom)   { free(g_oki_rom);   g_oki_rom   = NULL; }
    if (g_cps1_gfx)  { free(g_cps1_gfx);  g_cps1_gfx  = NULL; }
    if (g_char_pu)   { free(g_char_pu);   g_char_pu   = NULL; }
    if (g_tile16_pu) { free(g_tile16_pu); g_tile16_pu = NULL; }
    if (g_tile32_pu) { free(g_tile32_pu); g_tile32_pu = NULL; }

    g_oki_rom   = (uint8_t  *)calloc(1, g_oki_rom_size ? g_oki_rom_size : 0x40000u);
    g_cps1_gfx  = (uint32_t *)malloc(gfx_words * sizeof(uint32_t));
    g_char_pu   = (uint16_t *)calloc(g_max_char,   sizeof(uint16_t));
    g_tile16_pu = (uint16_t *)calloc(g_max_tile16, sizeof(uint16_t));
    g_tile32_pu = (uint16_t *)calloc(g_max_tile32, sizeof(uint16_t));

    uint8_t *bank_buf = (uint8_t *)malloc(0x200000u);
    if (!g_m68k_rom || !g_cps1_gfx || !g_char_pu || !g_tile16_pu ||
        !g_tile32_pu || !g_z80_rom || !g_oki_rom || !bank_buf) {
        if (bank_buf) free(bank_buf);
        return -3;
    }
    memset(g_m68k_rom, 0, M68K_MAX_ROM_SIZE);
    memset(g_z80_rom,  0, Z80_MAX_ROM_SIZE);

    uint32_t cur_off = cart_offset + sizeof(hdr2);
    cart_read(g_m68k_rom, cur_off, g_m68k_rom_size); cur_off += g_m68k_rom_size;
    cart_read(g_z80_rom,  cur_off, g_z80_rom_size);  cur_off += g_z80_rom_size;
    if (g_oki_rom_size > 0) {
        cart_read(g_oki_rom, cur_off, g_oki_rom_size);
        cur_off += g_oki_rom_size;
    }
    if (progress_cb) progress_cb(25);

    uint32_t byte_to_dw[256];
    uint16_t byte_pu[256];
    cps1_build_planar_luts(byte_to_dw, byte_pu);

    for (uint32_t bank_off = 0; bank_off < raw_gfx_size; bank_off += 0x200000u) {
        uint32_t bank_len = (raw_gfx_size - bank_off < 0x200000u)
                          ? (raw_gfx_size - bank_off) : 0x200000u;
        cart_read(bank_buf, cur_off + bank_off, bank_len);
        cps1_decode_planar_bank(bank_buf, bank_len, bank_off, byte_to_dw, byte_pu);
        if (progress_cb) {
            int pct = 25 + (int)(((uint64_t)(bank_off + bank_len) * 60u) / raw_gfx_size);
            progress_cb(pct);
        }
    }
    free(bank_buf);

    uint16_t *rom16 = (uint16_t *)g_m68k_rom;
    for (uint32_t i = 0; i < g_m68k_rom_size / 2; i++) {
        uint16_t w = rom16[i];
        rom16[i] = (uint16_t)((w >> 8) | (w << 8));
    }

    if (!g_ym2151_inited) {
        YM2151Init(1, 3579580, CPS1_AUDIO_RATE);
        YM2151SetIrqHandler(0, cps1_ym2151_irq_handler);
        YM2151SetPortWriteHandler(0, cps1_oki_bankswitch);
        g_ym2151_inited = 1;
    }

    cps1_emu_reset();
    if (progress_cb) progress_cb(100);
    return 0;
#else
    (void)cart_offset;
    (void)progress_cb;
    return -1;
#endif
}

void cps1_emu_reset(void)
{
    memset(g_gfxram,       0, sizeof(g_gfxram));
    memset(g_mainram,      0, sizeof(g_mainram));
    memset(g_output,       0, sizeof(g_output));
    memset(g_buffered_obj, 0, sizeof(g_buffered_obj));
    memset(g_z80_ram,      0, sizeof(g_z80_ram));

    g_pal_init = 0;
#ifdef __sh__
    memset(s_hw_bank_key, 0, sizeof(s_hw_bank_key));
    memset(s_hw_bank_gen, 0, sizeof(s_hw_bank_gen));
#endif

    /* Default CPS-A base register values */
    write_be16(&g_output[0x00], 0x9200);
    write_be16(&g_output[0x02], 0x9000);
    write_be16(&g_output[0x04], 0x9040);
    write_be16(&g_output[0x06], 0x9080);
    write_be16(&g_output[0x08], 0x9100);
    write_be16(&g_output[0x0A], 0x90C0);

    g_soundlatch       = 0;
    g_sound_fade_timer = 0;
    g_z80_bank         = 0;
    g_ym2151_reg       = 0;
    g_in_p1            = 0xFF;
    g_in_p2            = 0xFF;
    g_in_p3            = 0xFFFF;
    g_in_sys           = 0xFF;
    g_in_dswa          = g_cfg.dswa ? g_cfg.dswa : 0xFF;
    g_in_dswb          = g_cfg.dswb ? g_cfg.dswb : 0xF4;
    g_in_dswc          = g_cfg.dswc ? g_cfg.dswc : 0x9F;
    g_dial_val[0]      = 0;
    g_dial_val[1]      = 0;

    eeprom_init();
    cps1_detect_m68k_idle_loop();
    cps1_detect_z80_idle_loop();

    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_pulse_reset();

    z80_reset(NULL);
    z80_set_irq_callback(cps1_z80_irq_ack);

    YM2151ResetChip(0);
    oki6295_init();
}

void cps1_emu_frame(
    const cps1_inputs_t *inp,
    uint16_t *vis_fb_out,
    int16_t  *audio_lr_out,
    int       num_audio_samples
)
{
    if (inp) {
        g_in_p1   = (uint8_t)(~inp->p1 & 0xFF);
        g_in_p2   = (uint8_t)(~inp->p2 & 0xFF);
        uint16_t p3_hi = ((uint16_t)(inp->p1_extra & 0x0F)) | (((uint16_t)(inp->p2_extra & 0x0F)) << 4);
        g_in_p3   = (uint16_t)(~(p3_hi | (p3_hi << 8)));

        if (g_cfg.input_type == CPS1_INPUT_STRIDER) {
            uint8_t s = inp->sys & 0xF0;
            if (inp->sys & CPS1_SYS_SERVICE) s |= 0x01;
            if (inp->sys & CPS1_SYS_COIN1)   s |= 0x04;
            if (inp->sys & CPS1_SYS_COIN2)   s |= 0x08;
            g_in_sys = (uint8_t)(~s & 0xFF);
        } else {
            g_in_sys = (uint8_t)(~inp->sys & 0xFF);
        }
        g_in_dswa = inp->dswa ? inp->dswa : (g_cfg.dswa ? g_cfg.dswa : 0xFF);
        g_in_dswb = inp->dswb ? inp->dswb : (g_cfg.dswb ? g_cfg.dswb : 0xF4);
        g_in_dswc = inp->dswc ? inp->dswc : (g_cfg.dswc ? g_cfg.dswc : 0x9F);
    }

    /*
     * Interleave 68000 (10/12 MHz) and Z80 (3.58 MHz) across 16 sub-slices per frame
     * so soundlatch writes and YM2151 Timer A/B IRQs are serviced with sub-millisecond accuracy.
     */
    const int slices = 16;
    const uint32_t m68k_hz = (g_cfg.m68k_hz >= 10000000u && g_cfg.m68k_hz <= 16000000u)
                           ? g_cfg.m68k_hz : 10000000u;
    const int m68k_cyc_per_slice = (int)((m68k_hz / 60u) / slices);
    const int z80_cyc_per_slice  = (3579545  / 60) / slices;
    const uint32_t idle_start = g_m68k_idle_end
                              ? g_m68k_idle_start
                              : ((g_cfg.flags & CPS1_FLAG_FFIGHT_IDLE) ? 0x00075Au : 0u);
    const uint32_t idle_end   = g_m68k_idle_end
                              ? g_m68k_idle_end
                              : ((g_cfg.flags & CPS1_FLAG_FFIGHT_IDLE) ? 0x00076Eu : 0u);

    int16_t ym_l[512], ym_r[512];
    int16_t oki_v[OKI_VOICES][512];

#ifdef __sh__
    uint32_t t_m68k = 0, t_z80 = 0, t_ym = 0, t_gfx = 0, t_oki = 0;
#endif

    int samples_done = 0;
    int m68k_idle = 0;
    int idle_hits = 0;
    for (int s = 0; s < slices; s++) {
#ifdef __sh__
        if (!m68k_idle) {
            int pt = g_prof ? profile_start() : -1;
            m68k_execute(m68k_cyc_per_slice);
            if (pt >= 0) t_m68k += profile_end(pt);
            if (idle_end) {
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
                if (pc >= idle_start && pc <= idle_end) {
                    if (++idle_hits >= 2) m68k_idle = 1;
                } else {
                    idle_hits = 0;
                }
            }
        }

        int pt = g_prof ? profile_start() : -1;
        z80_execute(z80_cyc_per_slice);
        if (pt >= 0) t_z80 += profile_end(pt);
#else
        if (!m68k_idle) {
            m68k_execute(m68k_cyc_per_slice);
            if (idle_end) {
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
                if (pc >= idle_start && pc <= idle_end) {
                    if (++idle_hits >= 2) m68k_idle = 1;
                } else {
                    idle_hits = 0;
                }
            }
        }
        z80_execute(z80_cyc_per_slice);
#endif

        int target_samples = ((s + 1) * num_audio_samples) / slices;
        int chunk = target_samples - samples_done;
        if (chunk > 0 && audio_lr_out) {
            int16_t *ym_bufs[2] = { &ym_l[samples_done], &ym_r[samples_done] };
#ifdef __sh__
            int py = g_prof ? profile_start() : -1;
            YM2151UpdateOne(0, ym_bufs, chunk);
            if (py >= 0) t_ym += profile_end(py);
#else
            YM2151UpdateOne(0, ym_bufs, chunk);
#endif
            samples_done = target_samples;
        }
    }

    /* Trigger CPS-1 VBlank IRQ (Level 2 autovector) */
    m68k_set_irq(2);

    /* Render CPS-1 video layers into vis_fb_out (or just latch sprites if using TA hardware renderer) */
    if (vis_fb_out) {
#ifdef __sh__
        int pg = g_prof ? profile_start() : -1;
        cps1_render_frame(vis_fb_out);
        if (pg >= 0) t_gfx = profile_end(pg);
#else
        cps1_render_frame(vis_fb_out);
#endif
    } else {
        cps1_get_video_base();
        memcpy(g_buffered_obj, cps1_obj, CPS1_OBJ_SIZE);
    }

    /* Mix YM2151 stereo FM + 4x OKIM6295 ADPCM voices into interleaved L/R 16-bit PCM */
    if (audio_lr_out && num_audio_samples > 0 && num_audio_samples <= 512) {
#ifdef __sh__
        int po = g_prof ? profile_start() : -1;
#endif
        for (int v = 0; v < OKI_VOICES; v++) {
            oki6295_update_voice(&g_oki_voices[v], oki_v[v], num_audio_samples);
        }
        for (int i = 0; i < num_audio_samples; i++) {
            int32_t oki_sum = (int32_t)oki_v[0][i] + oki_v[1][i] + oki_v[2][i] + oki_v[3][i];
            int32_t l = (int32_t)ym_l[i] + (oki_sum >> 1);
            int32_t r = (int32_t)ym_r[i] + (oki_sum >> 1);
            if (l >  32767) l =  32767;
            if (l < -32768) l = -32768;
            if (r >  32767) r =  32767;
            if (r < -32768) r = -32768;
            audio_lr_out[i * 2 + 0] = (int16_t)l;
            audio_lr_out[i * 2 + 1] = (int16_t)r;
        }
#ifdef __sh__
        if (po >= 0) t_oki = profile_end(po);
#endif
    }

#ifdef __sh__
    if (g_prof) {
        g_prof->m68k_us   = t_m68k;
        g_prof->z80_us    = t_z80;
        g_prof->ym2151_us = t_ym;
        g_prof->oki_us    = t_oki;
        g_prof->gfx_us    = t_gfx;
    }
#endif
}
