/*
 * Internal to hmi/bridge. Everything here runs in the LVGL task (lv_timers,
 * event callbacks, EEZ native actions): the EEZ flow engine is not thread
 * safe, and services are only reached through their thread-safe APIs.
 */
#ifndef HMI_BRIDGE_H
#define HMI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------- uivar.c
 * EEZ global variables by name, resolved on first use (missing variable =
 * calls are ignored, so the UI may drop any of them). Setters only write when
 * the value differs, so unchanged texts are not redrawn. */
typedef struct {
    const char *name;
    int32_t     idx;        /* -2 = not resolved yet, -1 = not in the UI */
    uint32_t    hash;       /* last string written */
    bool        written;
} uivar_t;

#define UIVAR(n) { .name = (n), .idx = -2 }

void    uv_str(uivar_t *v, const char *s);
void    uv_strf(uivar_t *v, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void    uv_int(uivar_t *v, int32_t x);
void    uv_bool(uivar_t *v, bool b);
int32_t uv_get_int(uivar_t *v, int32_t dflt);
bool    uv_get_bool(uivar_t *v, bool dflt);
void    uv_get_str(uivar_t *v, char *buf, size_t len);
bool    uv_exists(uivar_t *v);

/* ---------------------------------------------------------------- nav.c */
void        nav_init(const char *page);     /* stored page name, "" = first */
void        nav_go(const char *page);
const char *nav_current(void);              /* EEZ screen name, lower case */
bool        nav_is(const char *page);

/* ---------------------------------------------------------------- screen.c
 * keyboard for text areas, screen timeout / wake, brightness */
void screen_init(uint8_t brightness, uint8_t timeout_idx);
void screen_set_timeout(uint8_t timeout_idx);
void screen_off(void);
bool screen_is_off(void);
void screen_wake(void);                     /* backlight on, timeout restarts */
void screen_hold(bool hold);                /* true: no timeout until released */
void screen_poll(void);                     /* 250 ms */

/* brightness.c (from FP_Dash) */
void brightness_init(uint8_t percent);
void brightness_set(uint8_t percent);

/* ---------------------------------------------------------------- views
 * init: once after ui_init(); update: from the bridge timers. `visible` =
 * the page is on screen (expensive texts only then). */
void view_status_update(void);              /* status line, every page */
void view_home_init(void);
void view_home_update(bool visible);
void view_wifi_init(void);
void view_wifi_update(bool visible);
void view_ble_init(void);
void view_ble_update(bool visible);
void view_can_init(void);
void view_can_update(bool visible);
void view_io_init(void);
void view_io_update(bool visible);
void view_gps_init(void);
void view_gps_update(bool visible);
void view_route_init(void);
void view_route_update(bool visible);
void view_profile_init(void);
void view_profile_update(bool visible);
void view_ride_init(void);
void view_ride_update(bool visible);
void view_display_init(void);
void view_display_update(bool visible);

/* actions (called by actions.c) */
void view_wifi_scan(void);
void view_wifi_connect(void);
void view_wifi_disconnect(void);
void view_wifi_forget(void);
void view_ble_scan(void);
void view_ble_connect(void);
void view_ble_disconnect(void);
void view_ble_forget(void);
void view_can_start_stop(void);
void view_can_log_toggle(void);
void view_can_reset(void);
void view_io_toggle_out(void);
void view_io_reset(void);
void view_io_i2c_scan(void);
void view_gps_enable(void);
void view_route_load(void);
void view_route_rescan(void);
void view_route_reverse(void);
void view_route_zoom(int dir);              /* +1 = in, -1 = out */
void view_route_toggle_view(void);
void view_profile_toggle_view(void);
void view_ride_start(void);
void view_ride_stop(void);
void view_display_sd_remount(void);
void view_display_restart(void);

/* navcue.c: the navigation arrow (Route box, Ride tile) */
#include "lvgl.h"
#include "gpx_svc.h"
typedef enum { CUE_NONE = 0, CUE_TURN, CUE_REJOIN, CUE_WRONG } cue_kind_t;
typedef struct {
    cue_kind_t kind;
    int        angle;           /* turn: heading change; rejoin: direction (0 = up) */
    float      dist;            /* m, < 0 = none */
    char       caption[32];     /* "Next: sharp left", "To the track", "Wrong way" */
} nav_cue_t;
typedef struct {
    lv_obj_t  *line, *head;
    lv_point_t p[4], h[3];
    float      cx, cy, s;
    int        key;
} arrow_t;
void nav_cue(const gpx_status_t *g, float turn_show_m, nav_cue_t *c);
void arrow_init(arrow_t *a, lv_obj_t *parent, float cx, float cy, float scale, int width);
void arrow_show(arrow_t *a, const nav_cue_t *c);
void fmt_dist_short(char *b, size_t n, float m);   /* "45 m", "850 m", "1.2 km", "61 km" */

/* sun.c: sunrise / sunset (UTC epoch) of the day containing `when`;
 * false in polar day / night */
#include <time.h>
bool sun_times(double lat, double lon, time_t when, time_t *rise, time_t *set);

/* shared formatting */
const char *fmt_duration(uint32_t s, char *buf, size_t len);    /* "1d 02:03:04" */

#endif /* HMI_BRIDGE_H */
