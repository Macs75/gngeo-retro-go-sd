/*
 * neo_prof.c -- device PROF build (`make PROF=1`, NEO_PROF): DWT cycle
 * counts of the main loop's phases, printed every NEO_PROF_WINDOW frames.
 *
 *   PROF/clock: <MHz>                       once, at boot
 *   PROF/frame: frames drawn fw_skip wall_ms
 *   PROF/budget: budget busy heaviest       cycles per frame: the frame time
 *                                           at the real frame rate (neo_timing.c), the average
 *                                           work (everything but the two
 *                                           sleeps), the worst single frame's
 *   PROF/emu: z80 m68k irq audio(ym) framectl, per frame (paid every frame)
 *   PROF/vsplit: draw_screen's clear, sprites and fix layer per drawn frame
 *   PROF/tiles: sprite tiles drawn per drawn frame by path (video.c)
 *   PROF/ym: the YM2610 loop, clean and instrumented cost per call and the
 *            split (eg fm ssg adpcmb adpcma mix), see ym2610.c
 *   PROF/cpi: device cycles per emulated 68000 and Z80 instruction
 *   PROF/video: draw present, per DRAWN frame; wait vwait per frame
 *
 * Every figure is cycles of the core clock. busy / budget is the share of
 * the frame the emulation uses; drawn < frames with fw_skip = 0 would mean
 * frames lost outside the firmware's frameskip.
 */
#include "profiler.h"

#if defined(NEO_PROF) && !defined(HOST_BUILD)
#include <stdio.h>
#include "common.h"
#include "gw_core_bridge.h"
#include "neo_idle.h"
#include "neo_timing.h"

uint32_t neo_prof_acc[PROF_N];
uint32_t neo_prof_t0[PROF_N];

static uint32_t ph[NEO_PH_N];
static uint32_t last, frame_t0, frame_wait, heaviest;
static uint32_t frames, drawn, fw_skip, vskip;
static uint32_t mhz, wall_t0;

void neo_prof_init(void)
{
    *(volatile uint32_t *)0xE000EDFCu |= (1u << 24);   /* DEMCR.TRCENA */
    *(volatile uint32_t *)0xE0001000u |= 1u;           /* DWT_CTRL.CYCCNTENA */
    uint32_t t = HAL_GetTick();
    while (HAL_GetTick() == t) { }
    uint32_t c0 = NEO_PROF_NOW(), t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < 100u)
        wdog_refresh();
    mhz = (NEO_PROF_NOW() - c0 + 50000u) / 100000u;
    printf("PROF/clock: %lu MHz\n", (unsigned long)mhz);
    wall_t0 = HAL_GetTick();
    last = frame_t0 = NEO_PROF_NOW();
}

void neo_prof_mark(int phase)
{
    uint32_t now = NEO_PROF_NOW();
    uint32_t d = now - last;
    ph[phase] += d;
    if (phase == NEO_PH_WAIT || phase == NEO_PH_VWAIT)
        frame_wait += d;
    last = now;
}

