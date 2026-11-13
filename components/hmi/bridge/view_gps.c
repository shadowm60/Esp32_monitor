/*
 * GPS page: gps_svc status (receiver on GPIO6, enabled by the IO mode "GPS").
 *   gps_enabled                         hides the "Use GPIO6 for GPS" button
 *   gps_fix_text, gps_pos_text, gps_info_text
 *   gps_sats_text (gps_sat_table, monospace), gps_link_text, gps_raw_text (gps_raw_table)
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "bridge.h"
#include "gps_svc.h"
#include "io_svc.h"

static uivar_t v_enabled = UIVAR("gps_enabled");
static uivar_t v_fix = UIVAR("gps_fix_text");
static uivar_t v_pos = UIVAR("gps_pos_text");
static uivar_t v_info = UIVAR("gps_info_text");
static uivar_t v_sats = UIVAR("gps_sats_text");
static uivar_t v_link = UIVAR("gps_link_text");
static uivar_t v_raw = UIVAR("gps_raw_text");
static uivar_t v_io_mode = UIVAR("io_mode_sel");    /* owned by view_io */

/* in PSRAM: internal RAM is the tight resource, and too big for the LVGL task stack */
static gps_status_t *s_st;
#define BUF_LEN (GPS_MAX_SATS * 40 + 64)
static char *s_buf;

static const char *sys_name(char s)
{
    switch (s) {
    case 'P': return "GPS";
    case 'L': return "GLO";
    case 'A': return "GAL";
    case 'B': return "BDS";
    case 'S': return "SBS";
    case 'Q': return "QZS";
    default:  return "???";
    }
}

static const char *compass(float deg)
{
    static const char *dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    if (deg < 0) {
        return "";
    }
    return dirs[(int)((deg + 22.5f) / 45.0f) % 8];
}

void view_gps_init(void)
{
    s_st = heap_caps_calloc(1, sizeof(*s_st), MALLOC_CAP_SPIRAM);
    s_buf = heap_caps_calloc(1, BUF_LEN, MALLOC_CAP_SPIRAM);
    uv_bool(&v_enabled, false);
}

static void link_text(const gps_status_t *g)
{
    if (!g->enabled) {
        uv_str(&v_link, "GPIO6 is not in GPS mode. Wiring: UEXT 1 VCC -> J8 3V3, "
                        "2 GND -> GND, 4 (module TX) -> AD.");
        return;
    }
    const char *state;
    if (g->baud_locked) {
        state = "receiving NMEA";
    } else if (g->bytes == 0) {
        state = "no data: check the wire on J8 AD (UEXT pin 4)";
    } else {
        state = "data but no valid NMEA: searching the baud rate";
    }
    uv_strf(&v_link, "GPIO6, %" PRIu32 " baud: %s\n%" PRIu32 " bytes, %" PRIu32 " sentences, "
            "%" PRIu32 " checksum errors", g->baud, state, g->bytes, g->sentences, g->cksum_err);
}

void view_gps_update(bool visible)
{
    if (s_st == NULL || s_buf == NULL) {
        return;
    }
    gps_svc_get_status(s_st);
    const gps_status_t *g = s_st;
    uv_bool(&v_enabled, g->enabled);
    if (!visible) {
        return;
    }
    link_text(g);

    /* fix + position */
    if (!g->enabled) {
        uv_str(&v_fix, "GPS off");
    } else if (!g->baud_locked) {
        uv_str(&v_fix, "No data");
    } else if (!g->fix) {
        uv_strf(&v_fix, "No fix  (%u sats seen)", g->sats_view);
    } else {
        uv_strf(&v_fix, "%s fix, %u sats", g->fix_type == 3 ? "3D" : g->fix_type == 2 ? "2D" : "GPS",
                g->sats_used);
    }
    if (g->fix_age_ms != UINT32_MAX) {
        uv_strf(&v_pos, "%c %.6f\n%c %.6f%s", g->lat >= 0 ? 'N' : 'S', g->lat < 0 ? -g->lat : g->lat,
                g->lon >= 0 ? 'E' : 'W', g->lon < 0 ? -g->lon : g->lon, g->fix ? "" : "  (last)");
    } else {
        uv_str(&v_pos, g->enabled ? "waiting for a fix:\nneeds open sky" : "");
    }

    /* details */
    char utc[32] = "-", course[16] = "-", age[16] = "-";
    if (g->time_valid) {
        snprintf(utc, sizeof(utc), "%04u-%02u-%02u %02u:%02u:%02u", g->year, g->month, g->day,
                 g->hour, g->min, g->sec);
    }
    if (g->fix && g->course_deg >= 0) {
        snprintf(course, sizeof(course), "%.0f %s", g->course_deg, compass(g->course_deg));
    }
    if (g->fix_age_ms != UINT32_MAX) {
        snprintf(age, sizeof(age), "%" PRIu32 " s ago", g->fix_age_ms / 1000);
    }
    uv_strf(&v_info,
            "Altitude:  %.1f m\n"
            "Speed:  %.1f km/h    Course:  %s\n"
            "HDOP:  %.1f    PDOP:  %.1f\n"
            "UTC:  %s\n"
            "Last fix:  %s\n"
            "Clock:  %s",
            g->alt_m, g->fix ? g->speed_kmh : 0.0f, course, g->hdop, g->pdop, utc, age,
            g->clock_set ? "set from GPS" : "not set from GPS");

    /* satellites: "GPS  12 * el 45 az 123  38 ########" */
    int len = 0;
    for (int i = 0; i < g->nsats && len < (int)BUF_LEN - 48; i++) {
        const gps_sat_t *s = &g->sats[i];
        char bar[11] = "";
        int nb = s->snr > 0 ? (s->snr + 2) / 5 : 0;      /* 50 dB-Hz = 10 */
        if (nb > 10) nb = 10;
        memset(bar, '#', nb);
        bar[nb] = '\0';
        char snr[4] = " -";
        if (s->snr >= 0) snprintf(snr, sizeof(snr), "%2d", s->snr);
        len += snprintf(s_buf + len, BUF_LEN - len, "%s %3u %c %2d %3d %s %s\n",
                        sys_name(s->sys), s->prn, s->used ? '*' : ' ',
                        s->elev < 0 ? 0 : s->elev, s->azim < 0 ? 0 : s->azim, snr, bar);
    }
    if (len == 0) {
        snprintf(s_buf, BUF_LEN, g->enabled ? "none in view yet" : "-");
    } else {
        s_buf[len - 1] = '\0';
    }
    uv_str(&v_sats, s_buf);

    /* raw NMEA */
    len = 0;
    s_buf[0] = '\0';
    for (int i = 0; i < g->nraw; i++) {
        len += snprintf(s_buf + len, BUF_LEN - len, "%s%s", i ? "\n" : "", g->raw[i]);
    }
    uv_str(&v_raw, s_buf);
}

void view_gps_enable(void)
{
    uv_int(&v_io_mode, IO_MODE_GPS);    /* view_io applies + stores it on its next update */
}
