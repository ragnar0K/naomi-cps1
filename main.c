#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <naomi/video.h>
#include <naomi/ta.h>
#include <naomi/audio.h>
#include <naomi/maple.h>
#include <naomi/romfs.h>
#include <naomi/timer.h>
#include <naomi/system.h>
#include <naomi/cart.h>
#include <naomi/dimmcomms.h>
#include <naomi/message/message.h>

#include "src/cps1_emu.h"

extern void ta_render_begin(void);
extern void ta_render_wait(void);
extern void _video_swap_vbuffers(void);

/*
 * ============================================================================
 * CAPCOM CPS-1 MULTI-GAME EMULATOR & NET DIMM MINI SERVER FOR SEGA NAOMI
 * ============================================================================
 * Supports TWO seamless ROM loading workflows in one unified binary:
 *
 * 1. Standalone Packaged .bin ("rom://game.cps1" or "rom://ffight.cps1"):
 *    Built in ~1 second by `cps1_tool.py build-bin <rom.zip>` without C
 *    recompilation. Boots directly into the game when selected from netdimm_menu!
 *
 * 2. Net DIMM "Mini Server" & Jukebox Mode (`naomi_cps1_server.bin`):
 *    When booted without a baked-in ROMFS game (or after exiting a game with
 *    P1 START + P2 START held for 1.5s), displays an interactive arcade menu
 *    and listens for live ROM pushes over the network:
 *      - `cps1_tool.py push <NAOMI_IP> <rom.zip>` pushes any CPS-1 game
 *        directly into Net DIMM Cartridge RAM (`0x04010000`) and boots it!
 *      - `cps1_tool.py serve <NAOMI_IP> <roms_dir>` populates the on-screen
 *        Jukebox menu (`0x04000000`) so you can browse and launch any CPS-1
 *        ROM in your folder using the NAOMI arcade joystick!
 * ============================================================================
 */

#define AUDIO_RINGBUF_SAMPLES 4096

/* Net DIMM Cartridge RAM offsets (accessed via cart_read/cart_write; 0xA4000000 is VRAM, NOT cart RAM!) */
#define CART_CATALOG_OFFSET      0x04000000u  /* 60 KB Jukebox catalog area */
#define CART_MAILBOX_OFFSET      0x0400FF00u  /* 256-byte live server mailbox */
#define CART_ROM_PAYLOAD_OFFSET  0x04010000u  /* Uploaded .cps1 bundle payload */

#define MAILBOX_MAGIC_IDLE       0x43505330u  /* "CPS0" - Server waiting */
#define MAILBOX_MAGIC_UPLD       0x43505355u  /* "CPSU" - Host uploading ROM */
#define MAILBOX_MAGIC_BOOT       0x43505342u  /* "CPSB" - Host upload complete, boot now! */
#define MAILBOX_MAGIC_RUN        0x43505352u  /* "CPSR" - Game running */
#define MAILBOX_MAGIC_REQ        0x43505351u  /* "CPSQ" - NAOMI requesting ROM index from host */

#define CATALOG_MAGIC            0x43415431u  /* "CAT1" - Jukebox catalog header */
#define CATALOG_MAX_GAMES        64

typedef struct __attribute__((packed)) {
    uint32_t magic;          /* +0x00: MAILBOX_MAGIC_* */
    uint32_t bundle_size;    /* +0x04: Total size of .cps1 bundle at 0x04010000 */
    uint32_t progress_bytes; /* +0x08: Bytes uploaded so far (for live progress bar) */
    uint32_t req_game_idx;   /* +0x0C: Requested game index from Jukebox menu */
    char     game_name[32];  /* +0x10: Short name or title of uploading/running game */
} cps1_mailbox_t;

typedef struct __attribute__((packed)) {
    char     short_name[16];
    char     title[44];
    uint32_t size_kb;
} catalog_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t        magic;
    uint32_t        count;
    catalog_entry_t entries[CATALOG_MAX_GAMES];
} cps1_catalog_t;

static volatile uint32_t g_srv_cmd       = MAILBOX_MAGIC_IDLE;
static volatile uint32_t g_srv_size      = 0;
static volatile uint32_t g_srv_progress  = 0;
static volatile uint32_t g_srv_req_idx   = 0xFFFFFFFFu;

