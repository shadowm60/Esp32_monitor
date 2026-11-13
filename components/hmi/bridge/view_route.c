/*
 * Route page: GPX file selector, map, progress along the track.
 *   gpx_list / gpx_sel (two-way), gpx_info, route_big, route_text, route_view_text
 *   route_map  (EEZ identifier) empty panel; this view draws into it:
 *              the track (blue), the part done (green), the GPS position
 *              (red dot + course), a scale bar. North is up.
 * View: "whole" fits the track, "follow" centres on the position at a zoom
 * level (+ / - switch to follow).
 *
 * Turns (gpx_svc, from the track's shape): an arrow box over the map's top
 * left corner when the next turn is closer than TURN_SHOW_M, a ring on the
 * map at the turn. With the screen timeout on, the screen is switched on
 * (and this page shown) WAKE_S seconds / at least WAKE_MIN_M before a turn,
 * and when going off route; it stays on until the turn is passed.
 *
 * Off route the box shows an arrow to the nearest point of the track
 * (relative to the direction of travel when moving, else north up like the
 * map); riding against the track it shows a U-turn. Progress counts from
 * the join point (where this ride met the track). POIs (GPX waypoints) are
 * yellow dots, named when zoomed in and for the next one ahead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "ui.h"
#include "screens.h"
#include "bridge.h"
#include "gpx_svc.h"
#include "settings.h"

#define MAX_LINE_PTS    3000        /* per polyline, decimated to ~2 px steps */
#define COORD_LIMIT     8000        /* keep lv_coord_t far from overflow */
#define MARKER_SIZE     18
#define COURSE_LEN      30
#define SCALE_TARGET_PX 90
#define TURN_SHOW_M     1000.0f
#define WAKE_MIN_M      150.0f
#define WAKE_S          25.0f
#define BOX_W           110
#define BOX_H           132

#define COL_TRACK   0x2d7ff9
#define COL_DONE    0x3ecf6e
#define COL_POS     0xff4d4d
#define COL_TEXT    0x8a96a3

static uivar_t v_list = UIVAR("gpx_list");
static uivar_t v_sel = UIVAR("gpx_sel");
static uivar_t v_info = UIVAR("gpx_info");
static uivar_t v_big = UIVAR("route_big");
static uivar_t v_text = UIVAR("route_text");
static uivar_t v_view = UIVAR("route_view_text");

static const float ZOOMS[] = { 0.25f, 0.5f, 1, 2, 4, 8, 16, 32, 64, 128, 256 };   /* m per px */
#define NZOOMS ((int)(sizeof(ZOOMS) / sizeof(ZOOMS[0])))

EXT_RAM_BSS_ATTR static gpx_status_t s_g;
static uint32_t s_list_gen = UINT32_MAX;

static lv_obj_t *s_map, *s_track, *s_done, *s_course, *s_marker, *s_msg, *s_scale, *s_scale_lbl;
static lv_obj_t *s_box, *s_box_lbl, *s_ring;
static arrow_t  s_arrow;
static char     s_box_text[16];
static bool    s_wake_turn, s_wake_off;
static lv_point_t *s_tpts, *s_dpts;
static lv_point_t s_cpts[2], s_spts[4];
static bool  s_follow;
static int   s_zoom = 3;
static float s_scale_mpp = 1;       /* metres per pixel last drawn */

/* what the picture depends on: redraw only when it changes */
typedef struct {
    uint32_t gen;
    bool     follow, fix, matched;
    int      zoom;
    int32_t  pos_x, pos_y;          /* decimetres */
    uint32_t seg;
    int16_t  course;
    int16_t  turn;
} draw_key_t;
static draw_key_t s_key;
static bool s_key_valid;

/* ---------------------------------------------------------------- drawing objects */

