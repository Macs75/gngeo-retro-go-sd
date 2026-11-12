/*
 * Neo Geo (GnGeo) dynamic core for Retro-Go SD.
 * Entry: app_main_neogeo
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#ifdef HOST_BUILD
#include <stdlib.h>
#endif

#include "common.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "rom_manager.h"
#include "odroid_system.h"
#include "appid.h"
#include "odroid_overlay.h"
#include "odroid_settings.h"
#include "gw_malloc.h"
#include "neo_mem.h"
#include "neo_pvc.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#include "gw_core_i18n.h"
#else
#include "host_compat.h"
#include "gw_core_i18n.h"
#endif

#include "config.h"
#include "emu.h"
#include "roms.h"
#include "screen.h"
#include "memory.h"
#include "conf.h"
#include "event.h"
#include "gnutil.h"
#include "gngeo_platform.h"
#include "gno_flash.h"
#include "neogeo_i18n.h"
#include "video.h"
#include "neo_state.h"
#include "neo_flash_ro.h"
#include "neo_settings.h"
#include "ym2610/ym2610.h"
#include "profiler.h"
#include "neo_rec.h"
#include "neo_timing.h"

extern void (**m68ki_instruction_jump_table)(void);

#ifndef NEO_DISABLE_VIDEO
#define NEO_DISABLE_VIDEO 0
#endif

#define APP_ID       APPID_CORE

static odroid_gamepad_state_t pad;
static const char *boot_fail_reason = NULL;

static bool LoadState(const char *path)
{
    neo_rec_close();                 /* a loaded state ends a record/replay */
    return neo_load_state_path(path) != 0;
}

static bool SaveState(const char *path)
{
    return neo_save_state_path(path) != 0;
}

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    if (lcd_get_active_buffer())
        neo_present_to_lcd(lcd_get_active_buffer());
    return lcd_get_active_buffer();
}

static void neo_repaint(void)
{
#ifndef HOST_BUILD
    neo_bind_lcd_buffer();
#endif
    draw_screen();
    neo_present_to_lcd(lcd_get_active_buffer());
    common_ingame_overlay();
}

/* Like gwenesis: boost to OC lvl 2 only when the launcher setting is stock
 * (0). Do not override OC 1/2/3 the user already chose. Not persisted. */
static void neo_apply_cpu_clock(void)
{
    if (odroid_settings_cpu_oc_level_get() == 0)
        SystemClock_Config(2);
}

/* Hard restart SAI/DMA. Needed after long silent gaps (ROM load) and after
 * SystemClock_Config (audio PLL). audio_start_playing alone can no-op if SAI
 * is still BUSY from a previous half-buffer.
 *
 * PHASE: the LCD runs at the audio-paced frame rate (neo_timing.c; the
 * two drift apart by about one scan line in 19 minutes), so the point of
 * the scan where each frame's audio wait ends -- and the loop draws and
 * swaps -- is fixed for the whole session, set by the moment the DMA
 * starts. A swap only shows at the start of vertical blanking (VBR
 * reload, scan line 248 of 256: 8 sync/back-porch lines, 240 active, 8
 * front porch). If the draw straddles that line, every swap misses it, is
 * still pending at the next frame's check, and every other picture is
 * dropped for the whole session (30 fps).
 *
 * So the DMA starts at scan line NEO_SYNC_LINE, a little after the
 * blanking: the check then comes 40 lines (2.6 ms) after the previous
 * swap showed, and the draw has 216 lines (14 ms) to swap before the next
 * blanking. NOT right after lcd_wait_for_vblank(): its line interrupt is
 * at line 239, 9 lines before the reload, which left the draw 0.6 ms
 * (heavy scenes then ran at 30 fps). gw-arcade-core can start there
 * because it emulates before it draws, so its swaps land mid-frame.
 * The first wait lets a re-programmed PLL3 (neo_timing_apply) settle. */
