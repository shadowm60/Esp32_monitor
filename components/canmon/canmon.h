/*
 * canmon - CAN bus monitor and raw logger.
 *
 * Owns the CAN side of the board for the UI:
 *   - start / stop the bus (switches GPIO19/20 from USB to the CAN
 *     transceiver via CH422G EXIO5 while running, back to USB when stopped)
 *   - statistics: frames/s, peak, estimated bus load, error counters
 *   - a table of every ID seen: DLC, last data, count, period
 *   - raw log to the SD card in candump format, readable by can-utils
 *     (canplayer, log2asc), SavvyCAN and python-can:
 *         (1731488400.123000) can0 1A0#0102030405060708
 *     File: CONFIG_MON_CANLOG_DIR/YYYYMMDD_HHMMSS.log when the clock is set
 *     (Wi-Fi + SNTP), else can_NNNN.log with seconds since boot.
 *
 * One task does everything (core 0). Commands are queued, so all calls are
 * non-blocking and safe from the LVGL task.
 */
#ifndef CANMON_H
#define CANMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     running;
    uint32_t bitrate;
    bool     listen_only;
    const char *state;          /* "stopped", "active", "warning", "passive", "bus-off" */
    uint32_t frames;            /* since start / reset */
    uint32_t fps;               /* last second */
    uint32_t fps_peak;
    uint32_t std_frames, ext_frames;
    uint8_t  load_pct;          /* estimated bus load, last second */
    uint32_t ids;               /* distinct IDs in the table */
    bool     id_table_full;
    uint32_t bus_errors;
    uint32_t tec, rec;
    uint32_t dropped;           /* frames lost before the monitor (queue full) */
    uint32_t uptime_s;          /* since start */
    /* logging */
    bool     log_active;
    char     log_file[40];      /* current or last file, "" = none */
    uint32_t log_frames;
    uint32_t log_bytes;
    uint32_t log_dropped;       /* buffer full: SD card too slow */
    uint32_t log_secs;
    char     log_error[48];     /* "" = ok */
} canmon_stats_t;

esp_err_t canmon_init(void);

esp_err_t canmon_start(uint32_t bitrate, bool listen_only);
esp_err_t canmon_stop(void);
esp_err_t canmon_log_start(void);
esp_err_t canmon_log_stop(void);
esp_err_t canmon_reset(void);           /* clear counters and the ID table */

void canmon_get_stats(canmon_stats_t *out);

/* Writes the ID table as fixed-width text (sorted by ID), at most `max_rows`
 * rows. Returns the number of IDs in the table. */
int canmon_format_ids(char *buf, size_t len, int max_rows);

#ifdef __cplusplus
}
#endif

#endif /* CANMON_H */
