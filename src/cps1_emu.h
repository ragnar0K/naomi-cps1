#ifndef CPS1_EMU_H
#define CPS1_EMU_H

#include <stdint.h>

/* CPS-1 Visible Screen Resolution */
#define CPS1_VIS_WIDTH   384
#define CPS1_VIS_HEIGHT  224

/* CPS-1 Full Tilemap Render Canvas (448 stride avoids SH-4 16KB D-cache aliasing) */
#define CPS1_BUF_WIDTH   448
#define CPS1_BUF_HEIGHT  320
#define CPS1_BUF_STRIDE  448

/* Audio Output Rate (11025 Hz = 44100/4 has 0.00 Hz drift on NAOMI AICA and halves YM2151 CPU load) */
#define CPS1_AUDIO_RATE  11025

/* Player Input Bits (Active-High in cps1_inputs_t, converted to Active-Low for CPS-1) */
#define CPS1_INP_RIGHT   0x01
#define CPS1_INP_LEFT    0x02
#define CPS1_INP_DOWN    0x04
#define CPS1_INP_UP      0x08
#define CPS1_INP_BTN1    0x10  /* Attack / Light Punch  */
#define CPS1_INP_BTN2    0x20  /* Jump   / Medium Punch */
#define CPS1_INP_BTN3    0x40  /* Special / Heavy Punch */

/* System Input Bits (IN0) */
#define CPS1_SYS_COIN1   0x01
#define CPS1_SYS_COIN2   0x02
#define CPS1_SYS_SERVICE 0x04
#define CPS1_SYS_START1  0x10
#define CPS1_SYS_START2  0x20

/* CPS-1 Input Type IDs */
#define CPS1_INPUT_STD      0   /* Standard 2/3-button CPS-1 */
#define CPS1_INPUT_SF2      1   /* Street Fighter II 6-button (extra kicks at 0x176/0x1FC) */
#define CPS1_INPUT_STRIDER  2   /* Strider (Coin 1/2 on bits 0x04/0x08) */
#define CPS1_INPUT_FORGOTTN 3   /* Forgotten Worlds (analog dial at 0x52..0x5D) */

/* CPS-1 Per-Game Flags */
#define CPS1_FLAG_FFIGHT_IDLE  0x01  /* Enable Final Fight 68K 0x075A..0x076E wait loop skip */

/*
 * Per-game CPS-1 hardware configuration embedded in the V2 .cps1 bundle header
 * (168 bytes; total V2 bundle header is 192 bytes).
 */
typedef struct {
    char     game_name[16];     /* Short MAME set name, e.g. "sf2ja" */
    char     game_title[48];    /* Human-readable title */

    /* CPS-B custom chip register map (byte offsets from 0x800100) */
    uint16_t cpsb_addr;         /* CPS-B ID register offset (0 = none) */
    uint16_t cpsb_value;        /* CPS-B ID value returned when read */
    uint16_t mult_factor1;      /* Multiply factor 1 offset (0 = none) */
    uint16_t mult_factor2;      /* Multiply factor 2 offset */
    uint16_t mult_result_lo;    /* Multiply result low word offset */
    uint16_t mult_result_hi;    /* Multiply result high word offset */
    uint16_t layer_control;     /* Layer control register offset */
    uint16_t priority0;         /* Priority mask 0 offset */
    uint16_t priority1;         /* Priority mask 1 offset */
    uint16_t priority2;         /* Priority mask 2 offset */
    uint16_t priority3;         /* Priority mask 3 offset */
    uint16_t control_reg;       /* Layer enable control reg offset */
    uint16_t scrl1_enable_mask; /* Bitmask in layer_control for Scroll 1 */
    uint16_t scrl2_enable_mask; /* Bitmask in layer_control for Scroll 2 */
    uint16_t scrl3_enable_mask; /* Bitmask in layer_control for Scroll 3 */
    uint16_t pad0;

    /* CPS-1 Gfx bank & range configuration (from MAME CPS1gfxConfig) */
    int32_t  bank_scroll1;      /* Scroll 1 32K char bank multiplier */
    int32_t  bank_scroll2;      /* Scroll 2 16K tile bank multiplier */
    int32_t  bank_scroll3;      /* Scroll 3 4K tile bank multiplier */
    int32_t  space_scroll1;     /* Unused char code (-1 = none, >=0 = skip) */
    int32_t  start_scroll2;     /* Min valid Scroll 2 raw code */
    int32_t  end_scroll2;       /* Max valid Scroll 2 raw code */
    int32_t  start_scroll3;     /* Min valid Scroll 3 raw code */
    int32_t  end_scroll3;       /* Max valid Scroll 3 raw code */
    int32_t  kludge;            /* Gfx/HW kludge (0, 1=ghouls, 2=3wonders, 4=mercs, 5=pang3, 7=forgottn) */

    /* Clocks, inputs, DIP switches, and flags */
    uint32_t m68k_hz;           /* 68000 clock Hz (10000000 or 12000000) */
    uint32_t oki_hz;            /* OKI6295 sample rate Hz (7576 or 6061) */
    uint8_t  input_type;        /* CPS1_INPUT_* */
    uint8_t  orientation;       /* 0 = horizontal, 1 = vertical ROT270 */
    uint8_t  dswa;              /* Default DIP Switch A */
    uint8_t  dswb;              /* Default DIP Switch B */
    uint8_t  dswc;              /* Default DIP Switch C */
    uint8_t  flags;             /* CPS1_FLAG_* */
    uint16_t reserved[9];       /* Pad config struct to 168 bytes */
} cps1_game_config_t;

