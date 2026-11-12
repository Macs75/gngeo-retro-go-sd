/*
 * neo_rec.c -- input record / replay, for repeatable sessions: the same
 * frames for a PROF A/B, and the state hashes that prove an optimisation
 * did not change the emulation.
 *
 * A recording is the Neo Geo input of every emulated frame, AFTER the G&W
 * button mapping (Swap A/B, Swap C/D), so a mapping change does not break
 * it, from a power-on (neo_rec_power_on), in "<rom path without
 * extension>.rec" next to the ROM:
 *   header (16 bytes): "NGRC", version 2, flags (bit 0 MVS, bits 1-2 the
 *   region, bit 3 audio at the native 18518 Hz instead of 18000: the
 *   samples per frame, neo_timing.c), FNV-1a of the game name and the first 64 KiB of the 68000
 *   program, frame count (0 until the recording is closed); then one
 *   little-endian uint16 per frame: P1 buttons (A 0x01, B 0x02, C 0x04,
 *   D 0x08, up 0x10, down 0x20, left 0x40, right 0x80), START 0x100,
 *   SELECT/coin 0x200.
 * Record or replay from the pause menu (device and host) or NEO_RECORD=1 /
 * NEO_REPLAY=1 at start-up (host). Every 120 frames both log
 *   REC/hash frame=N ram vram pal zram cpu audio
 * hashes of the 68000 work RAM, video RAM, palettes, Z80 RAM, both CPUs'
 * registers and the audio output so far: two runs of one recording must
 * log the same lines (host and device alike).
 *
 * The power-on resets every piece of machine state, including what the
 * savestate does not carry (the YM timers, the frame and auto-animation
 * counters, the 68000's cycle carry), clears the backup RAM and the memory
 * card, and runs the calendar chip from a fixed date instead of the real
 * clock. A replay needs the System and Region it was recorded with.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include "neo_rec.h"
#include "memory.h"
#include "video.h"
#include "emu.h"
#include "conf.h"
#include "timer.h"
#include "pd4990a.h"
#include "event.h"
#include "gno_flash.h"
#include "neo_settings.h"
#include "neo_timing.h"
#include "neo_pvc.h"
#undef READ_WORD                 /* memory.h's; m68k/macros.h has its own */
#undef WRITE_WORD
#undef READ_BYTE
#undef WRITE_BYTE
#include "m68k/m68k.h"
#include "mamez80/z80.h"
#include "ym2610/ym2610.h"
#include "gw_malloc.h"
#include "neo_state.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#endif

#define REC_MAGIC   0x4352474eu      /* "NGRC" */
#define REC_VERSION 2u          /* 2: the real frame timing (neo_timing.h) */
#define REC_BUF     1024u

typedef struct {
    uint32_t magic;
    uint16_t version, flags;
    uint32_t rom_hash, frames;
} rec_header_t;

/* from the emulator, reset at power-on */
extern double timer_count;
extern Uint32 bankaddress;
extern int pd4990a_fixed_clock;
void cpu_z80_init(void);
void neo_frame_reset(void);

static int rec_mode;
static FILE *rec_f;
static uint32_t rec_frame, rec_rom_hash, rec_audio_h;
#ifdef HOST_BUILD
/* host: every presented frame folded in (host_emu.c lcd_swap), printed as
 * pic= on the REC/hash lines -- the renderer's output, which the machine
 * state hashes cannot see */
uint32_t neo_rec_pic_h = 2166136261u;
#endif
static uint16_t *rec_buf;            /* AHB heap at the first use */
static uint32_t rec_n, rec_pos;
static char rec_file[300];

static uint32_t fnv(const void *p, uint32_t n, uint32_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}
#define FNV0 2166136261u

int neo_rec_mode(void) { return rec_mode; }

