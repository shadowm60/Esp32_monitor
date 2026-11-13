#include "gpx_svc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "sd_card.h"
#include "gps_svc.h"

static const char *TAG = "gpx_svc";

#define DIR_PATH        CONFIG_MON_GPX_DIR
#define READ_CHUNK      4096
#define NAV_PERIOD_MS   1000
#define EARTH_R         6371000.0
#define DEG             (M_PI / 180.0)
#define CLIMB_HYST_M    2.0f        /* elevation noise ignored for the climb */
#define WINDOW_BACK     100         /* segments searched around the last match */
#define WINDOW_AHEAD    500
#define REMATCH_M       50.0f       /* window match worse than this -> full search */
#define SMOOTH_M        50.0f       /* elevation smoothing, +- */
#define GRADE_M         50.0f       /* live gradient over +- this */
#define CLIMB_DROP_M    10.0f       /* a descent of more than this ends a climb */
#define CLIMB_MIN_LEN_M 300.0f
#define CLIMB_MIN_GAIN  15.0f
#define CLIMB_MIN_PCT   3.0f
#define CLIMB_TRIM_M    100.0f      /* run-in / run-out flatter than CLIMB_TRIM_PCT is cut */
#define CLIMB_TRIM_PCT  1.5f
#define TURN_STEP_M     5.0f        /* resampling for the turn search */
#define TURN_MAX_SAMPLES 40000
#define TURN_SMOOTH     2           /* +- samples, against recorded-track jitter */
#define TURN_WIN_M      20.0f       /* heading before / after */
#define TURN_WIN2_M     35.0f       /* wider window for the angle of a rounded corner */
#define TURN_MIN_DEG    35.0f
#define TURN_MERGE_M    30.0f
#define TURN_MAX        4000
#define TURN_PASSED_M   10.0f       /* a turn stays "next" until this far past it */
#define TURN2_NEAR_M    200.0f
#define WRONG_WAY_S     5           /* updates (s) against the track before warning */
#define WRONG_WAY_DEG   120.0f
#define RIGHT_WAY_DEG   60.0f
#define WRONG_WAY_KMH   5.0f
#define POI_PASSED_M    20.0f

typedef enum { CMD_RESCAN, CMD_LOAD, CMD_UNLOAD, CMD_REJOIN } cmd_type_t;
typedef struct {
    cmd_type_t type;
    bool       reverse;
    char       file[GPX_NAME_LEN];
} cmd_t;

static SemaphoreHandle_t s_lock;        /* s_st */
static SemaphoreHandle_t s_pts_lock;    /* s_pts / s_npts swap vs. readers */
static QueueHandle_t     s_q;

EXT_RAM_BSS_ATTR static gpx_status_t s_st;
EXT_RAM_BSS_ATTR static gps_status_t s_gps;
EXT_RAM_BSS_ATTR static char         s_chunk[READ_CHUNK];

static gpx_point_t *s_pts;              /* PSRAM, owned by the service task */
static uint32_t     s_npts;
static gpx_turn_t  *s_turns;            /* PSRAM, sorted by distance */
static uint32_t     s_nturns;
static float       *s_prof;             /* smoothed elevation every s_step m, PSRAM */
static float       *s_asc;              /* cumulative ascent at each sample */
static uint32_t     s_nprof;
static float        s_step;
static gpx_poi_t   *s_pois;             /* PSRAM */
static uint32_t     s_npois;
static double       s_lat0, s_lon0, s_coslat;
static bool         s_have_origin;
static int          s_wrong_cnt;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ---------------------------------------------------------------- listing */

static bool is_gpx(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".gpx") == 0 && n[0] != '.';
}

static int name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void do_rescan(void)
{
    EXT_RAM_BSS_ATTR static char files[GPX_MAX_FILES][GPX_NAME_LEN];    /* sorted outside the lock */
    int n = 0;
    char msg[48] = "";

    LOCK();
    s_st.listing = true;
    UNLOCK();

    if (!waveshare_sd_card_mounted()) {
        snprintf(msg, sizeof(msg), "no SD card");
    } else {
        DIR *d = opendir(DIR_PATH);
        if (d == NULL) {
            mkdir(DIR_PATH, 0775);
            snprintf(msg, sizeof(msg), "created %s: put .gpx files there", DIR_PATH);
        } else {
            struct dirent *e;
            while ((e = readdir(d)) != NULL && n < GPX_MAX_FILES) {
                if (e->d_type != DT_DIR && is_gpx(e->d_name) && strlen(e->d_name) < GPX_NAME_LEN) {
                    strcpy(files[n++], e->d_name);
                }
            }
            closedir(d);
            if (n == 0) {
                snprintf(msg, sizeof(msg), "no .gpx files in %s", DIR_PATH);
            }
        }
    }
    qsort(files, n, GPX_NAME_LEN, name_cmp);

    LOCK();
    memcpy(s_st.files, files, sizeof(files[0]) * n);
    s_st.nfiles = (uint8_t)n;
    snprintf(s_st.list_msg, sizeof(s_st.list_msg), "%s", msg);
    s_st.listing = false;
    s_st.list_gen++;
    UNLOCK();
    ESP_LOGI(TAG, "%d GPX file(s) in %s %s", n, DIR_PATH, msg);
}

/* ---------------------------------------------------------------- parser
 * A small streaming XML tokenizer: tags and the text of <ele> / <name>.
 * Good enough for GPX written by Strava, Komoot, GPS Visualizer, Garmin,
 * OsmAnd, ... Namespace prefixes are ignored. */

enum { KIND_NONE = 0, KIND_TRK, KIND_RTE };