static uint32_t server_peek_hook(unsigned int address, int size)
{
    if (address == 0xC0DE10) return g_srv_cmd;
    if (address == 0xC0DE20) return g_srv_size;
    if (address == 0xC0DE30) return g_srv_progress;
    if (address == 0xC0DE40) return g_srv_req_idx;
    return 0;
}

static void server_poke_hook(unsigned int address, int size, uint32_t data)
{
    if (address == 0xC0DE10) g_srv_cmd = data;
    if (address == 0xC0DE20) g_srv_size = data;
    if (address == 0xC0DE30) g_srv_progress = data;
    if (address == 0xC0DE40) g_srv_req_idx = data;
}

static void decode_progress_cb(int pct)
{
    video_fill_screen(rgb(12, 16, 28));
    video_draw_box(50, 170, 590, 290, rgb(0, 255, 128));
    video_draw_debug_text(130, 195, rgb(0, 255, 255), "CAPCOM CPS-1 NET DIMM MINI SERVER");
    video_draw_debug_text(135, 225, rgb(255, 255, 255), "Decoding CPS-1 Planar GFX (%d%%)...", pct);
    video_draw_box(100, 252, 540, 270, rgb(0, 200, 255));
    int fill_w = (436 * pct) / 100;
    if (fill_w > 0)
        video_fill_box(102, 254, 102 + fill_w, 268, rgb(0, 255, 128));
    video_display_on_vblank();
}

static int16_t g_audio_lr[512 * 2] __attribute__((aligned(32)));

typedef enum {
    ASPECT_4_3_FULL = 0,   /* 640x480 full screen */
    ASPECT_12_7_PROP = 1,  /* 640x374 square-pixel 384:224 aspect */
    ASPECT_INT_2X = 2      /* 576x448 integer 2x vertical scanlines */
} aspect_mode_t;

static const char *aspect_name(int mode)
{
    switch (mode)
    {
        case ASPECT_12_7_PROP: return "12:7 Native (640x374)";
        case ASPECT_INT_2X:    return "2x Integer Y (576x448)";
        default:               return "4:3 Fullscreen (640x480)";
    }
}

static void compute_aspect_rect(int aspect_mode, float *x0, float *y0, float *x1, float *y1)
{
    *x0 = 0.0f;
    *x1 = (float)video_width();
    *y0 = 0.0f;
    *y1 = (float)video_height();

    if (aspect_mode == ASPECT_12_7_PROP)
    {
        *y0 = 53.0f;
        *y1 = 427.0f;
    }
    else if (aspect_mode == ASPECT_INT_2X)
    {
        *x0 = 32.0f;
        *x1 = 608.0f;
        *y0 = 16.0f;
        *y1 = 464.0f;
    }
}

static uint8_t map_player_inputs(const player_buttons_t *btn, int is_sf2, uint8_t *extra_out)
{
    uint8_t out = 0;
    uint8_t ext = 0;
    if (btn->right)   out |= CPS1_INP_RIGHT;
    if (btn->left)    out |= CPS1_INP_LEFT;
    if (btn->down)    out |= CPS1_INP_DOWN;
    if (btn->up)      out |= CPS1_INP_UP;
    if (btn->button1) out |= CPS1_INP_BTN1;
    if (btn->button2) out |= CPS1_INP_BTN2;

    if (is_sf2)
    {
        /* 6-Button Street Fighter II Layout:
         * B1/B2/B3 = Light/Medium/Heavy Punch
         * B4/B5/B6 = Light/Medium/Heavy Kick */
        if (btn->button3) out |= CPS1_INP_BTN3;
        if (btn->button4) ext |= 0x01; /* Light Kick */
        if (btn->button5) ext |= 0x02; /* Medium Kick */
        if (btn->button6) ext |= 0x04; /* Heavy Kick */
    }
    else
    {
        /* Standard 2/3-button CPS-1 games:
         * B3 = dedicated 3rd button AND Attack+Jump special macro for 2-button games */
        if (btn->button3) out |= (CPS1_INP_BTN3 | CPS1_INP_BTN1 | CPS1_INP_BTN2);
    }

    if (extra_out) *extra_out = ext;
    return out;
}

