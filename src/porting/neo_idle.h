#ifndef NEO_IDLE_H
#define NEO_IDLE_H

/* generic 68000 idle-loop skip (src/porting/neo_idle.c) */
#ifndef NEO_IDLE_SKIP
#define NEO_IDLE_SKIP 1
#endif

#define NEO_IDLE_NONE 0xffffffffu
#define NEO_IDLE_MAXB 64u            /* the longest loop considered, bytes */

#if NEO_IDLE_SKIP
/* hot state, read by m68k_run's head compare (m68kcpu.c) */
extern unsigned int neo_idle_head, neo_idle_lap, neo_idle_last;
extern unsigned int neo_idle_skipped, neo_idle_skips, neo_idle_ran, neo_idle_bug;
extern unsigned int neo_idle_arms, neo_idle_rejects, neo_idle_log;

void neo_idle_reset(void);
/* after each 68000 slice: arm the loop the slice ended in, if it is one */
void neo_idle_slice_end(void);
/* the head's first time round after arming: trace one pass (sets the
 * pass length neo_idle_lap, 0 if it cannot) */
void neo_idle_trace(void);
/* up to that many passes from the head: the passes taken (state committed) */
unsigned int neo_idle_run(unsigned int laps);
#endif

#endif