typedef struct {
    /* tokenizer */
    bool   in_tag;
    char   tag[256];
    int    tlen;
    bool   capture;             /* collecting text for <ele> / <name> */
    char   text[64];
    int    xlen;
    /* GPX context */
    int    kind_used;
    int    pt_kind;             /* point being read, KIND_NONE = outside */
    double lat, lon;
    float  ele;
    bool   in_wpt;
    double wlat, wlon;
    bool   wok;
    char   wname[GPX_POI_NAME_LEN];
    struct { double lat, lon; char name[GPX_POI_NAME_LEN]; } wpts[GPX_MAX_POIS];
    int    nw;
    /* result */
    gpx_point_t *pts;
    uint32_t n, cap;
    bool   truncated;
    char   name[48];
    double lat0, lon0, coslat;
    float  ele_min, ele_max, climb, climb_ref;
    bool   has_ele;
    float  min_x, max_x, min_y, max_y;
} parser_t;

static const char *local_name(const char *s)
{
    const char *c = strchr(s, ':');
    return c ? c + 1 : s;
}

/* finds attribute `want` (prefix ignored) in "trkpt lat="1" lon='2'" */
static bool get_attr(const char *t, const char *want, double *out)
{
    const char *p = t;
    while (*p && !isspace((unsigned char)*p)) p++;      /* element name */
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        const char *n0 = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p) && *p != '/') p++;
        size_t nl = p - n0;
        while (isspace((unsigned char)*p)) p++;
        if (*p != '=') {
            if (*p) p++;
            continue;
        }
        p++;
        while (isspace((unsigned char)*p)) p++;
        char q = *p;
        if (q != '"' && q != '\'') {
            return false;
        }
        const char *v = ++p;
        while (*p && *p != q) p++;
        char name[16];
        if (nl < sizeof(name)) {
            memcpy(name, n0, nl);
            name[nl] = '\0';
            if (strcmp(local_name(name), want) == 0) {
                *out = strtod(v, NULL);
                return true;
            }
        }
        if (*p) p++;
    }
    return false;
}

static void add_point(parser_t *ps)
{
    if (ps->n >= GPX_MAX_POINTS) {
        ps->truncated = true;
        return;
    }
    if (ps->n == ps->cap) {
        uint32_t cap = ps->cap ? ps->cap * 2 : 2048;
        if (cap > GPX_MAX_POINTS) cap = GPX_MAX_POINTS;
        gpx_point_t *np = heap_caps_realloc(ps->pts, cap * sizeof(gpx_point_t), MALLOC_CAP_SPIRAM);
        if (np == NULL) {
            ps->truncated = true;
            return;
        }
        ps->pts = np;
        ps->cap = cap;
    }
    if (ps->n == 0) {
        ps->lat0 = ps->lat;
        ps->lon0 = ps->lon;
        ps->coslat = cos(ps->lat * DEG);
    }
    gpx_point_t *pt = &ps->pts[ps->n];
    pt->x = (float)((ps->lon - ps->lon0) * DEG * EARTH_R * ps->coslat);
    pt->y = (float)((ps->lat - ps->lat0) * DEG * EARTH_R);
    pt->ele = ps->ele;
    pt->dist = 0;
    if (ps->n > 0) {
        const gpx_point_t *pv = pt - 1;
        pt->dist = pv->dist + hypotf(pt->x - pv->x, pt->y - pv->y);
    }
    if (ps->n == 0) {
        ps->min_x = ps->max_x = pt->x;
        ps->min_y = ps->max_y = pt->y;
    } else {
        ps->min_x = fminf(ps->min_x, pt->x);
        ps->max_x = fmaxf(ps->max_x, pt->x);
        ps->min_y = fminf(ps->min_y, pt->y);
        ps->max_y = fmaxf(ps->max_y, pt->y);
    }
    if (!isnan(pt->ele)) {
        if (!ps->has_ele) {
            ps->has_ele = true;
            ps->ele_min = ps->ele_max = ps->climb_ref = pt->ele;
        }
        ps->ele_min = fminf(ps->ele_min, pt->ele);
        ps->ele_max = fmaxf(ps->ele_max, pt->ele);
        if (pt->ele - ps->climb_ref >= CLIMB_HYST_M) {
            ps->climb += pt->ele - ps->climb_ref;
            ps->climb_ref = pt->ele;
        } else if (ps->climb_ref - pt->ele >= CLIMB_HYST_M) {
            ps->climb_ref = pt->ele;
        }
    }
    ps->n++;
}

static void add_wpt(parser_t *ps)
{
    if (ps->wok && ps->nw < GPX_MAX_POIS) {
        ps->wpts[ps->nw].lat = ps->wlat;
        ps->wpts[ps->nw].lon = ps->wlon;
        strlcpy(ps->wpts[ps->nw].name, ps->wname[0] ? ps->wname : "Waypoint", GPX_POI_NAME_LEN);
        ps->nw++;
    }
    ps->wok = false;
}

static void start_capture(parser_t *ps)
{
    ps->capture = true;
    ps->xlen = 0;
}

static void on_text_done(parser_t *ps, const char *el)
{
    ps->text[ps->xlen] = '\0';
    ps->capture = false;
    if (strcmp(el, "ele") == 0 && ps->pt_kind != KIND_NONE) {
        ps->ele = strtof(ps->text, NULL);
    } else if (strcmp(el, "name") == 0 && ps->in_wpt) {
        const char *s = ps->text;
        while (isspace((unsigned char)*s)) s++;
        strlcpy(ps->wname, s, sizeof(ps->wname));
    } else if (strcmp(el, "name") == 0 && ps->name[0] == '\0' && !ps->in_wpt && ps->pt_kind == KIND_NONE) {
        const char *s = ps->text;
        while (isspace((unsigned char)*s)) s++;
        strlcpy(ps->name, s, sizeof(ps->name));
    }
}

