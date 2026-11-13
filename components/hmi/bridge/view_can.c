/*
 * CAN page.
 *   can_bitrate_sel, can_listen_only, can_autostart   inputs (two-way) -> settings
 *   can_running, can_logging                          state for hidden/checked flags
 *   can_run_text, can_log_text                        button captions
 *   can_stats, can_log_info, can_load, can_ids        monitor output
 * Changing bitrate or listen-only while running restarts the bus.
 */
#include <stdio.h>
#include "esp_attr.h"
#include <inttypes.h>
#include "lvgl.h"
#include "bridge.h"
#include "canmon.h"
#include "settings.h"

#define IDS_PERIOD_MS   500
#define IDS_MAX_ROWS    64

/* index = can_bitrate_sel (dropdown order in the EEZ project) */
static const uint32_t BITRATES[] = { 125000, 250000, 500000, 1000000 };
#define N_BITRATES (sizeof(BITRATES) / sizeof(BITRATES[0]))

static uivar_t v_bitrate = UIVAR("can_bitrate_sel");
static uivar_t v_listen = UIVAR("can_listen_only");
static uivar_t v_auto = UIVAR("can_autostart");
static uivar_t v_running = UIVAR("can_running");
static uivar_t v_logging = UIVAR("can_logging");
static uivar_t v_run_text = UIVAR("can_run_text");
static uivar_t v_log_text = UIVAR("can_log_text");
static uivar_t v_stats = UIVAR("can_stats");
static uivar_t v_log_info = UIVAR("can_log_info");
static uivar_t v_load = UIVAR("can_load");
static uivar_t v_ids = UIVAR("can_ids");

static app_settings_t s_cfg;        /* CAN part as last applied */
static uint32_t       s_ids_next;

static uint32_t bitrate_of(int idx)
{
    return BITRATES[idx >= 0 && idx < (int)N_BITRATES ? idx : 2];
}

void view_can_init(void)
{
    settings_get(&s_cfg);
    if (s_cfg.can_bitrate_idx >= N_BITRATES) {
        s_cfg.can_bitrate_idx = 2;
    }
    uv_int(&v_bitrate, s_cfg.can_bitrate_idx);
    uv_bool(&v_listen, s_cfg.can_listen_only);
    uv_bool(&v_auto, s_cfg.can_autostart);
}

static void poll_inputs(bool running)
{
    int br = uv_get_int(&v_bitrate, s_cfg.can_bitrate_idx);
    bool lo = uv_get_bool(&v_listen, s_cfg.can_listen_only);
    bool au = uv_get_bool(&v_auto, s_cfg.can_autostart);
    if (br < 0 || br >= (int)N_BITRATES) {
        br = 2;
    }
    if (br == s_cfg.can_bitrate_idx && lo == s_cfg.can_listen_only && au == s_cfg.can_autostart) {
        return;
    }
    bool restart = running && (br != s_cfg.can_bitrate_idx || lo != s_cfg.can_listen_only);
    app_settings_t cfg;
    settings_get(&cfg);
    cfg.can_bitrate_idx = s_cfg.can_bitrate_idx = br;
    cfg.can_listen_only = s_cfg.can_listen_only = lo;
    cfg.can_autostart = s_cfg.can_autostart = au;
    settings_put(&cfg);
    if (restart) {
        canmon_start(bitrate_of(br), lo);
    }
}

void view_can_update(bool visible)
{
    canmon_stats_t st;
    canmon_get_stats(&st);
    poll_inputs(st.running);

    uv_bool(&v_running, st.running);
    uv_bool(&v_logging, st.log_active);
    uv_str(&v_run_text, st.running ? "Stop" : "Start");
    uv_str(&v_log_text, st.log_active ? "Stop recording" : "Record to SD");
    uv_int(&v_load, st.load_pct);

    if (!visible) {
        return;
    }
    char up[24];
    if (st.running) {
        uv_strf(&v_stats,
                "%s  %" PRIu32 " kbit/s%s  %s\n"
                "%" PRIu32 " frames/s (peak %" PRIu32 ")  load ~%u %%\n"
                "total %" PRIu32 "  std %" PRIu32 "  ext %" PRIu32 "\n"
                "IDs %" PRIu32 "%s  dropped %" PRIu32 "\n"
                "bus errors %" PRIu32 "  TEC %" PRIu32 "  REC %" PRIu32,
                st.state, st.bitrate / 1000, st.listen_only ? " listen-only" : "",
                fmt_duration(st.uptime_s, up, sizeof(up)),
                st.fps, st.fps_peak, st.load_pct,
                st.frames, st.std_frames, st.ext_frames,
                st.ids, st.id_table_full ? " (table full)" : "", st.dropped,
                st.bus_errors, st.tec, st.rec);
    } else {
        uv_strf(&v_stats, "stopped\n\nNative USB is on GPIO19/20 while CAN is stopped.\n"
                          "Termination: 120R jumper on the board.");
    }

    if (st.log_active) {
        uv_strf(&v_log_info, "REC %s\n%" PRIu32 " frames, %" PRIu32 " kB, %s%s%" PRIu32 " lost",
                st.log_file, st.log_frames, st.log_bytes / 1024, fmt_duration(st.log_secs, up, sizeof(up)),
                st.log_dropped ? "\n" : ", ", st.log_dropped);
    } else if (st.log_error[0]) {
        uv_strf(&v_log_info, "Log error: %s", st.log_error);
    } else if (st.log_file[0]) {
        uv_strf(&v_log_info, "Last log: %s\n%" PRIu32 " frames, %" PRIu32 " kB",
                st.log_file, st.log_frames, st.log_bytes / 1024);
    } else {
        uv_str(&v_log_info, "Logs go to " CONFIG_MON_CANLOG_DIR " (candump format)");
    }

    uint32_t now = lv_tick_get();
    if ((int32_t)(now - s_ids_next) >= 0) {
        s_ids_next = now + IDS_PERIOD_MS;
        EXT_RAM_BSS_ATTR static char ids[IDS_MAX_ROWS * 64 + 128];
        canmon_format_ids(ids, sizeof(ids), IDS_MAX_ROWS);
        uv_str(&v_ids, ids);
    }
}

void view_can_start_stop(void)
{
    canmon_stats_t st;
    canmon_get_stats(&st);
    if (st.running) {
        canmon_stop();
    } else {
        canmon_start(bitrate_of(s_cfg.can_bitrate_idx), s_cfg.can_listen_only);
    }
}

void view_can_log_toggle(void)
{
    canmon_stats_t st;
    canmon_get_stats(&st);
    if (st.log_active) {
        canmon_log_stop();
        return;
    }
    if (!st.running) {
        canmon_start(bitrate_of(s_cfg.can_bitrate_idx), s_cfg.can_listen_only);
    }
    canmon_log_start();     /* queued after the start */
}

void view_can_reset(void)
{
    canmon_reset();
}
