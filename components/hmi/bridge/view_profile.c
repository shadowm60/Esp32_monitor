/*
 * Elevation page: profile of the loaded GPX, climbs, live climb info.
 *   profile_summary, profile_live, profile_climbs (profile_climb_table, monospace),
 *   profile_view_text
 *   profile_chart (EEZ identifier) empty panel; this view draws into it from
 *                 its draw event: one column per pixel, coloured by gradient,
 *                 the part done dimmed, climb numbers, the position line.
 * View: the whole route, or the next 6 km (1 km behind the position).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_attr.h"
#include "lvgl.h"
#include "ui.h"
#include "screens.h"
#include "bridge.h"
#include "gpx_svc.h"

#define MAX_COLS        800
#define LEFT_M          44      /* px for the elevation labels */
#define BOTTOM_M        18      /* px for the distance labels */
#define TOP_M           18      /* px for the climb numbers */
#define AHEAD_M         5000.0f
#define BEHIND_M        1000.0f
#define COL_GRADE_M     25.0f   /* gradient of a column: at least +- this */

static uivar_t v_summary = UIVAR("profile_summary");
static uivar_t v_live = UIVAR("profile_live");
static uivar_t v_climbs = UIVAR("profile_climbs");
static uivar_t v_view = UIVAR("profile_view_text");

EXT_RAM_BSS_ATTR static gpx_status_t s_g;
EXT_RAM_BSS_ATTR static float  s_col_ele[MAX_COLS];
EXT_RAM_BSS_ATTR static int8_t s_col_grade[MAX_COLS];
EXT_RAM_BSS_ATTR static char   s_table[GPX_MAX_CLIMBS * 48 + 64];

static lv_obj_t *s_chart;
static bool  s_ahead;
static int   s_ncols;               /* 0 = nothing to draw */
static float s_d0, s_d1;            /* distance window */
static float s_emin, s_emax;        /* elevation axis */
static float s_done = -1;           /* position, -1 = none */

typedef struct {
    uint32_t gen;
    bool     ahead, matched, has_profile;
    int32_t  done_px;
    int8_t   cur;
} pkey_t;
static pkey_t s_key;
static bool  s_key_valid;
static int8_t s_table_cur = -2;
static uint32_t s_table_gen = UINT32_MAX;

/* ---------------------------------------------------------------- colours */

static lv_color_t grade_color(int g)
{
    if (g < -1)  return lv_color_hex(0x4a5a6a);     /* down */
    if (g < 3)   return lv_color_hex(0x3ecf6e);     /* flat */
    if (g < 6)   return lv_color_hex(0xe8c547);
    if (g < 9)   return lv_color_hex(0xf08c2e);
    if (g < 12)  return lv_color_hex(0xe5484d);
    return lv_color_hex(0xa33ad1);                  /* wall */
}

/* ---------------------------------------------------------------- data */

static float prof_at(const float *p, uint32_t n, float step, float d)
{
    float fi = d / step;
    if (fi <= 0) return p[0];
    if (fi >= n - 1) return p[n - 1];
    uint32_t i = (uint32_t)fi;
    return p[i] + (fi - i) * (p[i + 1] - p[i]);
}

static void compute_columns(const gpx_status_t *g)
{
    s_ncols = 0;
    if (s_chart == NULL || g->state != GPX_LOADED || !g->has_profile) {
        return;
    }
    int w = lv_obj_get_content_width(s_chart) - LEFT_M - 6;
    if (w < 10) return;
    if (w > MAX_COLS) w = MAX_COLS;

    float len = g->length_m;
    s_done = g->matched ? g->done_m : -1;
    if (s_ahead) {
        float span = BEHIND_M + AHEAD_M;
        s_d0 = (s_done >= 0 ? s_done : 0) - BEHIND_M;
        if (s_d0 + span > len) s_d0 = len - span;
        if (s_d0 < 0) s_d0 = 0;
        s_d1 = fminf(len, s_d0 + span);
    } else {
        s_d0 = 0;
        s_d1 = len;
    }

    uint32_t n;
    float step;
    gpx_svc_points_lock(&n, NULL);
    const float *p = gpx_svc_profile(&n, &step);
    if (p == NULL || n < 2) {
        gpx_svc_points_unlock();
        return;
    }
    float colw = (s_d1 - s_d0) / w;
    s_emin = INFINITY;
    s_emax = -INFINITY;
    for (int c = 0; c < w; c++) {
        float a = s_d0 + c * colw, b = a + colw;
        /* highest sample in the column, so short peaks don't vanish */
        float e = prof_at(p, n, step, (a + b) / 2);
        for (uint32_t i = (uint32_t)ceilf(a / step); i * step < b && i < n; i++) {
            if (p[i] > e) e = p[i];
        }
        float half = fmaxf(colw / 2, COL_GRADE_M);
        float mid = (a + b) / 2;
        float lo = fmaxf(0, mid - half), hi = fminf(len, mid + half);
        float gr = hi - lo > 1 ? (prof_at(p, n, step, hi) - prof_at(p, n, step, lo)) / (hi - lo) * 100 : 0;
        s_col_ele[c] = e;
        s_col_grade[c] = (int8_t)fmaxf(-100, fminf(100, roundf(gr)));
        s_emin = fminf(s_emin, e);
        s_emax = fmaxf(s_emax, e);
    }
    gpx_svc_points_unlock();

    /* axis: at least 50 m tall, a little room at the top and bottom */
    float range = fmaxf(50, s_emax - s_emin);
    float mid = (s_emax + s_emin) / 2;
    s_emin = mid - range * 0.55f;
    s_emax = mid + range * 0.55f;
    s_ncols = w;
}

