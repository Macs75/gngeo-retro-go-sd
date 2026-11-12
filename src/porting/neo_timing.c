/*
 * neo_timing.c -- the Neo Geo's real frame rate on the G&W.
 *
 * The main loop is paced by the audio DMA, one half-buffer per frame, so
 * the frame rate is the output rate / the samples per frame. The output
 * rate is the YM2610's own ADPCM-A rate, 8 MHz / 432 = 18518.5 Hz, when
 * the firmware has it (set_audio_frequency's NEOGEO_AUDIO_FREQ), else
 * 18000 Hz. An unknown rate does not fail in the firmware: it plays its
 * 48K-mode clocks, a wrong rate. So the probe reads back the rate the SAI
 * really runs at from the PLL2 and SAI registers.
 *
 * The samples per frame are the nearest integer to the rate / the
 * System's frame rate (neo_timing.h), and the LCD runs at the resulting
 * frame rate (milli-Hz):
 *   18518.5 Hz: MVS 313 (59.165 Hz, real 59.1856), AES 311 (59.545, real 59.599)
 *   18000 Hz:   MVS 304 (59.211 Hz),               AES 302 (59.603)
 * The samples per frame decide how much YM2610 output a frame generates,
 * so a recording is tied to them (neo_rec.c's flags).
 *
 * Host: NEO_RATE=18000 plays the fallback (a firmware without the rate).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "gw_lcd.h"
#include "odroid_system.h"
#include "neo_settings.h"
#include "neo_timing.h"
#ifdef HOST_BUILD
#include "host_compat.h"
#else
#include "gw_core_bridge.h"
#include "stm32h7xx_hal.h"
#endif

#define NATIVE_MHZ 18518519u            /* 8 MHz / 432, in milli-Hz */
#define MVS_FPS_MHZ 59186u              /* 12 MHz / 202,752 */
#define AES_FPS_MHZ 59599u

static int rate = NEO_AUDIO_RATE_SAFE, spf = 304;
static uint32_t rate_mhz = NEO_AUDIO_RATE_SAFE * 1000u;
static uint32_t frame_mhz = NEO_AUDIO_RATE_SAFE * 1000u / 304u;

int neo_timing_rate(void) { return rate; }
int neo_timing_spf(void) { return spf; }
uint32_t neo_timing_frame_mhz(void) { return frame_mhz; }

#ifndef HOST_BUILD
/* the SAI's sample rate in milli-Hz from PLL2 and the SAI divider (0: a
 * configuration this does not decode) */
static uint32_t sai_rate_mhz(void)
{
    uint32_t ck = RCC->PLLCKSELR, divr = RCC->PLL2DIVR, cr1 = SAI1_Block_A->CR1;
    uint32_t src, m, n, p, frac = 0, div, osr;

    if ((ck & RCC_PLLCKSELR_PLLSRC) != RCC_PLLCKSELR_PLLSRC_HSI || (cr1 & SAI_xCR1_NODIV))
        return 0;
    src = HSI_VALUE >> ((RCC->CR & RCC_CR_HSIDIV) >> RCC_CR_HSIDIV_Pos);
    m = (ck & RCC_PLLCKSELR_DIVM2) >> RCC_PLLCKSELR_DIVM2_Pos;
    n = ((divr & RCC_PLL2DIVR_N2) >> RCC_PLL2DIVR_N2_Pos) + 1;
    p = ((divr & RCC_PLL2DIVR_P2) >> RCC_PLL2DIVR_P2_Pos) + 1;
    if (RCC->PLLCFGR & RCC_PLLCFGR_PLL2FRACEN)
        frac = (RCC->PLL2FRACR & RCC_PLL2FRACR_FRACN2) >> RCC_PLL2FRACR_FRACN2_Pos;
    div = (cr1 & SAI_xCR1_MCKDIV) >> SAI_xCR1_MCKDIV_Pos;
    if (!div)
        div = 1;
    osr = (cr1 & SAI_xCR1_OSR) ? 2 : 1;
    if (!m)
        return 0;
    return (uint32_t)((uint64_t)src * (n * 8192u + frac) * 1000u /
                      ((uint64_t)m * 8192u * p * div * 256u * osr));
}
#endif

void neo_timing_probe(void)
{
#ifdef HOST_BUILD
    rate = getenv("NEO_RATE") && atoi(getenv("NEO_RATE")) == NEO_AUDIO_RATE_SAFE ?
           NEO_AUDIO_RATE_SAFE : NEO_AUDIO_RATE_NATIVE;
    rate_mhz = rate == NEO_AUDIO_RATE_NATIVE ? NATIVE_MHZ : (uint32_t)rate * 1000u;
    odroid_audio_init(rate);
#else
    uint32_t got = sai_rate_mhz();
    if (got > NATIVE_MHZ - 10000u && got < NATIVE_MHZ + 10000u) {
        rate = NEO_AUDIO_RATE_NATIVE;
        rate_mhz = got;
    } else {
        printf("neogeo: no %d Hz audio in this firmware (SAI at %lu mHz): %d Hz\n",
               NEO_AUDIO_RATE_NATIVE, (unsigned long)got, NEO_AUDIO_RATE_SAFE);
        rate = NEO_AUDIO_RATE_SAFE;
        odroid_audio_init(rate);
        got = sai_rate_mhz();
        rate_mhz = (got > 17900000u && got < 18100000u) ? got : (uint32_t)rate * 1000u;
    }
#endif
}

void neo_timing_apply(void)
{
    uint32_t fps = neo_settings_hw_get() == NEO_HW_MVS ? MVS_FPS_MHZ : AES_FPS_MHZ;

    spf = (int)((rate_mhz + fps / 2) / fps);
    frame_mhz = (uint32_t)(((uint64_t)rate_mhz * 1000u / (uint32_t)spf + 500u) / 1000u);
    common_emu_state.frame_time_10us =
        (uint16_t)(((uint64_t)spf * 100000000u + rate_mhz / 2) / rate_mhz);
    lcd_set_refresh_rate(frame_mhz);
    printf("neogeo: %s, audio %lu.%03lu Hz, %d samples/frame: %lu.%03lu Hz\n",
           neo_settings_hw_get() == NEO_HW_MVS ? "MVS" : "AES",
           (unsigned long)(rate_mhz / 1000u), (unsigned long)(rate_mhz % 1000u), spf,
           (unsigned long)(frame_mhz / 1000u), (unsigned long)(frame_mhz % 1000u));
}