static uint16_t rec_flags(void)
{
    return (uint16_t)((neo_settings_hw_get() == NEO_HW_MVS ? 1u : 0u) |
                      ((unsigned)neo_settings_region_get() << 1) |
                      (neo_timing_rate() == NEO_AUDIO_RATE_NATIVE ? 8u : 0u));
}

static uint32_t rom_hash(void)
{
    uint32_t h = FNV0;
    if (conf.game)
        h = fnv(conf.game, (uint32_t)strlen(conf.game), h);
    if (memory.rom.cpu_m68k.p) {
        uint32_t n = memory.rom.cpu_m68k.size < 0x10000 ? memory.rom.cpu_m68k.size : 0x10000;
        h = fnv(memory.rom.cpu_m68k.p, n, h);
    }
    return h;
}

void neo_rec_power_on(void)
{
    VIDEO *v = &memory.vid;

    /* the board: every RAM, the video registers, the palettes in use */
    memset(memory.ram, 0, sizeof(memory.ram));
    memset(memory.z80_ram, 0, sizeof(memory.z80_ram));
    if (memory.sram)
        memset(memory.sram, 0, 0x10000);
    if (memory.memcard)
        memset(memory.memcard, 0, 0x800);
    memset(v, 0, offsetof(VIDEO, spr_cache));
    v->modulo = 1;
    current_pal = v->pal_neo[0];
    current_pc_pal = v->pal_host[0];
    current_fix = memory.rom.bios_sfix.p;
    fix_usage = memory.fix_board_usage;
    neogeo_frame_counter = 0;
    neogeo_frame_counter_speed = 8;
    memory.watchdog = 0;

    /* sound: the YM2610, its timers, the Z80 */
    free_all_timer();
    timer_count = 0;
    YM2610Reset();
    cpu_z80_init();

    /* the calendar chip from a fixed date, the frame loop's counters */
    pd4990a_fixed_clock = 1;
    pd4990a_init();
    neo_mslugx_prot_reset();
    neo_frame_reset();

    /* as the System/Region options' reset: BIOS prefs, vectors, 68000.
     * A 68000 reset loads SSP and PC only: zero the rest of the registers,
     * or the previous session's values linger until the BIOS overwrites
     * them (a replay started mid-session hashed differently at frame 120) */
    if (m68k_core) {
        memset(m68k.dar, 0, sizeof(m68k.dar));
        memset(m68k.sp, 0, sizeof(m68k.sp));
    }
    neo_settings_apply_unibios();
    neo_vector_use_bios();
    neogeo_reset();

    rec_frame = 0;
    rec_audio_h = FNV0;
#ifdef HOST_BUILD
    neo_rec_pic_h = FNV0;
#endif
    rec_n = rec_pos = 0;
}

static void rec_flush(void)
{
    if (rec_f && rec_n)
        fwrite(rec_buf, sizeof(uint16_t), rec_n, rec_f);
    rec_n = 0;
}

void neo_rec_close(void)
{
    if (!rec_f) {
        rec_mode = NEO_REC_OFF;
        return;
    }
    if (rec_mode == NEO_REC_RECORD) {
        rec_flush();
        rec_header_t h = { REC_MAGIC, REC_VERSION, rec_flags(), rec_rom_hash, rec_frame };
        fseek(rec_f, 0, SEEK_SET);
        fwrite(&h, sizeof h, 1, rec_f);
        printf("REC: recorded %lu frames in %s\n", (unsigned long)rec_frame, rec_file);
    } else {
        printf("REC: replay stopped at frame %lu\n", (unsigned long)rec_frame);
    }
    fclose(rec_f);
    rec_f = NULL;
    rec_mode = NEO_REC_OFF;
    pd4990a_fixed_clock = 0;
}

static void rec_path(char *out, size_t cap, const char *rom_path)
{
    snprintf(out, cap, "%s", rom_path ? rom_path : "neogeo");
    char *dot = strrchr(out, '.'), *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    strncat(out, ".rec", cap - strlen(out) - 1);
}

