#ifndef _PROFILER_H_
#define _PROFILER_H_

/*
 * Device profiling: `make PROF=1` (NEO_PROF) times the emulator's phases
 * with the Cortex-M7 DWT cycle counter and prints PROF/ lines every
 * NEO_PROF_WINDOW frames (src/porting/neo_prof.c). Without it every probe
 * compiles to nothing.
 *
 * PROFILER_START/STOP time the gngeo phases (accumulated per window);
 * NEO_PROF_MARK(phase) charges the time since the previous mark to one of
 * the main loop's phases (src/main_neogeo.c).
 */

#define PROF_ALL    0
#define PROF_VIDEO  1
#define PROF_68K    2
#define PROF_Z80    3
#define PROF_SOUND  4
#define PROF_N      5

/* the main loop's phases, in loop order */
enum {
    NEO_PH_FRAMECTL,   /* common_emu_frame_loop + input + pause menu */
    NEO_PH_EMU,        /* neo_run_frame: Z80 slices, 68000, the VBL IRQ */
    NEO_PH_AUDIO,      /* submit_audio: the YM2610 stream + volume */
    NEO_PH_WAIT,       /* common_emu_sound_sync: sleeping for the audio DMA */
    NEO_PH_VWAIT,      /* the LCD buffer handed to the renderer (no wait any more) */
    NEO_PH_VIDEO,      /* draw_screen */
    NEO_PH_PRESENT,    /* row move + letterbox + overlay + lcd_swap */
    NEO_PH_N
};

#if defined(NEO_PROF) && !defined(HOST_BUILD)
#include <stdint.h>

#define NEO_PROF_WINDOW 120u
#define NEO_PROF_NOW()  (*(volatile uint32_t *)0xE0001004u)   /* DWT_CYCCNT */

extern uint32_t neo_prof_acc[PROF_N];
extern uint32_t neo_prof_t0[PROF_N];

#define PROFILER_START(x) (neo_prof_t0[(x)] = NEO_PROF_NOW())
#define PROFILER_STOP(x)  (neo_prof_acc[(x)] += NEO_PROF_NOW() - neo_prof_t0[(x)])

void neo_prof_init(void);
void neo_prof_mark(int phase);
/* end of a main-loop iteration: drawn = the frame was drawn and presented,
 * fw_skip = common_emu_frame_loop asked to skip it, vskip = its picture was
 * dropped because the previous swap was still pending */
void neo_prof_frame_end(int drawn, int fw_skip, int vskip);

#define NEO_PROF_INIT()            neo_prof_init()
#define NEO_PROF_MARK(p)           neo_prof_mark(p)
#define NEO_PROF_FRAME_END(d, s, v) neo_prof_frame_end((d), (s), (v))

#else

#define PROFILER_START(x) ((void)0)
#define PROFILER_STOP(x)  ((void)0)
#define NEO_PROF_INIT()            ((void)0)
#define NEO_PROF_MARK(p)           ((void)0)
#define NEO_PROF_FRAME_END(d, s, v) ((void)0)

#endif

static inline void profiler_show_stat(void) {}

#endif
