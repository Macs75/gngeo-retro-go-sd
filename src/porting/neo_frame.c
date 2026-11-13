/* Single-frame Neo Geo step for Retro-Go main loop (replaces main_loop). */
#include "emu.h"
#include "memory.h"
#include "video.h"
#include "sound.h"
#include "timer.h"
#include "pd4990a.h"
#include "frame_skip.h"
#include "conf.h"
#include "profiler.h"
#include "gngeo_platform.h"
#include "config.h"
#include "neo_timing.h"
#include "neo_idle.h"

#ifndef NEO_DISABLE_VIDEO
#define NEO_DISABLE_VIDEO 0
#endif

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#endif

extern int nb_interlace;
extern char skip_next_frame;

static int fc;
static int skip_this_frame;
static int want_draw;
static Uint32 tm_cycle;

/* the loop's own state as at power-on (neo_rec_power_on) */
void neo_frame_reset(void)
{
    fc = 0;
    skip_this_frame = 0;
    skip_next_frame = 0;
    want_draw = 0;
    tm_cycle = 0;
}

static int neo_frame_irq(void)
{
    /* UniBIOS hardware test / RTC need retrace ticks. */
    pd4990a_addretrace();

    if (!(memory.vid.irq2control & 0x8)) {
        if (fc >= (int)neogeo_frame_counter_speed) {
            fc = 0;
            neogeo_frame_counter++;
        }
        fc++;
    }

    skip_this_frame = skip_next_frame;
    skip_next_frame = frame_skip(0);
    /* draw_screen is deferred to neo_draw_frame() so submit_audio can run
     * first — a slow blit must not starve the DMA half-buffer. */
    return 1;
}

/*
 * The Z80 kept in step with the 68000. The frame runs the 68000, and the
 * Z80 catches up to the 68000's time (12 MHz / 4 MHz: a third of its
 * cycles) whenever the 68000 writes a sound command or reads the reply at
 * $320000 (neo_z80_sync, from memory.c), then runs the rest of its frame.
 * The YM2610 timers tick at the same NEO_Z80_SLICES boundaries as before.
 * The Z80 used to run its whole frame first: a game that waits for the
 * reply (s1945p: up to 10,000 polls after every sound command) spun to the
 * end of the 68000's frame, ~half its cycles, and dropped game frames.
 */
uint32_t cpu_68k_slice_cycles(void);

static uint32_t z80_done, z80_slice_len, z80_frame_len, m68k_carry;
static int z80_slice, z80_in_frame;

static void z80_run_to(uint32_t target)
{
    if (target > z80_frame_len)
        target = z80_frame_len;
    while (z80_slice < nb_interlace) {
        uint32_t end = (uint32_t)(z80_slice + 1) * z80_slice_len;
        if (z80_done < end) {
            uint32_t to;
            if (z80_done >= target)
                return;
            to = target < end ? target : end;
            z80_done += (uint32_t)cpu_z80_run((int)(to - z80_done));
            if (z80_done < end)
                return;               /* reached the target inside the slice */
        }
        my_timer();
        z80_slice++;
#ifndef HOST_BUILD
        if ((z80_slice & 7) == 0)
            wdog_refresh();
#endif
    }
}

void neo_z80_sync(void)
{
    if (!z80_in_frame)
        return;
    PROFILER_STOP(PROF_68K);
    PROFILER_START(PROF_Z80);
    z80_run_to((m68k_carry + cpu_68k_slice_cycles()) / 3u);
    PROFILER_STOP(PROF_Z80);
    PROFILER_START(PROF_68K);
}

void neo_run_frame(int draw_video)
{
    int a;
    Uint32 cpu_68k_timeslice = NEO_M68K_CYCLES_PER_FRAME;
    int sound = neo_sound_gen_enabled();

    want_draw = draw_video && !NEO_DISABLE_VIDEO;

    if (conf.test_switch == 1)
        conf.test_switch = 0;

    z80_done = 0;
    z80_slice = 0;
    z80_frame_len = NEO_Z80_CYCLES_PER_FRAME;
    z80_slice_len = z80_frame_len / (Uint32)nb_interlace;
    m68k_carry = tm_cycle;            /* run by the last frame's slice already */
    z80_in_frame = sound;

    PROFILER_START(PROF_68K);
    tm_cycle = cpu_68k_run(cpu_68k_timeslice - tm_cycle);
    PROFILER_STOP(PROF_68K);
    z80_in_frame = 0;

    if (sound) {
        PROFILER_START(PROF_Z80);
        z80_run_to(z80_frame_len);
        PROFILER_STOP(PROF_Z80);
    }
    a = neo_frame_irq();

    memory.watchdog++;
    if (memory.watchdog > 60) {
        memory.watchdog = 0;
        cpu_68k_reset();
    }

    if (a)
        cpu_68k_interrupt(a);
#ifdef HOST_BUILD
    neo_idlestat_frame();
#endif
}

int neo_frame_needs_draw(void)
{
    return want_draw && !skip_this_frame && !NEO_DISABLE_VIDEO;
}

void neo_draw_frame(void)
{
    if (!neo_frame_needs_draw())
        return;
    PROFILER_START(PROF_VIDEO);
    draw_screen();
    PROFILER_STOP(PROF_VIDEO);
}