typedef struct {
    uint8_t p1;      /* Player 1 joystick + buttons (CPS1_INP_*) */
    uint8_t p2;      /* Player 2 joystick + buttons (CPS1_INP_*) */
    uint8_t sys;     /* Coins + Starts (CPS1_SYS_*)              */
    uint8_t dswa;    /* DIP Switch A (0 = use game default)      */
    uint8_t dswb;    /* DIP Switch B (0 = use game default)      */
    uint8_t dswc;    /* DIP Switch C (0 = use game default)      */
    uint8_t p1_extra;/* SF2 P1 Kick buttons (bits 0..2 = LK/MK/HK)*/
    uint8_t p2_extra;/* SF2 P2 Kick buttons (bits 0..2 = LK/MK/HK)*/
} cps1_inputs_t;

typedef struct {
    uint32_t m68k_us;
    uint32_t z80_us;
    uint32_t ym2151_us;
    uint32_t oki_us;
    uint32_t gfx_us;
    uint32_t gfx_clr_us;
    uint32_t gfx_s1_us;
    uint32_t gfx_s2_us;
    uint32_t gfx_s3_us;
    uint32_t gfx_spr_us;
} cps1_prof_t;

/* Optional hardware profiler hook (populated per-frame when non-NULL) */
void cps1_emu_set_profiler(cps1_prof_t *prof);

/* Query the currently loaded game configuration */
const cps1_game_config_t *cps1_emu_get_config(void);

#if defined(__sh__) || defined(HOST_TA_SIM)
/* Pre-twiddle CPS-1 tiles into PowerVR2 CLUT4 VRAM (call once after cps1_emu_init) */
void cps1_hw_gfx_init(void);

/* Emit all CPS-1 layers and sprites as PowerVR2 PUNCHTHRU CLUT4 quads inside ta_commit_begin()..end() */
void cps1_hw_render_ta(float x0, float y0, float x1, float y1, int bilinear);
#endif

/* Returns pointer to internal 384x224 visible region (stride = CPS1_BUF_STRIDE uint16s).
 * Pass this as vis_fb_out to cps1_emu_frame() to avoid an extra 172 KB framebuffer copy. */
uint16_t *cps1_emu_get_vis_canvas(void);

/* Load packed .cps1 bundle from ROMFS file path and initialize CPS-1 hardware */
int  cps1_emu_init(const char *cps1_rom_path);

/* Load packed .cps1 bundle directly from Net DIMM Cartridge RAM at cart_offset */
int  cps1_emu_init_mem(uint32_t cart_offset, void (*progress_cb)(int pct));

/* Reset 68000, Z80, YM2151, OKIM6295, and video state */
void cps1_emu_reset(void);

/* Execute one 60 Hz frame:
 * - Runs 68000 (10/12 MHz) + Z80 (3.58 MHz) + YM2151 + OKIM6295
 * - Renders 384x224 RGB565 visible screen into vis_fb_out (or NULL when using TA hardware renderer)
 * - Renders stereo 16-bit PCM audio at 11025 Hz into audio_lr_out (num_samples * 2 int16_t)
 */
void cps1_emu_frame(
    const cps1_inputs_t *inp,
    uint16_t *vis_fb_out,
    int16_t  *audio_lr_out,
    int       num_audio_samples
);

#endif /* CPS1_EMU_H */
