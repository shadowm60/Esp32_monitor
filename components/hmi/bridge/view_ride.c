/*
 * Ride page: big figures from ride_svc, recording state, arrival time for
 * the loaded route.
 *   ride_speed, ride_dist, ride_moving, ride_avg, ride_max, ride_climb,
 *   ride_info, ride_btn_text
 * Bottom row: the navigation arrow (ride_turn_arrow panel, drawn here) and
 * ride_turn_caption / ride_turn_dist: next turn at any distance, the way back
 * to the track when off route, or "Wrong way".
 * Sun: sunset (or sunrise) at the position, and a warning when the arrival
 * at the end of the route would be after dark.
 * Button: short click starts a ride; while one runs a short click only
 * says "hold to stop", a long press stops it.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "esp_attr.h"
#include "lvgl.h"
#include "ui.h"
#include "screens.h"
#include "bridge.h"
#include "ride_svc.h"
#include "gpx_svc.h"
#include "gps_svc.h"

#define EPOCH_VALID     1700000000
#define HINT_MS         3000
#define ETA_MIN_KMH     5.0f

static uivar_t v_speed = UIVAR("ride_speed");
static uivar_t v_dist = UIVAR("ride_dist");
static uivar_t v_moving = UIVAR("ride_moving");
static uivar_t v_avg = UIVAR("ride_avg");
static uivar_t v_max = UIVAR("ride_max");
static uivar_t v_climb = UIVAR("ride_climb");
static uivar_t v_info = UIVAR("ride_info");
static uivar_t v_btn = UIVAR("ride_btn_text");
static uivar_t v_turn_cap = UIVAR("ride_turn_caption");
static uivar_t v_turn_dist = UIVAR("ride_turn_dist");
static arrow_t s_arrow;

EXT_RAM_BSS_ATTR static ride_status_t s_r;
EXT_RAM_BSS_ATTR static gpx_status_t  s_g;
EXT_RAM_BSS_ATTR static gps_status_t  s_gps;
static uint32_t s_hint_until;           /* lv_tick: show "hold to stop" */
static bool     s_running;              /* as last commanded (the service is async) */