static void on_tag(parser_t *ps)
{
    char *t = ps->tag;
    t[ps->tlen] = '\0';
    if (t[0] == '?') {
        return;
    }
    if (t[0] == '!') {
        if (strncmp(t, "![CDATA[", 8) == 0 && ps->capture) {    /* <name><![CDATA[x]]></name> */
            char *c = t + 8;
            size_t l = strlen(c);
            if (l >= 2 && strcmp(c + l - 2, "]]") == 0) c[l - 2] = '\0';
            for (; *c && ps->xlen < (int)sizeof(ps->text) - 1; c++) {
                ps->text[ps->xlen++] = *c;
            }
        }
        return;
    }
    bool close = t[0] == '/';
    if (close) t++;
    bool self = ps->tlen > 0 && ps->tag[ps->tlen - 1] == '/';

    char el[16];
    int i = 0;
    while (t[i] && !isspace((unsigned char)t[i]) && t[i] != '/' && i < (int)sizeof(el) - 1) {
        el[i] = t[i];
        i++;
    }
    el[i] = '\0';
    const char *name = local_name(el);

    if (!close) {
        int kind = strcmp(name, "trkpt") == 0 ? KIND_TRK : strcmp(name, "rtept") == 0 ? KIND_RTE : KIND_NONE;
        if (kind != KIND_NONE) {
            if (ps->kind_used == KIND_RTE && kind == KIND_TRK) {
                ps->n = 0;                      /* a track beats a route */
                ps->has_ele = false;
                ps->climb = 0;
                ps->kind_used = KIND_TRK;
            } else if (ps->kind_used == KIND_NONE) {
                ps->kind_used = kind;
            }
            double lat, lon;
            if (kind == ps->kind_used && get_attr(t, "lat", &lat) && get_attr(t, "lon", &lon)) {
                ps->pt_kind = kind;
                ps->lat = lat;
                ps->lon = lon;
                ps->ele = NAN;
                if (self) {
                    add_point(ps);
                    ps->pt_kind = KIND_NONE;
                }
            }
        } else if (strcmp(name, "wpt") == 0) {
            ps->wok = get_attr(t, "lat", &ps->wlat) && get_attr(t, "lon", &ps->wlon);
            ps->wname[0] = '\0';
            ps->in_wpt = !self;
            if (self) add_wpt(ps);
        } else if ((strcmp(name, "ele") == 0 || strcmp(name, "name") == 0) && !self) {
            start_capture(ps);
        }
    } else {
        if ((strcmp(name, "trkpt") == 0 || strcmp(name, "rtept") == 0) && ps->pt_kind != KIND_NONE) {
            add_point(ps);
            ps->pt_kind = KIND_NONE;
        } else if (strcmp(name, "wpt") == 0) {
            if (ps->in_wpt) add_wpt(ps);
            ps->in_wpt = false;
        } else if (ps->capture && (strcmp(name, "ele") == 0 || strcmp(name, "name") == 0)) {
            on_text_done(ps, name);
        }
    }
}

static void feed(parser_t *ps, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (!ps->in_tag) {
            if (c == '<') {
                ps->in_tag = true;
                ps->tlen = 0;
            } else if (ps->capture && ps->xlen < (int)sizeof(ps->text) - 1) {
                ps->text[ps->xlen++] = c;
            }
            continue;
        }
        if (c == '>') {
            /* comments and CDATA may contain '>': end only at "-->" / "]]>" */
            bool comment = ps->tlen >= 3 && strncmp(ps->tag, "!--", 3) == 0;
            bool cdata = ps->tlen >= 8 && strncmp(ps->tag, "![CDATA[", 8) == 0;
            bool end = true;
            if (comment) end = ps->tlen >= 5 && ps->tag[ps->tlen - 1] == '-' && ps->tag[ps->tlen - 2] == '-';
            if (cdata) end = ps->tlen >= 10 && ps->tag[ps->tlen - 1] == ']' && ps->tag[ps->tlen - 2] == ']';
            if (end) {
                ps->in_tag = false;
                on_tag(ps);
                continue;
            }
        }
        if (ps->tlen < (int)sizeof(ps->tag) - 1) {
            ps->tag[ps->tlen++] = c;
        } else if (ps->tag[0] == '!') {
            /* long comment: keep the last two chars to see its end */
            ps->tag[ps->tlen - 2] = ps->tag[ps->tlen - 1];
            ps->tag[ps->tlen - 1] = c;
        }
    }
}

/* ---------------------------------------------------------------- load */

typedef struct {
    float   *prof, *asc;
    uint32_t n;
    float    step;
    float    ascent, descent;
    uint8_t  nclimbs;
    gpx_climb_t climbs[GPX_MAX_CLIMBS];
} profile_t;

static void set_track(gpx_point_t *pts, uint32_t n, const profile_t *pr, gpx_turn_t *turns, uint32_t nturns)
{
    xSemaphoreTake(s_pts_lock, portMAX_DELAY);
    gpx_point_t *old = s_pts;
    float *old_prof = s_prof, *old_asc = s_asc;
    gpx_turn_t *old_turns = s_turns;
    s_pts = pts;
    s_npts = n;
    s_turns = turns;
    s_nturns = nturns;
    s_prof = pr ? pr->prof : NULL;
    s_asc = pr ? pr->asc : NULL;
    s_nprof = pr ? pr->n : 0;
    s_step = pr ? pr->step : 0;
    xSemaphoreGive(s_pts_lock);
    heap_caps_free(old);
    heap_caps_free(old_prof);
    heap_caps_free(old_asc);
    heap_caps_free(old_turns);
}

/* ---------------------------------------------------------------- turns */

