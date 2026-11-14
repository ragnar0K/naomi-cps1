#include "../src/cps1_emu.h"
#include "../src/m68k/m68k.h"
#include "../src/sound/z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void save_ppm(const char *path, const uint16_t *fb)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", CPS1_VIS_WIDTH, CPS1_VIS_HEIGHT);
    for (int i = 0; i < CPS1_VIS_WIDTH * CPS1_VIS_HEIGHT; i++) {
        uint16_t p = fb[i];
        uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((p >>  5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)(( p        & 0x1F) * 255 / 31);
        uint8_t rgb[3] = { r, g, b };
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    const char *rom_path = (argc > 1) ? argv[1] : "romfs/ffight.cps1";
    int rc = cps1_emu_init(rom_path);
    if (rc != 0) {
        fprintf(stderr, "cps1_emu_init failed: %d\n", rc);
        return 1;
    }

    static uint16_t vis_fb[CPS1_VIS_WIDTH * CPS1_VIS_HEIGHT];
    static int16_t  audio_lr[512 * 2];

    int64_t total_audio_energy = 0;
    int     peak_audio_sample  = 0;

    for (int f = 0; f < 1800; f++) {
        cps1_inputs_t inp;
        memset(&inp, 0, sizeof(inp));

        if (f >= 700 && f < 708) inp.sys |= CPS1_SYS_COIN1;
        if (f >= 720 && f < 728) inp.sys |= CPS1_SYS_COIN1;
        if (f >= 750 && f < 758) inp.sys |= CPS1_SYS_START1;
        if (f >= 790 && f < 798) inp.p1  |= CPS1_INP_RIGHT;
        if (f >= 820 && f < 828) inp.p1  |= CPS1_INP_BTN1;
        if (f >= 920 && f < 928) inp.p1  |= CPS1_INP_BTN1;
        if (f >= 1000 && f < 1799) {
            if ((f % 120) < 80) inp.p1 |= CPS1_INP_RIGHT;
            if ((f % 16) < 4)   inp.p1 |= CPS1_INP_BTN1;
            if ((f % 90) == 45) inp.p1 |= CPS1_INP_BTN2;
        }

        int num_samples = (f & 1) ? 368 : 367;
        cps1_emu_frame(&inp, vis_fb, audio_lr, num_samples);

        for (int i = 0; i < num_samples * 2; i++) {
            int s = abs(audio_lr[i]);
            total_audio_energy += s;
            if (s > peak_audio_sample) peak_audio_sample = s;
        }

        if (f == 735 || f == 805 || f == 900 || f == 1200 || f == 1500 || f == 1780) {
            char path[256];
            snprintf(path, sizeof(path), "/tmp/ffight_frame_%04d.ppm", f);
            save_ppm(path, vis_fb);
            printf("Frame %4d: M68K PC=0x%06X  Z80 PC=0x%04X  PeakAudio=%d\n",
                   f, m68k_get_reg(NULL, M68K_REG_PC), z80_get_pc(), peak_audio_sample);
        }
    }

    printf("Total audio energy across 900 frames: %lld (Peak sample: %d)\n",
           (long long)total_audio_energy, peak_audio_sample);
    return 0;
}