static void fast_forward_post(const char *title)
{
    cps1_inputs_t zero_inp;
    memset(&zero_inp, 0, sizeof(zero_inp));
    for (int f = 0; f < 620; f++)
    {
        cps1_emu_frame(&zero_inp, NULL, NULL, 0);
        if ((f & 63) == 0)
        {
            video_fill_screen(rgb(12, 16, 28));
            video_draw_box(50, 165, 590, 295, rgb(0, 200, 255));
            video_draw_debug_text(130, 190, rgb(0, 255, 255), "CAPCOM CPS-1 EMULATOR FOR SEGA NAOMI");
            video_draw_debug_text(90, 215, rgb(255, 255, 255), "Game: %s", title);
            video_draw_debug_text(140, 240, rgb(0, 255, 128), "Running CPS-1 Power-On Self Test (%d%%)...", (f * 100) / 620);
            video_draw_debug_text(105, 265, rgb(255, 220, 100), "Hold P1 START + P2 START (1.5s) anytime for Menu");
            video_display_on_vblank();
        }
    }
}

/* Wait in the interactive Net DIMM Mini Server / Jukebox Menu until a ROM is pushed or selected */
static void run_netdimm_server_menu(void)
{
    static cps1_catalog_t cat;
    cps1_mailbox_t mb;

    memset(&mb, 0, sizeof(mb));
    mb.magic          = MAILBOX_MAGIC_IDLE;
    mb.bundle_size    = 0;
    mb.progress_bytes = 0;
    mb.req_game_idx   = 0xFFFFFFFFu;
    strcpy(mb.game_name, "Waiting for ROM...");
    cart_write(CART_MAILBOX_OFFSET, &mb, sizeof(mb));
    g_srv_cmd = MAILBOX_MAGIC_IDLE;

    int cursor = 0;
    uint32_t anim_tick = 0;
    uint32_t loaded_cat_count = 0;

    while (1)
    {
        maple_poll_buttons();
        jvs_buttons_t pressed = maple_buttons_pressed();

        if ((anim_tick & 15) == 0)
        {
            cart_read(&mb, CART_MAILBOX_OFFSET, sizeof(mb));
            if (mb.magic == MAILBOX_MAGIC_UPLD || mb.magic == MAILBOX_MAGIC_BOOT)
            {
                g_srv_cmd      = mb.magic;
                g_srv_size     = mb.bundle_size;
                g_srv_progress = mb.progress_bytes;
            }
        }
        uint32_t magic = mb.magic;

        if (g_srv_cmd == MAILBOX_MAGIC_BOOT || magic == MAILBOX_MAGIC_BOOT)
        {
            uint32_t sz = mb.bundle_size ? mb.bundle_size : g_srv_size;
            if (sz >= 64)
            {
                video_fill_screen(rgb(12, 16, 28));
                video_draw_box(50, 170, 590, 290, rgb(0, 255, 128));
                video_draw_debug_text(130, 195, rgb(0, 255, 255), "CAPCOM CPS-1 NET DIMM MINI SERVER");
                video_draw_debug_text(120, 225, rgb(255, 255, 255), "Unpacking & Decoding GFX (%lu KB)...", (unsigned long)(sz / 1024));
                video_display_on_vblank();

                int rc = cps1_emu_init_mem(CART_ROM_PAYLOAD_OFFSET, decode_progress_cb);
                if (rc == 0)
                {
                    mb.magic = MAILBOX_MAGIC_RUN;
                    g_srv_cmd = MAILBOX_MAGIC_RUN;
                    cart_write(CART_MAILBOX_OFFSET, &mb, sizeof(mb));
                    return;
                }
                else
                {
                    mb.magic = MAILBOX_MAGIC_IDLE;
                    g_srv_cmd = MAILBOX_MAGIC_IDLE;
                    snprintf(mb.game_name, 32, "Load Error (%d)", rc);
                    cart_write(CART_MAILBOX_OFFSET, &mb, sizeof(mb));
                }
            }
        }

        /* Jukebox Catalog navigation if `cps1_tool.py serve` populated 0x04000000 */
        if (loaded_cat_count == 0 && (anim_tick % 30) == 0)
        {
            cart_read(&cat, CART_CATALOG_OFFSET, 8);
            if (cat.magic == CATALOG_MAGIC && cat.count > 0 && cat.count <= CATALOG_MAX_GAMES)
            {
                cart_read(cat.entries, CART_CATALOG_OFFSET + 8, (unsigned int)cat.count * sizeof(catalog_entry_t));
                loaded_cat_count = cat.count;
            }
        }
        int cat_count = (int)loaded_cat_count;
        if (cat_count > 0 && magic != MAILBOX_MAGIC_UPLD && magic != MAILBOX_MAGIC_REQ)
        {
            if (pressed.player1.up || pressed.player2.up)
                cursor = (cursor - 1 + cat_count) % cat_count;
            if (pressed.player1.down || pressed.player2.down)
                cursor = (cursor + 1) % cat_count;
            if (pressed.player1.left)
                cursor = (cursor - 8 >= 0) ? (cursor - 8) : 0;
            if (pressed.player1.right)
                cursor = (cursor + 8 < cat_count) ? (cursor + 8) : (cat_count - 1);

            if (pressed.player1.button1 || pressed.player1.start)
            {
                mb.req_game_idx = (uint32_t)cursor;
                mb.magic        = MAILBOX_MAGIC_REQ;
                g_srv_req_idx   = (uint32_t)cursor;
                g_srv_cmd       = MAILBOX_MAGIC_REQ;
                cart_write(CART_MAILBOX_OFFSET, &mb, sizeof(mb));
            }
        }

        /* Draw Server / Jukebox UI */
        video_fill_screen(rgb(10, 14, 26));
        video_fill_box(24, 18, 616, 66, rgb(18, 28, 52));
        video_draw_box(24, 18, 616, 66, rgb(0, 200, 255));
        video_draw_debug_text(88, 28, rgb(0, 255, 255), "CAPCOM CPS-1 ARCADE SYSTEM  -  NET DIMM MINI SERVER");
        video_draw_debug_text(76, 46, rgb(200, 220, 255), "Push any CPS-1 .zip from PC or select from Jukebox below");

        if (magic == MAILBOX_MAGIC_UPLD || g_srv_cmd == MAILBOX_MAGIC_UPLD || magic == MAILBOX_MAGIC_REQ)
        {
            uint32_t total = mb.bundle_size ? mb.bundle_size : g_srv_size;
            uint32_t done  = mb.progress_bytes ? mb.progress_bytes : g_srv_progress;
            uint32_t pct   = (total > 0) ? ((done * 100u) / total) : 0;
            if (pct > 100) pct = 100;

            video_fill_box(60, 170, 580, 300, rgb(16, 24, 44));
            video_draw_box(60, 170, 580, 300, rgb(0, 255, 128));
            video_draw_debug_text(140, 195, rgb(0, 255, 128), "RECEIVING CPS-1 ROM OVER NETWORK...");
            video_draw_debug_text(140, 220, rgb(255, 255, 255), "ROM: %.31s", mb.game_name);
            video_draw_debug_text(
                140, 242, rgb(255, 220, 100),
                "Progress: %lu / %lu KB (%lu%%)",
                (unsigned long)(done / 1024),
                (unsigned long)(total / 1024),
                (unsigned long)pct
            );
            /* Progress bar */
            video_draw_box(100, 264, 540, 282, rgb(0, 200, 255));
            int fill_w = (int)((436u * pct) / 100u);
            if (fill_w > 0)
            {
                video_fill_box(102, 266, 102 + fill_w, 280, rgb(0, 255, 128));
            }
        }
        else if (cat_count > 0)
        {
            video_fill_box(24, 78, 616, 412, rgb(14, 20, 36));
            video_draw_box(24, 78, 616, 412, rgb(0, 160, 220));
            video_draw_debug_text(
                40, 88, rgb(255, 220, 100),
                "JUKEBOX CATALOG (%d CPS-1 Games Ready)  -  UP/DOWN + START/B1 to Launch:",
                cat_count
            );

            int page_size = 14;
            int start_idx = (cursor / page_size) * page_size;
            for (int i = 0; i < page_size && (start_idx + i) < cat_count; i++)
            {
                int idx = start_idx + i;
                int y = 114 + i * 20;
                if (idx == cursor)
                {
                    video_fill_box(34, y - 3, 606, y + 13, rgb(0, 90, 150));
                    video_draw_debug_text(
                        42, y, rgb(255, 255, 0),
                        "> [%-10.10s] %-38.38s (%4lu KB)",
                        cat.entries[idx].short_name,
                        cat.entries[idx].title,
                        (unsigned long)cat.entries[idx].size_kb
                    );
                }
                else
                {
                    video_draw_debug_text(
                        42, y, rgb(210, 225, 240),
                        "  [%-10.10s] %-38.38s (%4lu KB)",
                        cat.entries[idx].short_name,
                        cat.entries[idx].title,
                        (unsigned long)cat.entries[idx].size_kb
                    );
                }
            }
        }
        else
        {
            video_fill_box(40, 105, 600, 390, rgb(14, 20, 36));
            video_draw_box(40, 105, 600, 390, rgb(0, 160, 220));
            video_draw_debug_text(135, 130, rgb(0, 255, 128), "READY FOR LIVE NETWORK ROM PUSH OR JUKEBOX");
            video_draw_debug_text(64, 170, rgb(255, 255, 255), "Option A - Push & Boot any MAME CPS-1 .zip immediately:");
            video_draw_debug_text(80, 192, rgb(255, 220, 100), "python3 cps1_tool.py push <NAOMI_IP> /path/to/rom.zip");
            video_draw_debug_text(64, 232, rgb(255, 255, 255), "Option B - Serve an entire ROM folder to this Jukebox Menu:");
            video_draw_debug_text(80, 254, rgb(255, 220, 100), "python3 cps1_tool.py serve <NAOMI_IP> ~/Downloads/ROMS");
            video_draw_debug_text(64, 294, rgb(255, 255, 255), "Option C - Package standalone .bin files for netdimm_menu:");
            video_draw_debug_text(80, 316, rgb(255, 220, 100), "python3 cps1_tool.py build-bin ~/Downloads/ROMS -o ~/NAOMIGAMES");
            video_draw_debug_text(
                145, 358, rgb(0, 220, 255),
                "Status: %.31s %c",
                mb.game_name,
                "|/-\\"[(anim_tick >> 3) & 3]
            );
        }

        video_fill_box(24, 424, 616, 462, rgb(14, 18, 32));
        video_draw_box(24, 424, 616, 462, rgb(0, 180, 255));
        video_draw_debug_text(52, 438, rgb(180, 220, 255), "In-Game Hotkeys: Hold P1+P2 START (1.5s) = Return to Server Menu");

        video_display_on_vblank();
        anim_tick++;
    }
}