static float wrap180(float a)
{
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

static float bearing(const float *x, const float *y, uint32_t i, uint32_t j)
{
    return atan2f(x[j] - x[i], y[j] - y[i]) * (180.0f / (float)M_PI);   /* 0 = north, 90 = east */
}

/* heading change at i: leg (i-k .. i-q) -> leg (i+q .. i+k); skipping +-q
 * samples keeps the corner the smoothing rounded off out of the legs */
static float heading_change(const float *x, const float *y, int i, int k, int q)
{
    return wrap180(bearing(x, y, i + q, i + k) - bearing(x, y, i - k, i - q));
}

static bool same_way(float a, float b)
{
    return (a > 0) == (b > 0) || (fabsf(a) > 150 && fabsf(b) > 150);   /* +-180 has no side */
}

/* Turns from the track's shape; returns the count, *out in PSRAM (or NULL). */
static uint32_t find_turns(const gpx_point_t *pts, uint32_t n, gpx_turn_t **out)
{
    *out = NULL;
    float len = pts[n - 1].dist;
    float step = fmaxf(TURN_STEP_M, len / (TURN_MAX_SAMPLES - 1));
    uint32_t m = (uint32_t)(len / step) + 1;
    int k = (int)(TURN_WIN_M / step + 0.5f), k2 = (int)(TURN_WIN2_M / step + 0.5f);
    if (k < 1) k = 1;
    if (k2 <= k) k2 = k + 1;
    int q = k / 4 > 0 ? k / 4 : 1, q2 = k2 / 4 > 0 ? k2 / 4 : 1;
    if (q >= k) q = k - 1;
    if (m < (uint32_t)(2 * k2 + 2)) {
        return 0;
    }
    float *rx = heap_caps_malloc(m * sizeof(float), MALLOC_CAP_SPIRAM);
    float *ry = heap_caps_malloc(m * sizeof(float), MALLOC_CAP_SPIRAM);
    float *x = heap_caps_malloc(m * sizeof(float), MALLOC_CAP_SPIRAM);
    float *y = heap_caps_malloc(m * sizeof(float), MALLOC_CAP_SPIRAM);
    gpx_turn_t *t = heap_caps_malloc(TURN_MAX * sizeof(gpx_turn_t), MALLOC_CAP_SPIRAM);
    uint32_t nt = 0;
    if (!rx || !ry || !x || !y || !t) {
        goto done;
    }

    /* resample by distance, then a small moving average */
    uint32_t j = 0;
    for (uint32_t i = 0; i < m; i++) {
        float d = i * step;
        while (j + 2 < n && pts[j + 1].dist < d) j++;
        const gpx_point_t *a = &pts[j], *b = &pts[j + 1];
        float sl = b->dist - a->dist;
        float f = sl > 0 ? (d - a->dist) / sl : 0;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        rx[i] = a->x + f * (b->x - a->x);
        ry[i] = a->y + f * (b->y - a->y);
    }
    for (int i = 0; i < (int)m; i++) {
        int a = i - TURN_SMOOTH < 0 ? 0 : i - TURN_SMOOTH;
        int b = i + TURN_SMOOTH >= (int)m ? (int)m - 1 : i + TURN_SMOOTH;
        float sx = 0, sy = 0;
        for (int q = a; q <= b; q++) {
            sx += rx[q];
            sy += ry[q];
        }
        x[i] = sx / (b - a + 1);
        y[i] = sy / (b - a + 1);
    }

    /* groups of samples turning the same way by >= TURN_MIN_DEG: one turn at the sharpest */
    bool in_group = false;
    int bi = 0;
    float best = 0;
    for (int i = k; i <= (int)m - 1 - k + 1; i++) {
        bool end = i > (int)m - 1 - k;
        float d = end ? 0 : heading_change(x, y, i, k, q);
        bool hit = fabsf(d) >= TURN_MIN_DEG;
        if (in_group && hit && same_way(d, best)) {
            if (fabsf(d) > fabsf(best)) {
                best = d;
                bi = i;
            }
            continue;
        }
        if (in_group) {
            /* the wider window catches the whole of a rounded corner */
            float ang = best;
            if (bi - k2 >= 0 && bi + k2 < (int)m) {
                float d2 = heading_change(x, y, bi, k2, q2);
                if (same_way(d2, best) && fabsf(d2) > fabsf(ang)) ang = d2;
            }
            float dist = bi * step;
            if (nt > 0 && dist - t[nt - 1].dist < TURN_MERGE_M && same_way(t[nt - 1].angle, ang)) {
                if (fabsf(ang) > abs(t[nt - 1].angle)) {
                    t[nt - 1] = (gpx_turn_t){ dist, x[bi], y[bi], (int16_t)lroundf(ang) };
                }
            } else if (nt < TURN_MAX) {
                t[nt++] = (gpx_turn_t){ dist, x[bi], y[bi], (int16_t)lroundf(ang) };
            }
            in_group = false;
        }
        if (hit) {
            in_group = true;
            best = d;
            bi = i;
        }
    }

done:
    heap_caps_free(rx);
    heap_caps_free(ry);
    heap_caps_free(x);
    heap_caps_free(y);
    if (nt == 0) {
        heap_caps_free(t);
        return 0;
    }
    gpx_turn_t *shrunk = heap_caps_realloc(t, nt * sizeof(gpx_turn_t), MALLOC_CAP_SPIRAM);
    *out = shrunk ? shrunk : t;
    return nt;
}

/* ---------------------------------------------------------------- elevation profile */

static float prof_at(const float *prof, uint32_t n, float step, float d)
{
    float fi = d / step;
    if (fi <= 0) return prof[0];
    if (fi >= n - 1) return prof[n - 1];
    uint32_t i = (uint32_t)fi;
    float t = fi - i;
    return prof[i] + t * (prof[i + 1] - prof[i]);
}

static void add_climb(profile_t *pr, uint32_t s, uint32_t e)
{
    const float *p = pr->prof;
    const float step = pr->step;
    uint32_t w = (uint32_t)(CLIMB_TRIM_M / step + 0.5f);
    if (w < 1) w = 1;
    const float trim_rise = CLIMB_TRIM_PCT / 100 * w * step;
    while (s + w <= e && p[s + w] - p[s] < trim_rise) s++;
    while (e >= s + w && p[e] - p[e - w] < trim_rise) e--;
    if (e <= s) {
        return;
    }
    float len = (e - s) * step;
    float gain = p[e] - p[s];
    float avg = gain / len * 100;
    if (len < CLIMB_MIN_LEN_M || gain < CLIMB_MIN_GAIN || avg < CLIMB_MIN_PCT || pr->nclimbs >= GPX_MAX_CLIMBS) {
        return;
    }
    float mx = avg;
    for (uint32_t i = s; i + w <= e; i++) {
        float g = (p[i + w] - p[i]) / (w * step) * 100;
        if (g > mx) mx = g;
    }
    pr->climbs[pr->nclimbs++] = (gpx_climb_t){
        .start_m = s * step, .len_m = len, .gain_m = gain, .avg_pct = avg, .max_pct = mx, .top_ele = p[e],
    };
}

static void find_climbs(profile_t *pr)
{
    const float *p = pr->prof;
    uint32_t s = 0, mx = 0;
    for (uint32_t j = 1; j < pr->n; j++) {
        if (p[j] > p[mx]) {
            mx = j;
        }
        if (mx == s && p[j] <= p[s]) {      /* still going down: move the foot */
            s = mx = j;
            continue;
        }
        bool last = j == pr->n - 1;
        if (p[mx] - p[j] > CLIMB_DROP_M || last) {
            add_climb(pr, s, mx);
            s = mx = j;
        }
    }
}

/* Fills pr from the points (elevation NaNs are filled in place). false = no elevation. */
static bool build_profile(gpx_point_t *pts, uint32_t n, profile_t *pr)
{
    memset(pr, 0, sizeof(*pr));
    float len = pts[n - 1].dist;
    float first = NAN;
    for (uint32_t i = 0; i < n && isnan(first); i++) first = pts[i].ele;
    if (isnan(first) || len < 2 * GPX_PROFILE_STEP_M) {
        return false;
    }
    /* fill missing elevations: carry the last known one */
    float last = first;
    for (uint32_t i = 0; i < n; i++) {
        if (isnan(pts[i].ele)) pts[i].ele = last;
        else last = pts[i].ele;
    }

    pr->step = fmaxf(GPX_PROFILE_STEP_M, len / (GPX_PROFILE_MAX - 1));
    pr->n = (uint32_t)(len / pr->step) + 1;
    float *raw = heap_caps_malloc(pr->n * sizeof(float), MALLOC_CAP_SPIRAM);
    double *sum = heap_caps_malloc((pr->n + 1) * sizeof(double), MALLOC_CAP_SPIRAM);
    pr->prof = heap_caps_malloc(pr->n * sizeof(float), MALLOC_CAP_SPIRAM);
    pr->asc = heap_caps_malloc(pr->n * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!raw || !sum || !pr->prof || !pr->asc) {
        heap_caps_free(raw);
        heap_caps_free(sum);
        heap_caps_free(pr->prof);
        heap_caps_free(pr->asc);
        memset(pr, 0, sizeof(*pr));
        return false;
    }

    /* resample along the distance */
    uint32_t k = 0;
    for (uint32_t i = 0; i < pr->n; i++) {
        float d = i * pr->step;
        while (k + 2 < n && pts[k + 1].dist < d) k++;
        const gpx_point_t *a = &pts[k], *b = &pts[k + 1];
        float sl = b->dist - a->dist;
        float t = sl > 0 ? (d - a->dist) / sl : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        raw[i] = a->ele + t * (b->ele - a->ele);
    }

    /* moving average, window +-SMOOTH_M (shrinks at the ends) */
    int w = (int)(SMOOTH_M / pr->step + 0.5f);
    sum[0] = 0;
    for (uint32_t i = 0; i < pr->n; i++) sum[i + 1] = sum[i] + raw[i];
    for (int i = 0; i < (int)pr->n; i++) {
        int a = i - w < 0 ? 0 : i - w;
        int b = i + w >= (int)pr->n ? (int)pr->n - 1 : i + w;
        pr->prof[i] = (float)((sum[b + 1] - sum[a]) / (b - a + 1));
    }
    heap_caps_free(raw);
    heap_caps_free(sum);

    pr->asc[0] = 0;
    for (uint32_t i = 1; i < pr->n; i++) {
        float d = pr->prof[i] - pr->prof[i - 1];
        pr->asc[i] = pr->asc[i - 1] + (d > 0 ? d : 0);
        if (d < 0) pr->descent -= d;
    }
    pr->ascent = pr->asc[pr->n - 1];
    find_climbs(pr);
    return true;
}

static void reset_nav_locked(void)
{
    s_st.matched = false;
    s_st.off_route = false;
    s_st.done_m = s_st.togo_m = s_st.off_m = 0;
    s_st.climb_cur = s_st.climb_next = -1;
    s_st.turn_next = -1;
    s_st.turn2 = false;
    s_st.joined = false;
    s_st.wrong_way = false;
    s_st.poi_next = -1;
    s_wrong_cnt = 0;
}

static void set_pois(gpx_poi_t *p, uint32_t n)
{
    xSemaphoreTake(s_pts_lock, portMAX_DELAY);
    gpx_poi_t *old = s_pois;
    s_pois = p;
    s_npois = n;
    xSemaphoreGive(s_pts_lock);
    heap_caps_free(old);
}

static float nearest(float px, float py, uint32_t a, uint32_t b, uint32_t *seg, float *tt);

static int poi_cmp(const void *a, const void *b)
{
    float x = ((const gpx_poi_t *)a)->along, y = ((const gpx_poi_t *)b)->along;
    if (x < 0) x = INFINITY;
    if (y < 0) y = INFINITY;
    return x < y ? -1 : x > y;
}

/* waypoints -> POIs placed along the (already set) track */
static void build_pois(const parser_t *ps)
{
    uint32_t n = ps->nw;
    gpx_poi_t *p = n ? heap_caps_calloc(n, sizeof(gpx_poi_t), MALLOC_CAP_SPIRAM) : NULL;
    if (p == NULL) {
        n = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        p[i].x = (float)((ps->wpts[i].lon - ps->lon0) * DEG * EARTH_R * ps->coslat);
        p[i].y = (float)((ps->wpts[i].lat - ps->lat0) * DEG * EARTH_R);
        strlcpy(p[i].name, ps->wpts[i].name, sizeof(p[i].name));
        uint32_t seg = 0;
        float t = 0;
        p[i].off = nearest(p[i].x, p[i].y, 0, s_npts - 1, &seg, &t);
        p[i].along = p[i].off <= GPX_POI_NEAR_M ? s_pts[seg].dist + t * (s_pts[seg + 1].dist - s_pts[seg].dist) : -1;
    }
    if (n) {
        qsort(p, n, sizeof(p[0]), poi_cmp);
    }
    set_pois(p, n);
}

static void do_unload(void)
{
    set_track(NULL, 0, NULL, NULL, 0);
    set_pois(NULL, 0);
    LOCK();
    s_have_origin = false;
    s_st.state = GPX_NONE;
    s_st.file[0] = s_st.name[0] = s_st.msg[0] = '\0';
    s_st.npoints = 0;
    s_st.has_profile = false;
    s_st.nclimbs = 0;
    s_st.nturns = 0;
    s_st.npois = 0;
    s_st.reversed = false;
    s_st.gen++;
    reset_nav_locked();
    UNLOCK();
}

static void load_failed(const char *msg)
{
    set_track(NULL, 0, NULL, NULL, 0);
    set_pois(NULL, 0);
    LOCK();
    s_have_origin = false;
    s_st.state = GPX_FAILED;
    s_st.name[0] = '\0';
    s_st.npoints = 0;
    s_st.has_profile = false;
    s_st.nclimbs = 0;
    s_st.nturns = 0;
    s_st.npois = 0;
    s_st.reversed = false;
    s_st.gen++;
    snprintf(s_st.msg, sizeof(s_st.msg), "%s", msg);
    reset_nav_locked();
    UNLOCK();
}

static uint32_t npois_near(void)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < s_npois; i++) k += s_pois[i].along >= 0;
    return k;
}