#define NEO_SYNC_LINE 32u
static void neo_audio_start(void)
{
    audio_stop_playing();
    odroid_audio_init(neo_timing_rate());
    audio_clear_buffers();
    lcd_wait_for_vblank();
    lcd_wait_for_vblank();               /* the line 239 interrupt */
#ifndef HOST_BUILD
    for (;;) {                           /* through the blanking to NEO_SYNC_LINE */
        uint32_t y = lcd_get_pixel_position() & 0xffffu;   /* LTDC CPSR.CYPOS */
        if (y >= NEO_SYNC_LINE && y < 239u)
            break;
    }
#endif
    audio_start_playing((uint16_t)neo_timing_spf());
    if (common_emu_state.pause_after_frames == 0)
        odroid_audio_mute(false);
}

#ifndef HOST_BUILD
/* PHASE watchdog: the start-up alignment (neo_audio_start) holds only as
 * long as nothing else restarts the audio DMA or re-programs the LCD clock
 * (the firmware's pause menu only mutes; its sleep/power paths stop the
 * DMA, then SleepWake realigns). Rather than trust every path, watch the
 * symptom: when 20 or more of 60 pictures are dropped because the last
 * swap is still pending, the draw straddles the LCD's vblank -- realign.
 * A restart costs ~50 ms of silence, so at most one per 600 frames (a
 * scene that drops pictures because it overruns the frame would otherwise
 * restart the audio over and over). */
#define NEO_PHASE_WINDOW    60u
#define NEO_PHASE_MAX_DROPS 20u
#define NEO_PHASE_COOLDOWN  600u
static void neo_phase_watch(int vskip)
{
    static uint16_t frames, drops, cool;

    if (cool) {
        cool--;
        return;
    }
    drops += (uint16_t)vskip;
    if (++frames < NEO_PHASE_WINDOW)
        return;
    if (drops >= NEO_PHASE_MAX_DROPS) {
        printf("neogeo: %u of %u pictures dropped (swap pending): realigning the audio to the LCD\n",
               (unsigned)drops, (unsigned)frames);
        neo_audio_start();
        cool = NEO_PHASE_COOLDOWN;
    }
    frames = drops = 0;
}
#endif

static void SleepWake(void)
{
    /* gw_sleep restores the settings OC; re-boost when we had auto-forced lvl 2.
     * Always rebuild SAI — clock restore reprograms the audio PLL even when
     * OC was already non-zero. */
    if (odroid_settings_cpu_oc_level_get() == 0)
        SystemClock_Config(2);
    neo_timing_apply();              /* the resume may have reset the LCD rate */
    neo_audio_start();               /* and restarts the DMA: realign the phase */
}

static void SramSave(void)
{
    /* TODO: write memory.sram via ODROID_PATH_SAVE_SRAM when dirty */
    neo_rec_close();                 /* quit / sleep: the recording gets its frame count */
}

static void map_input(const odroid_gamepad_state_t *joy)
{
    Uint8 buttons = 0;

    int a = joy->values[ODROID_INPUT_A];
    int b = joy->values[ODROID_INPUT_B];
    int c = joy->values[ODROID_INPUT_X];
    int d = joy->values[ODROID_INPUT_Y];

    if (neo_settings_swap_ab_get()) {
        int temp = a;
        a = b;
        b = temp;
    }

    if (neo_settings_swap_cd_get()) {
        int temp = c;
        c = d;
        d = temp;
    }

    if (a) buttons |= 0x01;
    if (b) buttons |= 0x02;
    if (c) buttons |= 0x04; /* C */
    if (d) buttons |= 0x08; /* D */
    if (joy->values[ODROID_INPUT_UP])    buttons |= 0x10;
    if (joy->values[ODROID_INPUT_DOWN])  buttons |= 0x20;
    if (joy->values[ODROID_INPUT_LEFT])  buttons |= 0x40;
    if (joy->values[ODROID_INPUT_RIGHT]) buttons |= 0x80;

    /* START + SELECT(coin/select); recorded or replayed (neo_rec.c) */
    uint16_t in = (uint16_t)(buttons |
                             (joy->values[ODROID_INPUT_START] ? 0x100 : 0) |
                             (joy->values[ODROID_INPUT_SELECT] ? 0x200 : 0));
    in = neo_rec_input(in);
    neo_set_input((Uint8)in, (in & 0x100) ? 1 : 0, (in & 0x200) ? 1 : 0);
}