void main()
{
    video_init(VIDEO_COLOR_1555);
    /* Mask unused Holly HBlank interrupt to avoid 524 unnecessary ISRs per frame */
    *((volatile uint32_t *)0xA05F6910) &= ~0x00000020u;
    ta_set_background_color(rgb(0, 0, 0));
    romfs_init_default();
    message_init();
    message_stdio_redirect_init();
    dimm_comms_attach_hooks(&server_peek_hook, &server_poke_hook);
    printf("CPS1_NAOMI_MULTI: booted\n");

    /* 1. Check if a game bundle is baked into ROMFS ("rom://game.cps1" or "rom://ffight.cps1") */
    int init_ok = -1;
    FILE *fp_check = fopen("rom://game.cps1", "rb");
    if (fp_check)
    {
        fclose(fp_check);
        video_fill_screen(rgb(12, 16, 28));
        video_draw_box(60, 170, 580, 290, rgb(0, 200, 255));
        video_draw_debug_text(130, 195, rgb(0, 255, 255), "CAPCOM CPS-1 EMULATOR FOR SEGA NAOMI");
        video_draw_debug_text(150, 220, rgb(255, 255, 255), "Loading & Decoding rom://game.cps1...");
        video_draw_debug_text(140, 245, rgb(180, 200, 220), "68000 + Z80 + YM2151 FM + OKIM6295 ADPCM");
        video_display_on_vblank();
        init_ok = cps1_emu_init("rom://game.cps1");
    }
    else
    {
        fp_check = fopen("rom://ffight.cps1", "rb");
        if (fp_check)
        {
            fclose(fp_check);
            video_fill_screen(rgb(12, 16, 28));
            video_draw_box(60, 170, 580, 290, rgb(0, 200, 255));
            video_draw_debug_text(130, 195, rgb(0, 255, 255), "CAPCOM CPS-1 EMULATOR FOR SEGA NAOMI");
            video_draw_debug_text(150, 220, rgb(255, 255, 255), "Loading Final Fight [rom://ffight.cps1]...");
            video_display_on_vblank();
            init_ok = cps1_emu_init("rom://ffight.cps1");
        }
    }

    /* 2. If no ROMFS game was baked in, enter the interactive Net DIMM Mini Server / Jukebox Menu! */
    if (init_ok != 0)
    {
        run_netdimm_server_menu();
    }

    audio_init();
    audio_register_ringbuffer(AUDIO_FORMAT_16BIT, CPS1_AUDIO_RATE, AUDIO_RINGBUF_SAMPLES);

    int bilinear = 0; /* Crisp arcade nearest-neighbor by default */
    int aspect_mode = ASPECT_4_3_FULL;

    while (1)
    {
        const cps1_game_config_t *cfg = cps1_emu_get_config();
        int is_sf2 = (cfg && cfg->input_type == CPS1_INPUT_SF2) ? 1 : 0;
        const char *game_title = (cfg && cfg->game_title[0]) ? cfg->game_title : "CPS-1 Arcade Game";

        /* Fast-forward initial CPS-1 ROM/RAM power-on self-test (~620 frames) */
        fast_forward_post(game_title);
        cps1_hw_gfx_init();

        int osd_timer = 300; /* Show control hints for first 5 seconds */
        uint32_t frame_count = 0;
        int frame_timer = profile_start();

        cps1_prof_t prof;
        memset(&prof, 0, sizeof(prof));
        cps1_emu_set_profiler(&prof);

        uint32_t p_maple = 0, p_aica = 0, p_vram = 0, p_ta = 0, p_vbl = 0;
        uint64_t sum_us = 0;
        uint32_t avg_fps_x10 = 600;
        uint32_t smooth_us = 16667;
        uint32_t elapsed_us = 16667;
        int ta_render_in_flight = 0;
        int exit_hold_frames = 0;

        /* Inner 60 FPS emulation loop for the active game */
        while (1)
        {
            /* Check if host pushed a new ROM live over Net DIMM while game is running */
            if ((frame_count & 15) == 0)
            {
                cps1_mailbox_t mb;
                cart_read(&mb, CART_MAILBOX_OFFSET, 8);
                if (mb.magic == MAILBOX_MAGIC_BOOT || g_srv_cmd == MAILBOX_MAGIC_BOOT)
                {
                    if (ta_render_in_flight)
                    {
                        ta_render_wait();
                        ta_render_in_flight = 0;
                    }
                    uint32_t sz = mb.bundle_size ? mb.bundle_size : g_srv_size;
                    if (sz >= 64 && cps1_emu_init_mem(CART_ROM_PAYLOAD_OFFSET, decode_progress_cb) == 0)
                    {
                        mb.magic = MAILBOX_MAGIC_RUN;
                        g_srv_cmd = MAILBOX_MAGIC_RUN;
                        cart_write(CART_MAILBOX_OFFSET, &mb, 8);
                        break; /* Restart outer loop with new game! */
                    }
                    mb.magic = MAILBOX_MAGIC_RUN;
                    g_srv_cmd = MAILBOX_MAGIC_RUN;
                    cart_write(CART_MAILBOX_OFFSET, &mb, 8);
                }
            }

            int t_m = (osd_timer > 0) ? profile_start() : -1;
            maple_poll_buttons();
            if (t_m >= 0) p_maple = profile_end(t_m);
            jvs_buttons_t pressed = maple_buttons_pressed();
            jvs_buttons_t held = maple_buttons_held();

            /* Hold P1 START + P2 START for 1.5 seconds (90 frames) to return to Jukebox / Server Menu */
            if (held.player1.start && held.player2.start)
            {
                exit_hold_frames++;
                if (exit_hold_frames >= 90)
                {
                    if (ta_render_in_flight)
                    {
                        ta_render_wait();
                        ta_render_in_flight = 0;
                    }
                    run_netdimm_server_menu();
                    break; /* Restart outer loop with newly selected game! */
                }
            }
            else
            {
                exit_hold_frames = 0;
            }

            /* Hotkeys for video filter, aspect ratio, and OSD help:
             * On 6-button SF2 games, B5/B6 are Medium/Heavy Kick, so use START+B5 / START+B6 */
            if ((!is_sf2 && pressed.player1.button5) || (is_sf2 && held.player1.start && pressed.player1.button5))
            {
                bilinear = !bilinear;
                osd_timer = 180;
            }
            if ((!is_sf2 && pressed.player1.button6) || (is_sf2 && held.player1.start && pressed.player1.button6))
            {
                aspect_mode = (aspect_mode + 1) % 3;
                osd_timer = 180;
            }
            if (held.player1.start && pressed.player1.up)
            {
                osd_timer = (osd_timer > 0) ? 0 : 240;
            }

            cps1_emu_set_profiler(osd_timer > 0 ? &prof : NULL);

            int p1_coin_combo = (held.player1.start && (held.player1.button1 || held.player1.down));
            int p2_coin_combo = (held.player2.start && (held.player2.button1 || held.player2.down));

            cps1_inputs_t inp;
            memset(&inp, 0, sizeof(inp));
            inp.p1 = map_player_inputs(&held.player1, is_sf2, &inp.p1_extra);
            inp.p2 = map_player_inputs(&held.player2, is_sf2, &inp.p2_extra);

            if (p1_coin_combo) inp.p1 &= ~(CPS1_INP_BTN1 | CPS1_INP_DOWN);
            if (p2_coin_combo) inp.p2 &= ~(CPS1_INP_BTN1 | CPS1_INP_DOWN);

            if (held.player1.start && !p1_coin_combo) inp.sys |= CPS1_SYS_START1;
            if (held.player2.start && !p2_coin_combo) inp.sys |= CPS1_SYS_START2;

            /* Insert Coin: Service, Panel Switch, or Start+B1 / Start+Down across all games */
            if (held.player1.service || held.psw1 || p1_coin_combo)
            {
                inp.sys |= CPS1_SYS_COIN1;
            }
            if (held.player2.service || held.psw2 || p2_coin_combo)
            {
                inp.sys |= CPS1_SYS_COIN2;
            }
            if (held.test)
            {
                inp.sys |= CPS1_SYS_SERVICE;
            }

            /* Synthesize exact 11025 Hz arcade audio (183.75 samples/frame = 735 samples every 4 frames) */
            int num_samples = ((frame_count & 3) == 3) ? 183 : 184;

            /* Run 1 frame of CPS-1 emulation on SH-4 CPU in parallel with PowerVR2 TSP rendering previous frame */
            cps1_emu_frame(&inp, NULL, g_audio_lr, num_samples);

            /* Push stereo 16-bit PCM to Yamaha AICA ringbuffer */
            int t_a = (osd_timer > 0) ? profile_start() : -1;
            audio_write_stereo_data(g_audio_lr, num_samples);
            if (t_a >= 0) p_aica = profile_end(t_a);

            /* Complete previous frame's async PowerVR2 TSP render and flip framebuffer */
            if (ta_render_in_flight)
            {
                int t_ta_w = (osd_timer > 0) ? profile_start() : -1;
                ta_render_wait();
                if (t_ta_w >= 0) p_ta = profile_end(t_ta_w);
                ta_render_in_flight = 0;

                /* Profiler bar & OSD banner (shown during first 5s or toggled with START+UP) */
                if (osd_timer > 0)
                {
                    osd_timer--;
                    video_fill_box(8, 6, 632, 34, rgb(8, 10, 18));
                    video_draw_debug_text(
                        14, 9, rgb(0, 255, 128),
                        "AVG:%2lu.%lu TOT:%6lu | 68K:%6lu Z80:%6lu YM:%6lu",
                        (unsigned long)(avg_fps_x10 / 10), (unsigned long)(avg_fps_x10 % 10),
                        (unsigned long)elapsed_us,
                        (unsigned long)prof.m68k_us,
                        (unsigned long)prof.z80_us,
                        (unsigned long)prof.ym2151_us
                    );
                    video_draw_debug_text(
                        14, 21, rgb(255, 220, 100),
                        "OKI:%5lu GFX:%6lu VRAM:%5lu TA:%5lu AICA:%5lu MAP:%5lu VBL:%5lu",
                        (unsigned long)prof.oki_us,
                        (unsigned long)prof.gfx_us,
                        (unsigned long)p_vram,
                        (unsigned long)p_ta,
                        (unsigned long)p_aica,
                        (unsigned long)p_maple,
                        (unsigned long)p_vbl
                    );

                    video_fill_box(16, 434, 624, 470, rgb(12, 16, 28));
                    video_draw_box(16, 434, 624, 470, rgb(0, 200, 255));
                    video_draw_debug_text(
                        26, 441, rgb(0, 255, 255),
                        "%-22.22s | Filter:%s | Aspect:%s",
                        game_title,
                        bilinear ? "Bilinear" : "Nearest",
                        aspect_name(aspect_mode)
                    );
                    if (is_sf2)
                    {
                        video_draw_debug_text(
                            26, 455, rgb(255, 255, 180),
                            "B1-3:Punch B4-6:Kick  START+B1/SVC:Coin  P1+P2 START:Menu"
                        );
                    }
                    else
                    {
                        video_draw_debug_text(
                            26, 455, rgb(255, 255, 180),
                            "B1-3:Action  START+B1/SVC:Coin  B5:Filt B6:Asp  P1+P2 START:Menu"
                        );
                    }
                }

                /* Adaptive VBlank sync: sleep for VBlank if frame finished under 16.2ms;
                 * if slightly over 16.2ms, swap buffers immediately instead of stalling to 33.3ms (30fps) */
                uint32_t pre_vbl_us = (uint32_t)profile_end(frame_timer);
                if (pre_vbl_us < 16200)
                {
                    int vbl_t = profile_start();
                    video_display_on_vblank();
                    uint32_t vbl_wait = (uint32_t)profile_end(vbl_t);
                    p_vbl = vbl_wait;
                    elapsed_us = pre_vbl_us + vbl_wait;
                }
                else
                {
                    _video_swap_vbuffers();
                    p_vbl = 0;
                    elapsed_us = pre_vbl_us;
                }
                frame_timer = profile_start();

                if (elapsed_us > 1000 && elapsed_us < 100000)
                {
                    smooth_us = (smooth_us * 31 + elapsed_us) >> 5;
                }
                sum_us += elapsed_us;
                if ((frame_count % 60) == 0 && sum_us > 0)
                {
                    avg_fps_x10 = (uint32_t)(600000000ULL / sum_us);
                    sum_us = 0;
                }
            }
            else
            {
                /* Reset frame_timer on initial pipeline fill frame so next frame measures a single frame */
                elapsed_us = (uint32_t)profile_end(frame_timer);
                frame_timer = profile_start();
            }

            float x0, y0, x1, y1;
            compute_aspect_rect(aspect_mode, &x0, &y0, &x1, &y1);

            /* Submit CPS-1 tiles & sprites to PowerVR2 TA and launch async hardware render */
            ta_commit_begin();
            cps1_hw_render_ta(x0, y0, x1, y1, bilinear);
            ta_commit_end();
            ta_render_begin();
            ta_render_in_flight = 1;

            frame_count++;
        }
    }
}

void test()
{
    video_init(VIDEO_COLOR_1555);
    while (1)
    {
        video_fill_screen(rgb(16, 20, 32));
        video_draw_debug_text(140, 220, rgb(0, 255, 255), "CAPCOM CPS-1 MULTI-SYSTEM - NAOMI TEST MODE");
        video_display_on_vblank();
    }
}
