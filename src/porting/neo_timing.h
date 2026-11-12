#ifndef NEO_TIMING_H
#define NEO_TIMING_H

#include <stdint.h>

/*
 * The Neo Geo's frame: 264 lines of 384 dots at 6 MHz. Per frame the
 * 68000 (12 MHz) runs 202,752 cycles and the Z80 (4 MHz) 67,584 T-states;
 * on an MVS (24 MHz crystal) a frame lasts 202,752 / 12 MHz = 1/59.1856 s.
 * The AES's 24.167829 MHz crystal speeds the whole machine up by 0.7%
 * (59.599 Hz): the same cycles per frame, a shorter frame. The emulated
 * machine is therefore the same for both Systems; only the speed it is
 * played at differs (neo_timing.c).
 */
#define NEO_M68K_CYCLES_PER_FRAME 202752u
#define NEO_Z80_CYCLES_PER_FRAME  67584u
#define NEO_FRAME_SECONDS         (202752.0 / 12000000.0)

/* the YM2610's own ADPCM-A rate, 8 MHz / 432 = 18518.5 Hz (as the
 * firmware's odroid_audio_init knows it), and the rate every firmware has */
#define NEO_AUDIO_RATE_NATIVE 18518
#define NEO_AUDIO_RATE_SAFE   18000

/* once, after odroid_system_init(APP_ID, NEO_AUDIO_RATE_NATIVE): keep the
 * native rate if the firmware really plays it, else switch to 18000 Hz */
void neo_timing_probe(void);
/* samples per frame, frame time and LCD refresh for the current System;
 * at start-up and when the System changes (then restart the audio) */
void neo_timing_apply(void);
/* the output rate (NEO_AUDIO_RATE_NATIVE or _SAFE) and the samples per
 * frame (one audio half-buffer) */
int neo_timing_rate(void);
int neo_timing_spf(void);
/* the frame rate the loop runs at, in milli-Hz (rate / samples per frame) */
uint32_t neo_timing_frame_mhz(void);

#endif