static void submit_audio(void)
{
    int16_t *buf;
    uint16_t len;
    int32_t vol;
    int i;

    buf = audio_get_active_buffer();
    len = audio_get_buffer_length();
    if (!buf || !len)
        return;

    if (!neo_sound_gen_enabled()) {
        memset(buf, 0, (size_t)len * sizeof(int16_t));
        neo_rec_frame_done(buf, (int)len);
        return;
    }

    neo_render_audio(buf, (int)len);
    neo_rec_frame_done(buf, (int)len);   /* before the volume: the machine's output */
    vol = common_emu_sound_get_volume();
    if (vol < 255) {
        for (i = 0; i < (int)len; i++)
            buf[i] = (int16_t)((buf[i] * vol) / 255);
    }
}

static bool boot_game(void)
{
    if (!ACTIVE_FILE || !ACTIVE_FILE->path[0]) {
        printf("neogeo: no ACTIVE_FILE\n");
        boot_fail_reason = "no ROM path";
        return false;
    }

    /* YM tables in DTCM first. ZIP cache uses ram_emu then ram_init() —
     * lasting bump allocs (fix_board) happen inside bind after that. */
    neo_dtcm_ensure();
    if (!YM2610_prepare_tables()) {
        boot_fail_reason = "DTCM OOM (ym tables)";
        return false;
    }

    cf_init();
    cf_get_item_by_name("dump"); /* ensure dump=true default */
    conf.sound = 1;
    conf.sample_rate = neo_timing_rate();
    conf.screen320 = 1;
    conf.system = SYS_UNIBIOS;
    conf.country = CTY_EUROPE; /* overwritten by neo_settings_apply_unibios */
    /* Retro-Go / host pace via common_emu_frame_loop — never busy-wait. */
    conf.autoframeskip = 0;
    conf.sleep_idle = 0;

    if (screen_init() != GN_TRUE) {
        printf("neogeo: screen_init failed\n");
        boot_fail_reason = "screen_init OOM";
        return false;
    }

    if (init_game((char *)ACTIVE_FILE->path) != GN_TRUE) {
        boot_fail_reason = gno_flash_last_error();
        printf("neogeo: init_game failed: %s\n",
               boot_fail_reason ? boot_fail_reason : "?");
        return false;
    }

    /* Battery SRAM + memcard on AHB (after ZIP so AHB isn't fragmented by
     * transient FatFs/miniz state — heap is freeable but keep sram early). */
    if (!memory.sram) {
        memory.sram = ahb_calloc(1, 0x10000);
        if (!neo_alloc_ok(memory.sram)) {
            printf("neogeo: FATAL AHB sram\n");
            boot_fail_reason = "AHB OOM (sram)";
            return false;
        }
        printf("neogeo: AHB sram=%p free=%u\n",
               (void *)memory.sram, (unsigned)ahb_get_free_size());
    }
    if (!memory.memcard) {
        memory.memcard = ahb_calloc(1, 0x800);
        if (!neo_alloc_ok(memory.memcard)) {
            printf("neogeo: FATAL AHB memcard\n");
            boot_fail_reason = "AHB OOM (memcard)";
            return false;
        }
    }

    /* UniBIOS prefs (AES/MVS + region) before the 68k boots the BIOS. */
    neo_settings_apply_unibios();

    init_neo();
    if (!m68ki_instruction_jump_table) {
        printf("neogeo: FATAL no 68k jump table (RAM_EMU too tight)\n");
        boot_fail_reason = "RAM OOM (68k JT)";
        return false;
    }
    /* PVC 8 KiB after JT so RAM_EMU leftover is known. */
    if (!neo_pvc_ensure_ram()) {
        boot_fail_reason = "OOM (pvc)";
        return false;
    }
    printf("neogeo: booted %s (AHB free=%u RAM free=%u)\n",
           conf.game ? conf.game : "?",
           (unsigned)ahb_get_free_size(), (unsigned)ram_get_free_size());
    return true;
}