static void do_load(const char *file, bool reverse)
{
    LOCK();
    s_st.state = GPX_LOADING;
    snprintf(s_st.file, sizeof(s_st.file), "%s", file);
    s_st.msg[0] = '\0';
    UNLOCK();

    char path[sizeof(DIR_PATH) + GPX_NAME_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, file);
    int64_t t0 = esp_timer_get_time();
    FILE *f = waveshare_sd_card_mounted() ? fopen(path, "r") : NULL;
    if (f == NULL) {
        load_failed(waveshare_sd_card_mounted() ? "cannot open the file" : "no SD card");
        ESP_LOGW(TAG, "open %s failed", path);
        return;
    }

    parser_t *ps = heap_caps_calloc(1, sizeof(parser_t), MALLOC_CAP_SPIRAM);
    if (ps == NULL) {
        fclose(f);
        load_failed("out of memory");
        return;
    }
    size_t r;
    uint32_t bytes = 0;
    while ((r = fread(s_chunk, 1, sizeof(s_chunk), f)) > 0) {
        feed(ps, s_chunk, r);
        bytes += r;
    }
    bool read_err = ferror(f);
    fclose(f);

    if (ps->n < 2) {
        heap_caps_free(ps->pts);
        load_failed(read_err ? "read error" : "no track or route points");
        ESP_LOGW(TAG, "%s: %u points", path, (unsigned)ps->n);
        free(ps);
        return;
    }
    /* trim the spare capacity */
    gpx_point_t *pts = heap_caps_realloc(ps->pts, ps->n * sizeof(gpx_point_t), MALLOC_CAP_SPIRAM);
    if (pts == NULL) pts = ps->pts;
    if (reverse) {
        /* ride it end to start: flip the order, distances from the new start */
        for (uint32_t i = 0, j = ps->n - 1; i < j; i++, j--) {
            gpx_point_t tmp = pts[i];
            pts[i] = pts[j];
            pts[j] = tmp;
        }
        pts[0].dist = 0;
        for (uint32_t i = 1; i < ps->n; i++) {
            pts[i].dist = pts[i - 1].dist + hypotf(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        }
    }
    EXT_RAM_BSS_ATTR static profile_t pr;  /* ~1.2 KB, off the task stack; only this task loads */
    bool has_prof = build_profile(pts, ps->n, &pr);
    gpx_turn_t *turns;
    uint32_t nturns = find_turns(pts, ps->n, &turns);
    set_track(pts, ps->n, has_prof ? &pr : NULL, turns, nturns);
    build_pois(ps);

    LOCK();
    s_lat0 = ps->lat0;
    s_lon0 = ps->lon0;
    s_coslat = ps->coslat;
    s_have_origin = true;
    s_st.state = GPX_LOADED;
    s_st.gen++;
    snprintf(s_st.name, sizeof(s_st.name), "%s", ps->name);
    s_st.npoints = ps->n;
    s_st.truncated = ps->truncated;
    s_st.length_m = pts[ps->n - 1].dist;
    s_st.has_ele = ps->has_ele;
    s_st.ele_min = ps->ele_min;
    s_st.ele_max = ps->ele_max;
    s_st.climb_m = ps->climb;
    s_st.min_x = ps->min_x;
    s_st.max_x = ps->max_x;
    s_st.min_y = ps->min_y;
    s_st.max_y = ps->max_y;
    s_st.lat0 = ps->lat0;
    s_st.lon0 = ps->lon0;
    s_st.has_profile = has_prof;
    s_st.nturns = (uint16_t)nturns;
    s_st.npois = (uint8_t)s_npois;
    s_st.reversed = reverse;
    s_st.ascent_m = has_prof ? pr.ascent : 0;
    s_st.descent_m = has_prof ? pr.descent : 0;
    s_st.nclimbs = has_prof ? pr.nclimbs : 0;
    if (has_prof) {
        memcpy(s_st.climbs, pr.climbs, sizeof(pr.climbs[0]) * pr.nclimbs);
        s_st.climb_m = pr.ascent;       /* smoothed: less noise than the raw sum */
    }
    if (ps->truncated) {
        snprintf(s_st.msg, sizeof(s_st.msg), "only the first %u points loaded", (unsigned)ps->n);
    } else if (read_err) {
        snprintf(s_st.msg, sizeof(s_st.msg), "read error, file cut short");
    }
    reset_nav_locked();
    UNLOCK();
    ESP_LOGI(TAG, "%s: %u points, %.2f km, %u kB in %u ms", file, (unsigned)ps->n,
             pts[ps->n - 1].dist / 1000.0f, (unsigned)(bytes / 1024),
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    if (has_prof) {
        ESP_LOGI(TAG, "profile: %u samples every %.0f m, up %.0f m, down %.0f m, %u climbs",
                 (unsigned)pr.n, pr.step, pr.ascent, pr.descent, pr.nclimbs);
    }
    ESP_LOGI(TAG, "%u turns, %u waypoints (%u near the track)%s", (unsigned)nturns, (unsigned)ps->nw,
             (unsigned)npois_near(), reverse ? ", reversed" : "");
    free(ps);
}

/* ---------------------------------------------------------------- navigation */

/* nearest point on segments a..b-1 (segment i = points i, i+1) */
static float nearest(float px, float py, uint32_t a, uint32_t b, uint32_t *seg, float *tt)
{
    float best = INFINITY;
    for (uint32_t i = a; i < b; i++) {
        const gpx_point_t *p = &s_pts[i], *q = &s_pts[i + 1];
        float dx = q->x - p->x, dy = q->y - p->y;
        float l2 = dx * dx + dy * dy;
        float t = l2 > 0 ? ((px - p->x) * dx + (py - p->y) * dy) / l2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float ex = p->x + t * dx - px, ey = p->y + t * dy - py;
        float d2 = ex * ex + ey * ey;
        if (d2 < best) {
            best = d2;
            *seg = i;
            *tt = t;
        }
    }
    return sqrtf(best);
}

/* elevation figures at distance d along the track (lock held) */
static void elevation_here_locked(float d)
{
    s_st.climb_cur = s_st.climb_next = -1;
    if (s_prof == NULL) {
        return;
    }
    s_st.ele_here = prof_at(s_prof, s_nprof, s_step, d);
    float a = prof_at(s_prof, s_nprof, s_step, d - GRADE_M);
    float b = prof_at(s_prof, s_nprof, s_step, d + GRADE_M);
    float span = fminf(d + GRADE_M, s_st.length_m) - fmaxf(d - GRADE_M, 0);
    s_st.grade_pct = span > 1 ? (b - a) / span * 100 : 0;
    s_st.ascent_left_m = s_asc[s_nprof - 1] - prof_at(s_asc, s_nprof, s_step, d);
    for (int i = 0; i < s_st.nclimbs; i++) {
        const gpx_climb_t *c = &s_st.climbs[i];
        if (d >= c->start_m && d <= c->start_m + c->len_m) {
            s_st.climb_cur = (int8_t)i;
            s_st.climb_left_m = c->start_m + c->len_m - d;
            s_st.climb_left_up_m = fmaxf(0, c->top_ele - s_st.ele_here);
        } else if (c->start_m > d && s_st.climb_next < 0) {
            s_st.climb_next = (int8_t)i;
            s_st.climb_next_in_m = c->start_m - d;
        }
    }
}

/* next turn at distance d along the track (lock held) */
static void turns_here_locked(float d)
{
    s_st.turn_next = -1;
    s_st.turn2 = false;
    /* first turn not yet passed by TURN_PASSED_M (sorted: binary search) */
    uint32_t lo = 0, hi = s_nturns;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s_turns[mid].dist + TURN_PASSED_M <= d) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= s_nturns) {
        return;
    }
    const gpx_turn_t *t = &s_turns[lo];
    s_st.turn_next = (int16_t)lo;
    s_st.turn_in_m = t->dist - d;
    s_st.turn_angle = t->angle;
    s_st.turn_x = t->x;
    s_st.turn_y = t->y;
    if (lo + 1 < s_nturns && s_turns[lo + 1].dist - t->dist < TURN2_NEAR_M) {
        s_st.turn2 = true;
        s_st.turn2_after_m = s_turns[lo + 1].dist - t->dist;
        s_st.turn2_angle = s_turns[lo + 1].angle;
    }
}