int neo_rec_start(int mode, const char *rom_path)
{
    neo_rec_close();
    if (!rec_buf)
        rec_buf = (uint16_t *)ahb_calloc(REC_BUF, sizeof(uint16_t));
    if (!rec_buf)
        rec_buf = (uint16_t *)ram_calloc(REC_BUF, sizeof(uint16_t));
    if (!rec_buf) {
        printf("REC: no memory for the input buffer\n");
        return 0;
    }
    rec_path(rec_file, sizeof rec_file, rom_path);
    rec_rom_hash = rom_hash();
    if (mode == NEO_REC_RECORD) {
        rec_f = fopen(rec_file, "wb");
        if (!rec_f) {
            printf("REC: cannot create %s\n", rec_file);
            return 0;
        }
        rec_header_t h = { REC_MAGIC, REC_VERSION, rec_flags(), rec_rom_hash, 0 };
        fwrite(&h, sizeof h, 1, rec_f);
    } else {
        rec_header_t h;
        rec_f = fopen(rec_file, "rb");
        if (!rec_f) {
            printf("REC: no recording %s\n", rec_file);
            return 0;
        }
        if (fread(&h, sizeof h, 1, rec_f) != 1 || h.magic != REC_MAGIC || h.version != REC_VERSION) {
            printf("REC: %s is not a version %u recording (older ones predate the real frame timing: record again)\n",
                   rec_file, (unsigned)REC_VERSION);
            fclose(rec_f);
            rec_f = NULL;
            return 0;
        }
        if (h.rom_hash != rec_rom_hash) {
            printf("REC: %s was recorded with another game (%08lx, this one %08lx)\n", rec_file,
                   (unsigned long)h.rom_hash, (unsigned long)rec_rom_hash);
            fclose(rec_f);
            rec_f = NULL;
            return 0;
        }
        if (h.flags != rec_flags()) {
            printf("REC: %s was recorded with System %s, Region %u, audio %d Hz: set them first\n",
                   rec_file, (h.flags & 1) ? "MVS" : "AES", (unsigned)((h.flags >> 1) & 3),
                   (h.flags & 8) ? NEO_AUDIO_RATE_NATIVE : NEO_AUDIO_RATE_SAFE);
            fclose(rec_f);
            rec_f = NULL;
            return 0;
        }
        printf("REC: replaying %lu frames from %s\n", (unsigned long)h.frames, rec_file);
    }
    rec_mode = mode;
    neo_rec_power_on();
    printf("REC: %s from power-on, %s, region %u, game %08lx\n",
           mode == NEO_REC_RECORD ? "recording" : "replay",
           neo_settings_hw_get() == NEO_HW_MVS ? "MVS" : "AES",
           (unsigned)neo_settings_region_get(), (unsigned long)rec_rom_hash);
    return 1;
}

#ifdef HOST_BUILD
#include <stdlib.h>
/* NEO_INPUT_RAW=<file> (host): the "live" input of loop frame k is the
 * file's k-th little-endian uint16 (tools/neo_input_script.py), so a
 * recording can be made without a keyboard; 0 after its end */
static uint16_t host_scripted(uint16_t live)
{
    static FILE *f;
    static int tried;
    uint16_t w = 0;
    if (!tried) {
        const char *e = getenv("NEO_INPUT_RAW");
        tried = 1;
        if (e && e[0] && !(f = fopen(e, "rb")))
            printf("REC: cannot open NEO_INPUT_RAW %s\n", e);
    }
    if (!f)
        return live;
    if (fread(&w, sizeof w, 1, f) != 1)
        w = 0;
    return w;
}
#endif