static lv_obj_t *mk_line(lv_color_t c, int w)
{
    lv_obj_t *l = lv_line_create(s_map);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static void poi_draw_cb(lv_event_t *e);

static void build_objects(void)
{
    s_map = objects.route_map;
    s_tpts = heap_caps_malloc(MAX_LINE_PTS * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    s_dpts = heap_caps_malloc(MAX_LINE_PTS * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    if (s_map == NULL || s_tpts == NULL || s_dpts == NULL) {
        s_map = NULL;
        return;
    }
    lv_obj_clear_flag(s_map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(s_map);
    lv_obj_add_event_cb(s_map, poi_draw_cb, LV_EVENT_DRAW_POST_END, NULL);

    s_track = mk_line(lv_color_hex(COL_TRACK), 4);
    s_done = mk_line(lv_color_hex(COL_DONE), 5);
    s_course = mk_line(lv_color_white(), 3);
    s_scale = mk_line(lv_color_hex(COL_TEXT), 2);

    s_marker = lv_obj_create(s_map);
    lv_obj_remove_style_all(s_marker);
    lv_obj_set_size(s_marker, MARKER_SIZE, MARKER_SIZE);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_marker, lv_color_hex(COL_POS), 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_marker, 3, 0);
    lv_obj_clear_flag(s_marker, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);

    s_scale_lbl = lv_label_create(s_map);
    lv_obj_set_style_text_color(s_scale_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(s_scale_lbl, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(s_scale_lbl, LV_OBJ_FLAG_HIDDEN);

    s_ring = lv_obj_create(s_map);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, 22, 22);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_ring, 3, 0);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);

    /* turn box: arrow + distance, top left over the map */
    s_box = lv_obj_create(s_map);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, BOX_W, BOX_H);
    lv_obj_set_pos(s_box, 8, 8);
    lv_obj_set_style_bg_color(s_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_box, 10, 0);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    arrow_init(&s_arrow, s_box, BOX_W / 2.0f, 52, 1.0f, 9);
    s_box_lbl = lv_label_create(s_box);
    lv_obj_set_style_text_color(s_box_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_box_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_box_lbl, BOX_W);
    lv_obj_set_style_text_align(s_box_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_box_lbl, 0, BOX_H - 32);

    s_msg = lv_label_create(s_map);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_msg);
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_coord_t clampc(float v)
{
    return (lv_coord_t)(v < -COORD_LIMIT ? -COORD_LIMIT : v > COORD_LIMIT ? COORD_LIMIT : v);
}

/* ---------------------------------------------------------------- map */

typedef struct {
    float cx, cy, mpp;      /* centre (track frame, m), metres per pixel */
    int   w, h;
} view_t;

static view_t s_view;       /* last drawn: the POI layer uses it */
static bool   s_view_ok;

static lv_point_t to_px(const view_t *v, float x, float y)
{
    lv_point_t p = { clampc(v->w / 2.0f + (x - v->cx) / v->mpp),
                     clampc(v->h / 2.0f - (y - v->cy) / v->mpp) };
    return p;
}

/* polyline of points [begin, end) plus an optional extra point, decimated */
static uint32_t make_line(const view_t *v, const gpx_point_t *pts, uint32_t begin, uint32_t end, lv_point_t *out,
                          const lv_point_t *extra)
{
    uint32_t stride = (end - begin) / (MAX_LINE_PTS - 2) + 1;
    uint32_t n = 0;
    for (uint32_t i = begin; i < end && n < MAX_LINE_PTS - 2; i += stride) {
        lv_point_t p = to_px(v, pts[i].x, pts[i].y);
        if (n > 0 && i + stride < end && abs(p.x - out[n - 1].x) + abs(p.y - out[n - 1].y) < 2) {
            continue;
        }
        out[n++] = p;
    }
    if (end > begin && (end - 1 - begin) % stride != 0) {
        out[n++] = to_px(v, pts[end - 1].x, pts[end - 1].y);    /* keep the last point */
    }
    if (extra) {
        out[n++] = *extra;
    }
    return n;
}

static float nice_dist(float m)
{
    float e = powf(10, floorf(log10f(m)));
    float f = m / e;
    return (f < 1.5f ? 1 : f < 3.5f ? 2 : f < 7.5f ? 5 : 10) * e;
}

static void draw_scale(const view_t *v)
{
    float d = nice_dist(SCALE_TARGET_PX * v->mpp);
    int len = (int)(d / v->mpp);
    int x0 = 12, y0 = v->h - 14;
    s_spts[0] = (lv_point_t){ x0, y0 - 6 };
    s_spts[1] = (lv_point_t){ x0, y0 };
    s_spts[2] = (lv_point_t){ x0 + len, y0 };
    s_spts[3] = (lv_point_t){ x0 + len, y0 - 6 };
    lv_obj_set_size(s_scale, v->w, v->h);
    lv_line_set_points(s_scale, s_spts, 4);
    show(s_scale, true);
    if (d >= 1000) lv_label_set_text_fmt(s_scale_lbl, "%g km", d / 1000);
    else           lv_label_set_text_fmt(s_scale_lbl, "%g m", d);
    lv_obj_set_pos(s_scale_lbl, x0 + len + 6, y0 - 12);
    show(s_scale_lbl, true);
}

static void draw_map(const gpx_status_t *g)
{
    if (s_map == NULL) {
        return;
    }
    bool loaded = g->state == GPX_LOADED;
    if (!loaded) {
        show(s_track, false);
        show(s_done, false);
        show(s_course, false);
        show(s_marker, false);
        show(s_scale, false);
        show(s_scale_lbl, false);
        show(s_ring, false);
        s_view_ok = false;
        show(s_msg, true);
        lv_label_set_text(s_msg, g->state == GPX_LOADING ? "Loading..." : "No track loaded\n\n"
                          "Put .gpx files in /gpx on the SD card,\npick one and press Load.");
        lv_obj_center(s_msg);
        return;
    }
    show(s_msg, false);

    view_t v = { .w = lv_obj_get_content_width(s_map), .h = lv_obj_get_content_height(s_map) };
    if (s_follow) {
        v.mpp = ZOOMS[s_zoom];
        v.cx = g->fix ? g->pos_x : (g->min_x + g->max_x) / 2;
        v.cy = g->fix ? g->pos_y : (g->min_y + g->max_y) / 2;
    } else {
        /* whole track, plus the position if it is close enough to show */
        float x0 = g->min_x, x1 = g->max_x, y0 = g->min_y, y1 = g->max_y;
        if (g->fix && g->off_m < 2000) {
            x0 = fminf(x0, g->pos_x); x1 = fmaxf(x1, g->pos_x);
            y0 = fminf(y0, g->pos_y); y1 = fmaxf(y1, g->pos_y);
        }
        v.cx = (x0 + x1) / 2;
        v.cy = (y0 + y1) / 2;
        v.mpp = fmaxf(fmaxf((x1 - x0) / (v.w - 40), (y1 - y0) / (v.h - 40)), 0.25f);
    }
    s_scale_mpp = v.mpp;
    s_view = v;
    s_view_ok = true;

    uint32_t n = 0;
    const gpx_point_t *pts = gpx_svc_points_lock(&n, NULL);
    if (pts == NULL || n < 2) {
        gpx_svc_points_unlock();
        return;
    }
    uint32_t nt = make_line(&v, pts, 0, n, s_tpts, NULL);
    uint32_t nd = 0;
    if (g->matched && g->joined && g->seg + 1 < n && g->start_seg <= g->seg) {
        lv_point_t near = to_px(&v, g->near_x, g->near_y);
        nd = make_line(&v, pts, g->start_seg, g->seg + 1, s_dpts, &near);     /* from the join point */
    }
    gpx_svc_points_unlock();

    lv_obj_set_size(s_track, v.w, v.h);
    lv_line_set_points(s_track, s_tpts, nt);
    show(s_track, true);
    lv_obj_set_size(s_done, v.w, v.h);
    lv_line_set_points(s_done, s_dpts, nd);
    show(s_done, nd >= 2);

    if (g->fix) {
        lv_point_t p = to_px(&v, g->pos_x, g->pos_y);
        lv_obj_set_pos(s_marker, p.x - MARKER_SIZE / 2, p.y - MARKER_SIZE / 2);
        show(s_marker, true);
        bool moving = g->course_deg >= 0 && g->speed_kmh > 2;
        if (moving) {
            float a = g->course_deg * (float)M_PI / 180;
            s_cpts[0] = p;
            s_cpts[1] = (lv_point_t){ clampc(p.x + COURSE_LEN * sinf(a)), clampc(p.y - COURSE_LEN * cosf(a)) };
            lv_obj_set_size(s_course, v.w, v.h);
            lv_line_set_points(s_course, s_cpts, 2);
        }
        show(s_course, moving);
    } else {
        show(s_marker, false);
        show(s_course, false);
    }
    if (g->matched && g->turn_next >= 0 && g->turn_in_m < TURN_SHOW_M) {
        lv_point_t t = to_px(&v, g->turn_x, g->turn_y);
        lv_obj_set_pos(s_ring, t.x - 11, t.y - 11);
        show(s_ring, true);
    } else {
        show(s_ring, false);
    }
    lv_obj_move_foreground(s_ring);
    lv_obj_move_foreground(s_course);
    lv_obj_move_foreground(s_marker);
    lv_obj_move_foreground(s_box);
    draw_scale(&v);
    lv_obj_invalidate(s_map);       /* the POI layer */
}

/* POIs: drawn over the children of the map (LV_EVENT_DRAW_POST_END) */
static void poi_draw_cb(lv_event_t *e)
{
    if (!s_view_ok || s_g.state != GPX_LOADED || s_g.npois == 0) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_content_coords(s_map, &box);

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = lv_color_hex(0xffd23f);
    dot.bg_opa = LV_OPA_COVER;
    dot.border_color = lv_color_black();
    dot.border_width = 2;
    dot.border_opa = LV_OPA_COVER;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_montserrat_12;
    ld.color = lv_color_hex(0xffd23f);

    uint32_t n = 0;
    gpx_svc_points_lock(&n, NULL);
    const gpx_poi_t *p = gpx_svc_pois(&n);
    for (uint32_t i = 0; p && i < n; i++) {
        lv_point_t q = to_px(&s_view, p[i].x, p[i].y);
        if (q.x < -6 || q.y < -6 || q.x > s_view.w + 6 || q.y > s_view.h + 6) continue;
        lv_area_t a = { box.x1 + q.x - 6, box.y1 + q.y - 6, box.x1 + q.x + 6, box.y1 + q.y + 6 };
        lv_draw_rect(ctx, &dot, &a);
        if (s_view.mpp <= 8 || (int)i == s_g.poi_next) {
            lv_area_t t = { a.x2 + 3, a.y1 - 1, a.x2 + 160, a.y1 + 14 };
            lv_draw_label(ctx, &ld, &t, p[i].name, NULL);
        }
    }
    gpx_svc_points_unlock();
}

/* ---------------------------------------------------------------- turn box */




static void update_turn_box(const gpx_status_t *g)
{
    if (s_map == NULL) {
        return;
    }
    nav_cue_t c;
    nav_cue(g, TURN_SHOW_M, &c);
    show(s_box, c.kind != CUE_NONE);
    arrow_show(&s_arrow, &c);
    if (c.kind == CUE_NONE) {
        return;
    }
    char t[16];
    if (c.dist < 0) snprintf(t, sizeof(t), "back");
    else            fmt_dist_short(t, sizeof(t), c.dist);
    if (strcmp(t, s_box_text) != 0) {
        strcpy(s_box_text, t);
        lv_label_set_text(s_box_lbl, t);
    }
}

/* screen on before a turn / when going off route (any page) */
static void wake_logic(const gpx_status_t *g)
{
    bool nav = g->state == GPX_LOADED && g->fix && g->matched;
    float wake_m = fmaxf(WAKE_MIN_M, g->speed_kmh / 3.6f * WAKE_S);
    bool turn = nav && !g->off_route && g->turn_next >= 0 && g->turn_in_m < wake_m;
    bool off = nav && (g->off_route || g->wrong_way);
    if ((turn && !s_wake_turn) || (off && !s_wake_off)) {
        if (screen_is_off()) {
            screen_wake();
            nav_go("route");
        }
    }
    if ((turn || off) != (s_wake_turn || s_wake_off)) {
        screen_hold(turn || off);
    }
    s_wake_turn = turn;
    s_wake_off = off;
}

/* ---------------------------------------------------------------- texts */

static void fmt_km(char *buf, size_t len, float m)
{
    if (m < 1000) snprintf(buf, len, "%.0f m", m);
    else          snprintf(buf, len, "%.2f km", m / 1000);
}

static void update_list(const gpx_status_t *g)
{
    if (g->list_gen == s_list_gen) {
        return;
    }
    s_list_gen = g->list_gen;
    EXT_RAM_BSS_ATTR static char opts[GPX_MAX_FILES * GPX_NAME_LEN];
    int len = 0;
    int sel = 0;
    for (int i = 0; i < g->nfiles; i++) {
        len += snprintf(opts + len, sizeof(opts) - len, "%s%s", i ? "\n" : "", g->files[i]);
        if (strcmp(g->files[i], g->file) == 0) {
            sel = i;
        }
    }
    uv_str(&v_list, g->nfiles ? opts : (g->list_msg[0] ? g->list_msg : "(no files)"));
    uv_int(&v_sel, sel);
}

static void update_texts(const gpx_status_t *g)
{
    char a[24], b[24];
    switch (g->state) {
    case GPX_LOADING:
        uv_strf(&v_info, "Loading %s ...", g->file);
        break;
    case GPX_FAILED:
        uv_strf(&v_info, "%s:\n%s", g->file, g->msg);
        break;
    case GPX_LOADED:
        fmt_km(a, sizeof(a), g->length_m);
        if (g->has_ele) {
            uv_strf(&v_info, "%s%s\n%s, %u points\nElevation %.0f - %.0f m, climb %.0f m%s%s",
                    g->name[0] ? g->name : g->file, g->reversed ? " (reversed)" : "", a, (unsigned)g->npoints,
                    g->ele_min, g->ele_max, g->climb_m, g->msg[0] ? "\n" : "", g->msg);
        } else {
            uv_strf(&v_info, "%s%s\n%s, %u points%s%s", g->name[0] ? g->name : g->file,
                    g->reversed ? " (reversed)" : "", a, (unsigned)g->npoints, g->msg[0] ? "\n" : "", g->msg);
        }
        break;
    default:
        uv_str(&v_info, g->nfiles ? "Pick a file and press Load" : g->list_msg);
        break;
    }

    if (g->state != GPX_LOADED) {
        uv_str(&v_big, "-");
        uv_str(&v_text, "");
    } else if (!g->fix) {
        uv_str(&v_big, "No GPS fix");
        uv_str(&v_text, "GPS page: GPIO6 in GPS mode,\nantenna with open sky");
    } else if (!g->matched) {
        uv_str(&v_big, "-");
        uv_str(&v_text, "");
    } else {
        fmt_dist_short(a, sizeof(a), g->togo_m);
        fmt_dist_short(b, sizeof(b), g->off_m);
        if (!g->joined && g->off_route) {
            uv_strf(&v_big, "To track %s", b);
        } else if (g->wrong_way) {
            uv_str(&v_big, "Wrong way!");
        } else if (g->off_route) {
            uv_strf(&v_big, "Off route %s", b);
        } else {
            uv_strf(&v_big, "%s to go", a);
        }
        /* progress from the join point */
        float start = g->joined ? g->start_m : 0;
        float done = fmaxf(0, g->done_m - start), total = g->length_m - start;
        fmt_km(b, sizeof(b), done);
        int pct = total > 0 ? (int)(100 * done / total + 0.5f) : 0;
        char join[40] = "";
        if (!g->joined) snprintf(join, sizeof(join), "\nNot on the track yet");
        else if (start > 100) snprintf(join, sizeof(join), "\nJoined at km %.1f", start / 1000);
        char poi[GPX_POI_NAME_LEN + 32] = "";
        if (g->poi_next >= 0) {
            uint32_t n;
            gpx_svc_points_lock(&n, NULL);
            const gpx_poi_t *p = gpx_svc_pois(&n);
            if (p && (uint32_t)g->poi_next < n) {
                fmt_km(a, sizeof(a), fmaxf(0, g->poi_in_m));
                snprintf(poi, sizeof(poi), "%s in %s\n", p[g->poi_next].name, a);
            }
            gpx_svc_points_unlock();
        }
        char turn[64] = "No more turns\n";
        if (g->turn_next >= 0) {
            int n = snprintf(turn, sizeof(turn), "Next: %s in %.0f m\n", gpx_turn_text(g->turn_angle),
                             g->turn_in_m < 0 ? 0 : g->turn_in_m);
            if (g->turn2) {
                snprintf(turn + n, sizeof(turn) - n, "then %s after %.0f m\n", gpx_turn_text(g->turn2_angle),
                         g->turn2_after_m);
            }
        }
        char off[24] = "";
        if (g->off_m >= 10 && !g->off_route) snprintf(off, sizeof(off), "\nTo track %.0f m", g->off_m);
        uv_strf(&v_text, "%s%sDone %s (%d %%)%s%s", turn, poi, b, pct, join, off);
    }
    uv_str(&v_view, s_follow ? "Whole" : "Follow");
}

/* ---------------------------------------------------------------- view API */

void view_route_init(void)
{
    build_objects();
    uv_str(&v_view, "Follow");
}

void view_route_update(bool visible)
{
    gpx_svc_get_status(&s_g);
    update_list(&s_g);
    wake_logic(&s_g);
    if (!visible) {
        s_key_valid = false;        /* redraw when the page comes back */
        return;
    }
    update_texts(&s_g);
    update_turn_box(&s_g);

    draw_key_t k;
    memset(&k, 0, sizeof(k));       /* compared with memcmp: no stray padding */
    k.gen = s_g.gen + (s_g.state == GPX_LOADING ? 0x80000000u : 0);
    k.follow = s_follow;
    k.fix = s_g.fix;
    k.matched = s_g.matched;
    k.zoom = s_zoom;
    k.pos_x = (int32_t)(s_g.pos_x * 10);
    k.pos_y = (int32_t)(s_g.pos_y * 10);
    k.seg = s_g.seg;
    k.course = (int16_t)s_g.course_deg;
    k.turn = s_g.turn_next;
    if (!s_key_valid || memcmp(&k, &s_key, sizeof(k)) != 0) {
        s_key = k;
        s_key_valid = true;
        draw_map(&s_g);
    }
}

void view_route_load(void)
{
    int sel = uv_get_int(&v_sel, 0);
    if (sel < 0 || sel >= s_g.nfiles) {
        return;
    }
    const char *file = s_g.files[sel];
    if (gpx_svc_load(file, false) == ESP_OK) {
        app_settings_t cfg;
        settings_get(&cfg);
        strlcpy(cfg.gpx_file, file, sizeof(cfg.gpx_file));
        cfg.gpx_reverse = false;
        settings_put(&cfg);
    }
}

/* reload the current track the other way round */
void view_route_reverse(void)
{
    if (s_g.file[0] == '\0' || s_g.state == GPX_LOADING) {
        return;
    }
    bool rev = !s_g.reversed;
    if (gpx_svc_load(s_g.file, rev) == ESP_OK) {
        app_settings_t cfg;
        settings_get(&cfg);
        strlcpy(cfg.gpx_file, s_g.file, sizeof(cfg.gpx_file));
        cfg.gpx_reverse = rev;
        settings_put(&cfg);
    }
}

void view_route_rescan(void)
{
    gpx_svc_rescan();
}

static void enter_follow(void)
{
    if (!s_follow) {
        /* start from the zoom level closest to the whole-track view */
        s_follow = true;
        s_zoom = 0;
        while (s_zoom < NZOOMS - 1 && ZOOMS[s_zoom] < s_scale_mpp) {
            s_zoom++;
        }
    }
}

void view_route_zoom(int dir)
{
    bool was = s_follow;
    enter_follow();
    if (was || dir > 0) {
        s_zoom += dir > 0 ? -1 : 1;     /* + = fewer metres per pixel */
    }
    if (s_zoom < 0) s_zoom = 0;
    if (s_zoom >= NZOOMS) s_zoom = NZOOMS - 1;
}

void view_route_toggle_view(void)
{
    if (s_follow) {
        s_follow = false;
    } else {
        enter_follow();
    }
}