static const char *hms(uint32_t s, char *b, size_t n)
{
    snprintf(b, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    return b;
}

void view_ride_init(void)
{
    uv_str(&v_btn, "Start ride");
    lv_obj_t *box = objects.ride_turn_arrow;
    if (box != NULL) {
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_update_layout(box);
        float w = lv_obj_get_content_width(box), h = lv_obj_get_content_height(box);
        float scale = fminf(w / 100.0f, h / 110.0f);         /* the arrow is ~100 x 100 at scale 1 */
        arrow_init(&s_arrow, box, w / 2, h / 2 + 8 * scale, scale, (int)(8 * scale));
    }
}

static void update_turn(const gpx_status_t *g)
{
    nav_cue_t c;
    nav_cue(g, INFINITY, &c);
    arrow_show(&s_arrow, &c);
    char d[16];
    if (c.kind == CUE_NONE) {
        bool on_track = g->state == GPX_LOADED && g->fix && g->matched;
        uv_str(&v_turn_cap, g->state != GPX_LOADED ? "No route loaded" : !g->fix ? "No GPS fix"
                            : on_track ? "No more turns" : "Next turn");
        uv_str(&v_turn_dist, "-");
        return;
    }
    uv_str(&v_turn_cap, c.caption);
    if (c.dist < 0) {
        uv_str(&v_turn_dist, "back");
    } else {
        fmt_dist_short(d, sizeof(d), c.dist);
        uv_str(&v_turn_dist, d);
    }
}

void view_ride_update(bool visible)
{
    if (!visible) {
        return;
    }
    ride_svc_get_status(&s_r);
    s_running = s_r.state == RIDE_RUNNING;
    char a[24], b[24], c[24];

    uv_strf(&v_speed, s_r.fix ? "%.1f" : "-", s_r.speed_kmh);
    if (s_r.state == RIDE_IDLE) {
        uv_str(&v_dist, "-");
        uv_str(&v_moving, "-");
        uv_str(&v_avg, "-");
        uv_str(&v_max, "-");
        uv_str(&v_climb, "-");
    } else {
        uv_strf(&v_dist, "%.2f", s_r.dist_m / 1000);
        uv_str(&v_moving, hms(s_r.moving_s, a, sizeof(a)));
        uv_strf(&v_avg, "%.1f", s_r.avg_kmh);
        uv_strf(&v_max, "%.1f", s_r.max_kmh);
        uv_strf(&v_climb, "%.0f", s_r.climb_m);
    }
    uv_str(&v_btn, s_running ? "Hold to stop" : "Start ride");

    /* info lines */
    char state[96], rec[96], eta[96] = "";
    if (s_running) {
        uint32_t stopped = s_r.elapsed_s > s_r.moving_s ? s_r.elapsed_s - s_r.moving_s : 0;
        snprintf(state, sizeof(state), "%s  -  elapsed %s, stopped %s", !s_r.fix ? "No GPS fix" :
                 s_r.moving ? "Moving" : "Paused (not moving)", hms(s_r.elapsed_s, a, sizeof(a)),
                 hms(stopped, b, sizeof(b)));
    } else if (s_r.state == RIDE_STOPPED) {
        snprintf(state, sizeof(state), "Ride finished, elapsed %s", hms(s_r.elapsed_s, a, sizeof(a)));
    } else {
        snprintf(state, sizeof(state), "%s", s_r.fix ? "Ready: press Start ride" : "Waiting for a GPS fix");
    }
    if (s_r.logging) {
        snprintf(rec, sizeof(rec), "Recording rides/%s, %u points", s_r.file, (unsigned)s_r.points);
    } else if (s_r.msg[0]) {
        snprintf(rec, sizeof(rec), "%s", s_r.msg);
    } else if (s_r.file[0]) {
        snprintf(rec, sizeof(rec), "Saved rides/%s, %u points", s_r.file, (unsigned)s_r.points);
    } else {
        snprintf(rec, sizeof(rec), "The ride is saved as GPX in rides/ on the SD card");
    }

    /* arrival at the end of the loaded route, at the average moving speed */
    gpx_svc_get_status(&s_g);
    gps_svc_get_status(&s_gps);
    update_turn(&s_g);
    time_t now = time(NULL);
    time_t arrive = 0;
    if (s_g.state == GPX_LOADED && s_g.matched && s_running) {
        float v = s_r.avg_kmh >= ETA_MIN_KMH ? s_r.avg_kmh : s_r.speed_kmh;
        if (v >= ETA_MIN_KMH && now > EPOCH_VALID) {
            time_t at = now + (time_t)(s_g.togo_m / (v / 3.6f));
            arrive = at;
            struct tm tm;
            localtime_r(&at, &tm);
            strftime(c, sizeof(c), "%H:%M", &tm);
            snprintf(eta, sizeof(eta), "\nArrival %s: %.1f km to go at %.1f km/h", c, s_g.togo_m / 1000, v);
        } else {
            snprintf(eta, sizeof(eta), "\nRoute: %.1f km to go", s_g.togo_m / 1000);
        }
    }
    /* sun: today's sunset, or tomorrow's sunrise once it is dark */
    char sun[80] = "";
    time_t rise, set;
    if (s_gps.fix && now > EPOCH_VALID && sun_times(s_gps.lat, s_gps.lon, now, &rise, &set)) {
        struct tm tm;
        if (now < set) {
            unsigned left = (unsigned)(set - now) / 60;
            localtime_r(&set, &tm);
            strftime(c, sizeof(c), "%H:%M", &tm);
            int n = snprintf(sun, sizeof(sun), "\nSunset %s (in %u:%02u h)", c, left / 60, left % 60);
            if (arrive > set) {
                snprintf(sun + n, sizeof(sun) - n, " - you arrive after dark!");
            }
        } else if (sun_times(s_gps.lat, s_gps.lon, now + 86400, &rise, &set)) {
            localtime_r(&rise, &tm);
            strftime(c, sizeof(c), "%H:%M", &tm);
            snprintf(sun, sizeof(sun), "\nDark: sunrise %s", c);
        }
    }
    bool hint = (int32_t)(s_hint_until - lv_tick_get()) > 0;
    uv_strf(&v_info, "%s\n%s%s%s%s", state, rec, eta, sun, hint ? "\nHold the button to stop the ride" : "");
}

void view_ride_start(void)
{
    if (s_running) {
        s_hint_until = lv_tick_get() + HINT_MS;     /* a short click doesn't stop */
        return;
    }
    if (ride_svc_start() == ESP_OK) {
        s_running = true;
        gpx_svc_rejoin();           /* progress on the route counts from where this ride meets it */
    }
}

void view_ride_stop(void)
{
    if (s_running && ride_svc_stop() == ESP_OK) {
        s_running = false;
        s_hint_until = 0;
    }
}