/* True if any face/d-pad button is down (POWER excluded — that is sleep). */
static bool neo_any_button(const odroid_gamepad_state_t *j)
{
    for (int i = 0; i < ODROID_INPUT_MAX; i++) {
        if (i == ODROID_INPUT_POWER)
            continue;
        if (j->values[i])
            return true;
    }
    return false;
}

/* Show a fatal boot error until any button is pressed, then return to the
 * launcher. Waits for a release first so a leftover press from launch does
 * not dismiss immediately. */
static void neo_fatal_quit(const char *line1, const char *line2)
{
    odroid_gamepad_state_t joystick;
    uint16_t *fb = lcd_get_active_buffer();

    if (fb) {
        memset(fb, 0, WIDTH * HEIGHT * 2);
        if (line1)
            odroid_overlay_draw_text(8, 8, 0, (char *)line1, 0xFFFF, 0);
        if (line2)
            odroid_overlay_draw_text(8, 28, 0, (char *)line2, 0xFFFF, 0);
        odroid_overlay_draw_text(8, 56, 0, "Press any button", 0xFFFF, 0);
        lcd_swap();
    }

    do {
        wdog_refresh();
        odroid_input_read_gamepad(&joystick);
    } while (neo_any_button(&joystick));

    do {
        wdog_refresh();
        odroid_input_read_gamepad(&joystick);
    } while (!neo_any_button(&joystick));

    odroid_system_switch_app(0);
}

static char neo_opt_hw_str[12];
static char neo_opt_region_str[12];
static char neo_opt_swap_ab_str[4];
static char neo_opt_swap_cd_str[4];

static void neo_hw_to_str(neo_hw_t hw, char *buf)
{
    strcpy(buf, hw == NEO_HW_MVS ? "MVS" : "AES");
}

static void neo_region_to_str(neo_region_t region, char *buf)
{
    switch (region) {
    case NEO_REGION_JAPAN:  strcpy(buf, "Japan");  break;
    case NEO_REGION_USA:    strcpy(buf, "USA");    break;
    default:                strcpy(buf, "Europe"); break;
    }
}

static void neo_apply_and_reset(void)
{
    neo_rec_close();                 /* a reset ends a record/replay */
    /*
     * UniBIOS reads region/mode once into 68k RAM at power-on; a plain
     * cpu_68k_reset keeps those values. Clear work RAM + restamp BRAM/
     * memcard + BIOS vectors so the splash re-reads our options.
     */
    neo_settings_apply_unibios();
    neo_timing_apply();              /* the System sets the frame rate */
    neo_audio_start();
    memset(memory.ram, 0, 0x10000);
    neo_vector_use_bios();
    neogeo_reset();
}

static bool update_hw_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                         uint32_t repeat)
{
    (void)repeat;
    neo_hw_t hw = neo_settings_hw_get();

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        hw = (hw == NEO_HW_AES) ? NEO_HW_MVS : NEO_HW_AES;
        neo_settings_hw_set(hw);
    }
    neo_hw_to_str(hw, option->value);
    if (event == ODROID_DIALOG_ENTER)
        neo_apply_and_reset();
    return event == ODROID_DIALOG_ENTER;
}

static bool update_region_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                             uint32_t repeat)
{
    (void)repeat;
    int region = (int)neo_settings_region_get();

    if (event == ODROID_DIALOG_PREV)
        region = region > 0 ? region - 1 : 2;
    if (event == ODROID_DIALOG_NEXT)
        region = region < 2 ? region + 1 : 0;
    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT)
        neo_settings_region_set((neo_region_t)region);

    neo_region_to_str((neo_region_t)region, option->value);
    if (event == ODROID_DIALOG_ENTER)
        neo_apply_and_reset();
    return event == ODROID_DIALOG_ENTER;
}