static void nav_update(void)
{
    gps_svc_get_status(&s_gps);
    bool fix = s_gps.fix;

    LOCK();
    bool loaded = s_st.state == GPX_LOADED && s_npts >= 2;
    bool matched = s_st.matched;
    uint32_t last = s_st.seg;
    UNLOCK();

    float px = 0, py = 0;
    if (fix && s_have_origin) {
        px = (float)((s_gps.lon - s_lon0) * DEG * EARTH_R * s_coslat);
        py = (float)((s_gps.lat - s_lat0) * DEG * EARTH_R);
    }
    uint32_t seg = 0;
    float t = 0, off = 0;
    bool ok = false;
    if (fix && loaded) {
        uint32_t nseg = s_npts - 1;
        if (matched) {
            uint32_t a = last > WINDOW_BACK ? last - WINDOW_BACK : 0;
            uint32_t b = last + WINDOW_AHEAD < nseg ? last + WINDOW_AHEAD : nseg;
            off = nearest(px, py, a, b, &seg, &t);
            ok = off <= REMATCH_M;
        }
        if (!ok) {
            off = nearest(px, py, 0, nseg, &seg, &t);
            ok = true;
        }
    }

    LOCK();
    s_st.fix = fix;
    s_st.speed_kmh = s_gps.speed_kmh;
    s_st.course_deg = s_gps.course_deg;
    s_st.pos_x = px;
    s_st.pos_y = py;
    s_st.matched = ok;
    if (ok) {
        const gpx_point_t *p = &s_pts[seg], *q = &s_pts[seg + 1];
        s_st.seg = seg;
        s_st.near_x = p->x + t * (q->x - p->x);
        s_st.near_y = p->y + t * (q->y - p->y);
        s_st.off_m = off;
        s_st.off_route = off > GPX_OFF_ROUTE_M;
        s_st.done_m = p->dist + t * (q->dist - p->dist);
        s_st.togo_m = s_st.length_m - s_st.done_m;
        if (!s_st.joined && !s_st.off_route) {
            s_st.joined = true;             /* where this ride met the track (on it, not near it) */
            s_st.start_m = s_st.done_m;
            s_st.start_seg = seg;
        }
        s_st.rejoin_bearing = atan2f(s_st.near_x - px, s_st.near_y - py) * (180.0f / (float)M_PI);
        if (s_gps.speed_kmh >= WRONG_WAY_KMH && s_gps.course_deg >= 0) {
            float h = atan2f(q->x - p->x, q->y - p->y) * (180.0f / (float)M_PI);
            float diff = fabsf(wrap180(s_gps.course_deg - h));
            if (diff > WRONG_WAY_DEG) s_wrong_cnt++;
            else if (diff < RIGHT_WAY_DEG) s_wrong_cnt = 0;
            s_st.wrong_way = !s_st.off_route && s_wrong_cnt >= WRONG_WAY_S;
        }
        s_st.poi_next = -1;
        for (uint32_t i = 0; i < s_npois && s_pois[i].along >= 0; i++) {
            if (s_pois[i].along > s_st.done_m - POI_PASSED_M) {
                s_st.poi_next = (int8_t)i;
                s_st.poi_in_m = s_pois[i].along - s_st.done_m;
                break;
            }
        }
        elevation_here_locked(s_st.done_m);
        turns_here_locked(s_st.done_m);
    }
    UNLOCK();
}