uint16_t neo_rec_input(uint16_t live)
{
#ifdef HOST_BUILD
    live = host_scripted(live);
#endif
    if (rec_mode == NEO_REC_RECORD) {
        rec_buf[rec_n++] = live;
        if (rec_n == REC_BUF)
            rec_flush();
        return live;
    }
    if (rec_mode == NEO_REC_REPLAY) {
        if (rec_pos == rec_n) {
            rec_n = (uint32_t)fread(rec_buf, sizeof(uint16_t), REC_BUF, rec_f);
            rec_pos = 0;
            if (!rec_n) {
                printf("REC: replay finished at frame %lu, live input from here\n",
                       (unsigned long)rec_frame);
                neo_rec_close();
                return live;
            }
        }
        return rec_buf[rec_pos++];
    }
    return live;
}

static uint32_t cpu_hash(void)
{
    uint32_t h = FNV0;
    if (m68k_core) {
        h = fnv(m68k.dar, sizeof(m68k.dar), h);
        h = fnv(&m68k.pc, sizeof(m68k.pc), h);
        h = fnv(m68k.sp, sizeof(m68k.sp), h);
        h = fnv(&m68k.x_flag, sizeof(m68k.x_flag), h);
        h = fnv(&m68k.n_flag, sizeof(m68k.n_flag), h);
        h = fnv(&m68k.not_z_flag, sizeof(m68k.not_z_flag), h);
        h = fnv(&m68k.v_flag, sizeof(m68k.v_flag), h);
        h = fnv(&m68k.c_flag, sizeof(m68k.c_flag), h);
        h = fnv(&m68k.int_mask, sizeof(m68k.int_mask), h);
    }
    h = fnv(z80_stateData(), z80_stateDataSize, h);
    return h;
}

void neo_rec_frame_done(const int16_t *audio, int n)
{
    if (rec_mode == NEO_REC_OFF)
        return;
    if (audio && n > 0)
        rec_audio_h = fnv(audio, (uint32_t)n * sizeof(int16_t), rec_audio_h);
    rec_frame++;
#ifdef HOST_BUILD
    {   /* NEO_STATE_TEST=N (host): save a state at frame N and load it back
         * at once -- the run's REC lines must not change (the format holds
         * the whole machine) */
        static long st_at = -2;
        if (st_at == -2) { const char *e = getenv("NEO_STATE_TEST"); st_at = e ? atol(e) : -1; }
        if (st_at >= 0 && rec_frame == (uint32_t)st_at) {
            int okw = neo_save_state_path("host_saves_statetest.sav");
            int okr = okw && neo_load_state_path("host_saves_statetest.sav");
            printf("REC: state test at frame %lu: save %s, load %s\n", (unsigned long)rec_frame,
                   okw ? "ok" : "FAILED", okr ? "ok" : "FAILED");
        }
    }
    static unsigned int every;
    if (!every) {   /* NEO_HASH_EVERY=N (host): a REC/hash line every N frames */
        const char *e = getenv("NEO_HASH_EVERY");
        every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 120u;
    }
    if (rec_frame % every)
        return;
#else
    if (rec_frame % 120u)
        return;
#endif
#ifdef HOST_BUILD
    printf("REC/pic frame=%lu pic=%08lx\n", (unsigned long)rec_frame, (unsigned long)neo_rec_pic_h);
#endif
    /* video RAM: the LSPC's 0x8800 words (VRAM_BYTES, memory.h) */
    printf("REC/hash frame=%lu ram=%08lx vram=%08lx pal=%08lx zram=%08lx cpu=%08lx audio=%08lx\n",
           (unsigned long)rec_frame,
           (unsigned long)fnv(memory.ram, sizeof(memory.ram), FNV0),
           (unsigned long)fnv(memory.vid.ram, sizeof(memory.vid.ram), FNV0),
           (unsigned long)fnv(memory.vid.pal_neo, sizeof(memory.vid.pal_neo), FNV0),
           (unsigned long)fnv(memory.z80_ram, sizeof(memory.z80_ram), FNV0),
           (unsigned long)cpu_hash(), (unsigned long)rec_audio_h);
}