static bool update_swap_ab_cb(odroid_dialog_choice_t *option,
                              odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int enabled = neo_settings_swap_ab_get();

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        enabled = !enabled;
        neo_settings_swap_ab_set(enabled);
    }

    strcpy(option->value, enabled ? "On" : "Off");
    return event == ODROID_DIALOG_ENTER;
}

static bool update_swap_cd_cb(odroid_dialog_choice_t *option,
                              odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int enabled = neo_settings_swap_cd_get();

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        enabled = !enabled;
        neo_settings_swap_cd_set(enabled);
    }

    strcpy(option->value, enabled ? "On" : "Off");
    return event == ODROID_DIALOG_ENTER;
}

/* Record / replay (neo_rec.c): actions, not toggles -- the label says what
 * selecting the entry does. */
static bool rec_record_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                          uint32_t repeat)
{
    (void)option; (void)repeat;
    if (event == ODROID_DIALOG_ENTER) {
        if (neo_rec_mode() == NEO_REC_RECORD)
            neo_rec_close();
        else
            neo_rec_start(NEO_REC_RECORD, ACTIVE_FILE ? ACTIVE_FILE->path : NULL);
    }
    return event == ODROID_DIALOG_ENTER;
}

static bool rec_replay_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                          uint32_t repeat)
{
    (void)option; (void)repeat;
    if (event == ODROID_DIALOG_ENTER) {
        if (neo_rec_mode() == NEO_REC_REPLAY)
            neo_rec_close();
        else
            neo_rec_start(NEO_REC_REPLAY, ACTIVE_FILE ? ACTIVE_FILE->path : NULL);
    }
    return event == ODROID_DIALOG_ENTER;
}

void app_main_neogeo(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    odroid_gamepad_state_t joystick;

    gw_core_bridge_init();
    neo_apply_cpu_clock();
    memset(&pad, 0, sizeof(pad));

#ifndef HOST_BUILD
    /* Map cold .text/.rodata into QSPI and rebase CAFE pointers in RAM_EMU
     * before any call into neo_state / gno_flash / neocrypt / … */
    if (!neo_load_flash_cold()) {
        boot_fail_reason = "missing gngeo.ro";
        neo_fatal_quit("Neo Geo: missing gngeo.ro", "Copy /cores/gngeo.ro");
    }
#endif

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    /* the real frame rate: the audio rate, samples per frame, frame time
     * and LCD refresh for the System (neo_timing.c) */
    odroid_system_init(APP_ID, NEO_AUDIO_RATE_NATIVE);
    neo_timing_probe();
    neo_timing_apply();
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot,
                           NULL, &SleepWake, &SramSave, NULL);

    neo_hw_to_str(neo_settings_hw_get(), neo_opt_hw_str);
    neo_region_to_str(neo_settings_region_get(), neo_opt_region_str);
    strcpy(neo_opt_swap_ab_str, neo_settings_swap_ab_get() ? "On" : "Off");
    strcpy(neo_opt_swap_cd_str, neo_settings_swap_cd_get() ? "On" : "Off");

    /* Start SAI/DMA only after ROM/flash load. Starting earlier leaves the
     * DMA half unfilled for seconds (QSPI cache / zip inflate) and can leave
     * audio silently dead until the next cold boot. */
    if (!boot_game()) {
        /* User cancelled flash cache — back to launcher, no error screen. */
        if (boot_fail_reason && strcmp(boot_fail_reason, "Cancelled") == 0)
            odroid_system_switch_app(0);

        neo_fatal_quit("Neo Geo: boot failed",
                       boot_fail_reason ? boot_fail_reason : "unknown");
    }

    if (load_state)
        odroid_system_emu_load_state(save_slot);
    else
        lcd_clear_buffers();

    neo_audio_start();
    NEO_PROF_INIT();