/* ---------------------------------------------------------------- task */

static void gpx_task(void *arg)
{
    int64_t last_nav = 0;
    cmd_t c;
    while (1) {
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(NAV_PERIOD_MS)) == pdTRUE) {
            switch (c.type) {
            case CMD_RESCAN: do_rescan(); break;
            case CMD_LOAD:   do_load(c.file, c.reverse); break;
            case CMD_UNLOAD: do_unload(); break;
            case CMD_REJOIN:
                LOCK();
                s_st.joined = false;
                UNLOCK();
                break;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_nav >= NAV_PERIOD_MS * 1000LL) {
            last_nav = now;
            nav_update();
        }
    }
}

/* ---------------------------------------------------------------- API */

static esp_err_t send(cmd_type_t type, const char *file, bool reverse)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    cmd_t c = { .type = type, .reverse = reverse };
    if (file) {
        snprintf(c.file, sizeof(c.file), "%s", file);
    }
    return xQueueSend(s_q, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t gpx_svc_init(const char *autoload, bool reverse)
{
    s_lock = xSemaphoreCreateMutex();
    s_pts_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreateWithCaps(4, sizeof(cmd_t), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_pts_lock == NULL || s_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_st.course_deg = -1;
    s_st.poi_next = -1;
    /* stack in PSRAM: SD card and math only, never writes flash */
    if (xTaskCreatePinnedToCoreWithCaps(gpx_task, "gpx_svc", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    send(CMD_RESCAN, NULL, false);
    if (autoload && autoload[0]) {
        send(CMD_LOAD, autoload, reverse);
    }
    return ESP_OK;
}

esp_err_t gpx_svc_rescan(void)
{
    return send(CMD_RESCAN, NULL, false);
}

esp_err_t gpx_svc_load(const char *file, bool reverse)
{
    if (file == NULL || !file[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return send(CMD_LOAD, file, reverse);
}

esp_err_t gpx_svc_unload(void)
{
    return send(CMD_UNLOAD, NULL, false);
}

esp_err_t gpx_svc_rejoin(void)
{
    return send(CMD_REJOIN, NULL, false);
}

void gpx_svc_get_status(gpx_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    LOCK();
    *out = s_st;
    UNLOCK();
}

const gpx_point_t *gpx_svc_points_lock(uint32_t *n, uint32_t *gen)
{
    if (s_pts_lock == NULL) {
        *n = 0;
        return NULL;
    }
    xSemaphoreTake(s_pts_lock, portMAX_DELAY);
    *n = s_npts;
    if (gen) {
        LOCK();
        *gen = s_st.gen;
        UNLOCK();
    }
    return s_pts;
}

void gpx_svc_points_unlock(void)
{
    xSemaphoreGive(s_pts_lock);
}

const gpx_poi_t *gpx_svc_pois(uint32_t *n)
{
    *n = s_npois;
    return s_pois;
}

const float *gpx_svc_profile(uint32_t *n, float *step)
{
    *n = s_nprof;
    *step = s_step;
    return s_prof;
}

bool gpx_svc_to_xy(double lat, double lon, float *x, float *y)
{
    LOCK();
    bool ok = s_have_origin;
    if (ok) {
        *x = (float)((lon - s_lon0) * DEG * EARTH_R * s_coslat);
        *y = (float)((lat - s_lat0) * DEG * EARTH_R);
    }
    UNLOCK();
    return ok;
}