/* ---------------------------------------------------------------- drawing */

static void draw_text(lv_draw_ctx_t *ctx, const char *txt, lv_coord_t x, lv_coord_t y, lv_color_t col,
                      lv_text_align_t align)
{
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_montserrat_12;
    ld.color = col;
    ld.align = align;
    lv_area_t a = { x, y, x + 60, y + 14 };
    if (align == LV_TEXT_ALIGN_RIGHT) {
        a.x1 = x - 60;
        a.x2 = x;
    } else if (align == LV_TEXT_ALIGN_CENTER) {
        a.x1 = x - 150;         /* also the centred messages */
        a.x2 = x + 150;
    }
    lv_draw_label(ctx, &ld, &a, txt, NULL);
}

static void fmt_dist(char *b, size_t n, float m)
{
    if (m < 10000) snprintf(b, n, "%.1f km", m / 1000);
    else           snprintf(b, n, "%.0f km", m / 1000);
}

static void chart_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_content_coords(s_chart, &box);
    const lv_color_t grey = lv_color_hex(0x8a96a3);

    if (s_ncols == 0) {
        const char *msg = s_g.state != GPX_LOADED ? "No track loaded (Route page)"
                        : "This GPX file has no elevation data";
        draw_text(ctx, msg, (box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2 - 7, grey, LV_TEXT_ALIGN_CENTER);
        return;
    }
    lv_coord_t x0 = box.x1 + LEFT_M;
    lv_coord_t y0 = box.y1 + TOP_M;                 /* top of the plot */
    lv_coord_t yb = box.y2 - BOTTOM_M;              /* bottom of the plot */
    float ppm = (yb - y0) / (s_emax - s_emin);      /* px per metre of height */
    float colw = (s_d1 - s_d0) / s_ncols;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    for (int c = 0; c < s_ncols; c++) {
        lv_coord_t yt = (lv_coord_t)(yb - (s_col_ele[c] - s_emin) * ppm);
        if (yt < y0) yt = y0;
        lv_area_t a = { x0 + c, yt, x0 + c, yb };
        rd.bg_color = grade_color(s_col_grade[c]);
        rd.bg_opa = s_done >= 0 && s_d0 + (c + 0.5f) * colw < s_done ? LV_OPA_40 : LV_OPA_COVER;
        lv_draw_rect(ctx, &rd, &a);
    }

    /* axes: elevation at top / bottom, distance at both ends and the middle */
    char t[24];
    snprintf(t, sizeof(t), "%.0f m", s_emax);
    draw_text(ctx, t, x0 - 4, y0 - 6, grey, LV_TEXT_ALIGN_RIGHT);
    snprintf(t, sizeof(t), "%.0f m", s_emin);
    draw_text(ctx, t, x0 - 4, yb - 8, grey, LV_TEXT_ALIGN_RIGHT);
    fmt_dist(t, sizeof(t), s_d0);
    draw_text(ctx, t, x0, yb + 2, grey, LV_TEXT_ALIGN_LEFT);
    fmt_dist(t, sizeof(t), (s_d0 + s_d1) / 2);
    draw_text(ctx, t, x0 + s_ncols / 2, yb + 2, grey, LV_TEXT_ALIGN_CENTER);
    fmt_dist(t, sizeof(t), s_d1);
    draw_text(ctx, t, x0 + s_ncols, yb + 2, grey, LV_TEXT_ALIGN_RIGHT);

    /* climb numbers above their tops */
    for (int i = 0; i < s_g.nclimbs; i++) {
        const gpx_climb_t *c = &s_g.climbs[i];
        float top = c->start_m + c->len_m;
        if (top < s_d0 || top > s_d1) continue;
        lv_coord_t x = (lv_coord_t)(x0 + (top - s_d0) / colw);
        lv_coord_t y = (lv_coord_t)(yb - (c->top_ele - s_emin) * ppm) - 16;
        if (y < box.y1) y = box.y1;
        snprintf(t, sizeof(t), "%d", i + 1);
        draw_text(ctx, t, x, y, lv_color_white(), LV_TEXT_ALIGN_CENTER);
    }

    /* position */
    if (s_done >= s_d0 && s_done <= s_d1) {
        lv_coord_t x = (lv_coord_t)(x0 + (s_done - s_d0) / colw);
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = lv_color_white();
        ld.width = 2;
        lv_point_t a = { x, y0 }, b = { x, yb };
        lv_draw_line(ctx, &ld, &a, &b);
    }
}