#ifdef HOST_BUILD
    /* NEO_RECORD=1 / NEO_REPLAY=1: from power-on at start-up, or at loop
     * frame NEO_REC_AT (a replay started mid-session must log the same
     * REC/hash lines); the recording is closed at exit */
    uint32_t host_loop_frame = 0, host_rec_at = 0;
    int host_rec_mode = getenv("NEO_RECORD") ? NEO_REC_RECORD :
                        getenv("NEO_REPLAY") ? NEO_REC_REPLAY : NEO_REC_OFF;
    if (getenv("NEO_REC_AT"))
        host_rec_at = (uint32_t)atoi(getenv("NEO_REC_AT"));
    atexit(neo_rec_close);
#endif

    while (1) {
        wdog_refresh();

        bool draw_frame = common_emu_frame_loop();
        bool can_present = draw_frame && !NEO_DISABLE_VIDEO;

#ifdef HOST_BUILD
        if (host_rec_mode != NEO_REC_OFF && host_loop_frame == host_rec_at) {
            neo_rec_start(host_rec_mode, ACTIVE_FILE ? ACTIVE_FILE->path : NULL);
            host_rec_mode = NEO_REC_OFF;
        }
        host_loop_frame++;
#endif
        odroid_input_read_gamepad(&joystick);
        {
            int rm = neo_rec_mode();
            odroid_dialog_choice_t options[] = {
                {100, "System", neo_opt_hw_str, 1, &update_hw_cb},
                {101, "Region", neo_opt_region_str, 1, &update_region_cb},
                {102, "Swap A/B", neo_opt_swap_ab_str, 1, &update_swap_ab_cb},
                {103, "Swap C/D", neo_opt_swap_cd_str, 1, &update_swap_cd_cb},
                {104, rm == NEO_REC_RECORD ? "Stop recording" : "Record input", NULL, 1, &rec_record_cb},
                {105, rm == NEO_REC_REPLAY ? "Stop replay" : "Replay input", NULL, 1, &rec_replay_cb},
                ODROID_DIALOG_CHOICE_LAST
            };
            common_emu_input_loop(&joystick, options, &neo_repaint);
        }
        common_emu_input_loop_handle_turbo(&joystick);
        pad = joystick;
        map_input(&joystick);

        /* Emu first, then audio, then video. A slow draw_screen must not
         * sit between YM fill and DMA — that was the voice chop/repeat. */
        NEO_PROF_MARK(NEO_PH_FRAMECTL);
        neo_run_frame(can_present ? 1 : 0);
        NEO_PROF_MARK(NEO_PH_EMU);
        submit_audio();
        NEO_PROF_MARK(NEO_PH_AUDIO);
        common_emu_sound_sync(false);
        NEO_PROF_MARK(NEO_PH_WAIT);

        int drawn = neo_frame_needs_draw(), vskip = 0;
#ifndef HOST_BUILD
        /* Never draw while LTDC still shows the back buffer: the last
         * lcd_swap() takes effect at the LCD's next vertical blank. If it
         * has not happened yet, this frame's picture is DROPPED, not waited
         * for: the loop runs on the audio DMA's clock (common_emu_sound_sync)
         * and the LCD on its own PLL3 at nearly the same rate (neo_timing.c:
         * the two drift slowly in phase), and
         * sleeping until the vblank here made the loop miss audio buffers
         * whenever the two phases met (the audio hung, and every cycle the
         * emulation saved went into that sleep). Checked HERE, after the
         * audio wait, about a frame after the last swap, it is rarely still
         * pending; checked before the emulation, a few microseconds after
         * the swap, it always was (every other frame undrawn: 30 fps). */
        if (drawn && lcd_is_swap_pending()) {
            drawn = 0;
            vskip = 1;
        }
        neo_phase_watch(vskip);
#endif
        if (drawn) {
#ifndef HOST_BUILD
            neo_bind_lcd_buffer();
#endif
            NEO_PROF_MARK(NEO_PH_VWAIT);
            neo_draw_frame();
            NEO_PROF_MARK(NEO_PH_VIDEO);
            neo_present_to_lcd(lcd_get_active_buffer());
            common_ingame_overlay();
            lcd_swap();
            NEO_PROF_MARK(NEO_PH_PRESENT);
        }
        NEO_PROF_FRAME_END(drawn, !draw_frame, vskip);
    }
}