void neo_prof_frame_end(int was_drawn, int was_fw_skip, int was_vskip)
{
    uint32_t now = NEO_PROF_NOW();
    uint32_t busy = now - frame_t0 - frame_wait;
    if (busy > heaviest)
        heaviest = busy;
    frame_t0 = now;
    frame_wait = 0;
    frames++;
    drawn += was_drawn ? 1u : 0u;
    fw_skip += was_fw_skip ? 1u : 0u;
    vskip += was_vskip ? 1u : 0u;
    if (frames < NEO_PROF_WINDOW)
        return;

    uint32_t n = frames, d = drawn ? drawn : 1u;
    uint32_t wall = HAL_GetTick() - wall_t0;
    uint32_t budget = (uint32_t)((uint64_t)mhz * 1000000000u / neo_timing_frame_mhz());
    uint32_t busy_sum = 0;
    for (int i = 0; i < NEO_PH_N; i++)
        if (i != NEO_PH_WAIT && i != NEO_PH_VWAIT)
            busy_sum += ph[i] / n;
    uint32_t z80 = neo_prof_acc[PROF_Z80] / n, m68k = neo_prof_acc[PROF_68K] / n;
    uint32_t emu = ph[NEO_PH_EMU] / n;
    uint32_t irq = emu > z80 + m68k ? emu - z80 - m68k : 0;

    printf("PROF/frame: frames=%lu drawn=%lu fw_skip=%lu vskip=%lu wall_ms=%lu (%lu of %lu rendered; fw_skip: the firmware's frameskip, vskip: dropped, the last swap still pending)\n",
           (unsigned long)n, (unsigned long)drawn, (unsigned long)fw_skip, (unsigned long)vskip,
           (unsigned long)wall, (unsigned long)drawn, (unsigned long)n);
    printf("PROF/budget: budget=%lu busy=%lu (%lu%%) heaviest=%lu (cycles per frame, %lu MHz)\n",
           (unsigned long)budget, (unsigned long)busy_sum,
           (unsigned long)(budget ? (uint64_t)busy_sum * 100u / budget : 0),
           (unsigned long)heaviest, (unsigned long)mhz);
    printf("PROF/emu: z80=%lu m68k=%lu irq=%lu audio=%lu (ym=%lu) framectl=%lu per frame\n",
           (unsigned long)z80, (unsigned long)m68k, (unsigned long)irq,
           (unsigned long)(ph[NEO_PH_AUDIO] / n), (unsigned long)(neo_prof_acc[PROF_SOUND] / n),
           (unsigned long)(ph[NEO_PH_FRAMECTL] / n));
    printf("PROF/video: draw=%lu present=%lu per drawn frame; wait=%lu vwait=%lu per frame\n",
           (unsigned long)(ph[NEO_PH_VIDEO] / d), (unsigned long)(ph[NEO_PH_PRESENT] / d),
           (unsigned long)(ph[NEO_PH_WAIT] / n), (unsigned long)(ph[NEO_PH_VWAIT] / n));

    {
        /* draw_screen's parts per drawn frame (video.c): clear (the DMA2D
         * fill and its wait), sprites (list + tiles), fix layer */
        extern unsigned int neo_vid_part[3], neo_vid_frames;
        uint32_t f = neo_vid_frames ? neo_vid_frames : 1u;
        printf("PROF/vsplit: clear=%lu sprites=%lu fix=%lu per drawn frame\n",
               (unsigned long)(neo_vid_part[0] / f), (unsigned long)(neo_vid_part[1] / f),
               (unsigned long)(neo_vid_part[2] / f));
        extern unsigned int neo_tile_cnt[4];
        printf("PROF/tiles: full=%lu yzoom=%lu narrow=%lu clipped=%lu per drawn frame\n",
               (unsigned long)(neo_tile_cnt[0] / f), (unsigned long)(neo_tile_cnt[1] / f),
               (unsigned long)(neo_tile_cnt[2] / f), (unsigned long)(neo_tile_cnt[3] / f));
        for (int i = 0; i < 4; i++)
            neo_tile_cnt[i] = 0;
        neo_vid_part[0] = neo_vid_part[1] = neo_vid_part[2] = neo_vid_frames = 0;
    }
    {
        /* the YM2610's sample loop (ym2610.c): every other call instrumented.
         * clean = cycles per uninstrumented call, instr = per instrumented
         * call (their difference is the counters' own price), then the
         * instrumented calls' loop split per call */
        extern unsigned int neo_ym_part[6], neo_ym_tot[2], neo_ym_calls[2];
        uint32_t c0 = neo_ym_calls[0] ? neo_ym_calls[0] : 1u, c1 = neo_ym_calls[1] ? neo_ym_calls[1] : 1u;
        printf("PROF/ym: clean=%lu instr=%lu per call (%lu+%lu calls) eg=%lu fm=%lu ssg=%lu adpcmb=%lu adpcma=%lu mix=%lu\n",
               (unsigned long)(neo_ym_tot[0] / c0), (unsigned long)(neo_ym_tot[1] / c1),
               (unsigned long)neo_ym_calls[0], (unsigned long)neo_ym_calls[1],
               (unsigned long)(neo_ym_part[0] / c1), (unsigned long)(neo_ym_part[1] / c1),
               (unsigned long)(neo_ym_part[2] / c1), (unsigned long)(neo_ym_part[3] / c1),
               (unsigned long)(neo_ym_part[4] / c1), (unsigned long)(neo_ym_part[5] / c1));
        for (int i = 0; i < 6; i++)
            neo_ym_part[i] = 0;
        neo_ym_tot[0] = neo_ym_tot[1] = neo_ym_calls[0] = neo_ym_calls[1] = 0;
    }
    {
        /* device cycles per emulated instruction (gw-arcade-core's
         * M68K_ICOUNT): the 68000 phase without the idle skip's own work
         * (arm, setup, runner), over the instructions Musashi executed;
         * the Z80 phase (with my_timer) over the Z80's */
        extern unsigned int neo_m68k_icount, neo_z80_icount;
#if NEO_IDLE_SKIP
        extern unsigned int neo_idle_t_setup, neo_idle_t_run, neo_idle_t_arm;
        uint32_t idle = neo_idle_t_setup + neo_idle_t_run + neo_idle_t_arm;
#else
        uint32_t idle = 0;
#endif
        uint32_t m = neo_prof_acc[PROF_68K] > idle ? neo_prof_acc[PROF_68K] - idle : 0;
        uint32_t mi = neo_m68k_icount ? neo_m68k_icount : 1u, zi = neo_z80_icount ? neo_z80_icount : 1u;
        printf("PROF/cpi: m68k=%lu.%lu (%lu instructions per frame) z80=%lu.%lu (%lu per frame) device cycles per instruction\n",
               (unsigned long)(m / mi), (unsigned long)((uint64_t)(m % mi) * 10u / mi),
               (unsigned long)(neo_m68k_icount / n),
               (unsigned long)(neo_prof_acc[PROF_Z80] / zi),
               (unsigned long)((uint64_t)(neo_prof_acc[PROF_Z80] % zi) * 10u / zi),
               (unsigned long)(neo_z80_icount / n));
        neo_m68k_icount = neo_z80_icount = 0;
    }
#if NEO_IDLE_SKIP
    /* the 68000 idle skip (neo_idle.c): Musashi cycles not executed per
     * frame (consumed at once or run natively), passes run natively, skips,
     * the loop armed now, arms and rejects in the window */
    {
        static unsigned int skip0, ran0, n0, arms0, rej0;
        printf("PROF/idle: skipped=%lu natively=%lu passes skips=%lu head=%06lx arms=%lu rejects=%lu per frame / window\n",
               (unsigned long)((neo_idle_skipped - skip0) / n), (unsigned long)((neo_idle_ran - ran0) / n),
               (unsigned long)(neo_idle_skips - n0), (unsigned long)(neo_idle_head & 0xffffff),
               (unsigned long)(neo_idle_arms - arms0), (unsigned long)(neo_idle_rejects - rej0));
        extern unsigned int neo_idle_t_setup, neo_idle_t_run, neo_idle_t_arm;
        printf("PROF/idle: cost arm=%lu setup=%lu runner=%lu (core cycles per frame: the analysis and the shadow pass in flash, the runner in RAM)\n",
               (unsigned long)(neo_idle_t_arm / n), (unsigned long)(neo_idle_t_setup / n),
               (unsigned long)(neo_idle_t_run / n));
        neo_idle_t_arm = neo_idle_t_setup = neo_idle_t_run = 0;
        skip0 = neo_idle_skipped; ran0 = neo_idle_ran; n0 = neo_idle_skips;
        arms0 = neo_idle_arms; rej0 = neo_idle_rejects;
    }
    {   /* the Z80 idle skip (mamez80/z80.c): T-states consumed at once per
         * frame (of the Z80's 73,333), skips, the loop armed now */
        extern unsigned int neo_z80i_head, neo_z80i_skipped, neo_z80i_skips, neo_z80i_arms, neo_z80i_rejects;
        static unsigned int zs0, zn0, za0, zr0;
        printf("PROF/z80idle: skipped=%lu T-states per frame, skips=%lu head=%04lx arms=%lu rejects=%lu per window\n",
               (unsigned long)((neo_z80i_skipped - zs0) / n), (unsigned long)(neo_z80i_skips - zn0),
               (unsigned long)(neo_z80i_head & 0xffff), (unsigned long)(neo_z80i_arms - za0),
               (unsigned long)(neo_z80i_rejects - zr0));
        zs0 = neo_z80i_skipped; zn0 = neo_z80i_skips; za0 = neo_z80i_arms; zr0 = neo_z80i_rejects;
    }
#endif
    for (int i = 0; i < NEO_PH_N; i++)
        ph[i] = 0;
    for (int i = 0; i < PROF_N; i++)
        neo_prof_acc[i] = 0;
    frames = drawn = fw_skip = vskip = heaviest = 0;
    wall_t0 = HAL_GetTick();
    /* the printing above is charged to nobody */
    last = frame_t0 = NEO_PROF_NOW();
}
#endif