/* ---------------------------------------------------------------- texts */

static const char *cat_name(const gpx_climb_t *c)
{
    static const char *names[] = { "-", "1", "2", "3", "4", "HC" };
    return names[gpx_climb_category(c)];
}

static void update_table(const gpx_status_t *g)
{
    int8_t cur = g->matched ? g->climb_cur : -1;
    if (g->gen == s_table_gen && cur == s_table_cur) {
        return;
    }
    s_table_gen = g->gen;
    s_table_cur = cur;
    if (!g->has_profile || g->nclimbs == 0) {
        uv_str(&v_climbs, g->has_profile ? "No climbs (>= 300 m at >= 3 %)" : "");
        return;
    }
    int len = snprintf(s_table, sizeof(s_table), "  #  at km    long  up m  avg%%  max%% cat\n");
    for (int i = 0; i < g->nclimbs && len < (int)sizeof(s_table) - 48; i++) {
        const gpx_climb_t *c = &g->climbs[i];
        len += snprintf(s_table + len, sizeof(s_table) - len, "%c%3d %6.1f %5.2fkm %5.0f %5.1f %5.1f %3s\n",
                        i == cur ? '>' : ' ', i + 1, c->start_m / 1000, c->len_m / 1000, c->gain_m,
                        c->avg_pct, c->max_pct, cat_name(c));
    }
    s_table[len - 1] = '\0';
    uv_str(&v_climbs, s_table);
}

static void update_texts(const gpx_status_t *g)
{
    uv_str(&v_view, s_ahead ? "Whole route" : "Next 6 km");
    if (g->state != GPX_LOADED) {
        uv_str(&v_summary, "No track loaded");
        uv_str(&v_live, "");
        return;
    }
    if (!g->has_profile) {
        uv_str(&v_summary, "No elevation in this GPX file");
        uv_str(&v_live, "");
        return;
    }
    uv_strf(&v_summary, "Up %.0f m, down %.0f m, %u climb%s\nLowest %.0f m, highest %.0f m",
            g->ascent_m, g->descent_m, g->nclimbs, g->nclimbs == 1 ? "" : "s", g->ele_min, g->ele_max);

    if (!g->fix) {
        uv_str(&v_live, "No GPS fix");
        return;
    }
    if (!g->matched) {
        uv_str(&v_live, "-");
        return;
    }
    char cl[96] = "";
    if (g->climb_cur >= 0) {
        const gpx_climb_t *c = &g->climbs[g->climb_cur];
        snprintf(cl, sizeof(cl), "Climb %d/%u: %.0f m, %.0f m up\nto the top (avg %.1f %%)", g->climb_cur + 1,
                 g->nclimbs, g->climb_left_m, g->climb_left_up_m, c->avg_pct);
    } else if (g->climb_next >= 0) {
        const gpx_climb_t *c = &g->climbs[g->climb_next];
        snprintf(cl, sizeof(cl), "Next climb in %.1f km:\n%.1f km at %.1f %%, %.0f m up", g->climb_next_in_m / 1000,
                 c->len_m / 1000, c->avg_pct, c->gain_m);
    } else {
        snprintf(cl, sizeof(cl), "No more climbs");
    }
    uv_strf(&v_live, "Here %.0f m, %+.1f %%\n%s\nStill to climb %.0f m", g->ele_here, g->grade_pct, cl,
            g->ascent_left_m);
}

/* ---------------------------------------------------------------- view API */

void view_profile_init(void)
{
    s_chart = objects.profile_chart;
    if (s_chart == NULL) {
        return;
    }
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chart, chart_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
}

void view_profile_update(bool visible)
{
    if (!visible) {
        s_key_valid = false;
        return;
    }
    gpx_svc_get_status(&s_g);
    update_texts(&s_g);
    update_table(&s_g);

    /* the position moves the picture by whole pixels: redraw only then */
    float span = s_ahead ? BEHIND_M + AHEAD_M : fmaxf(1, s_g.length_m);
    pkey_t k;
    memset(&k, 0, sizeof(k));       /* compared with memcmp: no stray padding */
    k.gen = s_g.gen;
    k.ahead = s_ahead;
    k.matched = s_g.matched;
    k.has_profile = s_g.has_profile;
    k.done_px = s_g.matched ? (int32_t)(s_g.done_m / span * MAX_COLS) : -1;
    k.cur = s_g.climb_cur;
    if (s_chart && (!s_key_valid || memcmp(&k, &s_key, sizeof(k)) != 0)) {
        s_key = k;
        s_key_valid = true;
        compute_columns(&s_g);
        lv_obj_invalidate(s_chart);
    }
}

void view_profile_toggle_view(void)
{
    s_ahead = !s_ahead;
}
