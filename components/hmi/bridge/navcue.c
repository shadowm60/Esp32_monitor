/*
 * navcue - what to show as the navigation arrow, and the arrow itself.
 * Shared by the Route page (box over the map) and the Ride page (tiles).
 *
 *   wrong way   U-turn, "Wrong way"
 *   off route   straight arrow to the nearest point of the track: relative
 *               to the direction of travel when moving, else north up
 *   turn        the next turn's shape, its distance
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bridge.h"

static float wrap180(float a)
{
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

void fmt_dist_short(char *b, size_t n, float m)
{
    if (m < 0) m = 0;
    if (m < 100)        snprintf(b, n, "%.0f m", roundf(m / 5) * 5);
    else if (m < 1000)  snprintf(b, n, "%.0f m", roundf(m / 10) * 10);
    else if (m < 10000) snprintf(b, n, "%.1f km", m / 1000);
    else                snprintf(b, n, "%.0f km", m / 1000);
}

void nav_cue(const gpx_status_t *g, float show_m, nav_cue_t *c)
{
    memset(c, 0, sizeof(*c));
    if (g->state != GPX_LOADED || !g->fix || !g->matched) {
        return;
    }
    if (g->wrong_way) {
        c->kind = CUE_WRONG;
        c->angle = 180;
        c->dist = -1;
        snprintf(c->caption, sizeof(c->caption), "Wrong way");
    } else if (g->off_route) {
        bool moving = g->course_deg >= 0 && g->speed_kmh > 3;
        float a = moving ? wrap180(g->rejoin_bearing - g->course_deg) : g->rejoin_bearing;
        c->kind = CUE_REJOIN;
        c->angle = (int)lroundf(a / 5) * 5;
        c->dist = g->off_m;
        snprintf(c->caption, sizeof(c->caption), "%s", g->joined ? "Back to the track" : "To the track");
    } else if (g->turn_next >= 0 && g->turn_in_m < show_m) {
        c->kind = CUE_TURN;
        c->angle = g->turn_angle;
        c->dist = g->turn_in_m < 0 ? 0 : g->turn_in_m;
        snprintf(c->caption, sizeof(c->caption), "Next: %s", gpx_turn_text(g->turn_angle));
    }
}

/* ---------------------------------------------------------------- arrow */

static lv_obj_t *mk_line(lv_obj_t *parent, int width)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_set_size(l, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void arrow_init(arrow_t *a, lv_obj_t *parent, float cx, float cy, float scale, int width)
{
    memset(a, 0, sizeof(*a));
    lv_obj_update_layout(parent);
    a->line = mk_line(parent, width);
    a->head = mk_line(parent, width);
    a->cx = cx;
    a->cy = cy;
    a->s = scale;
    a->key = -1;
}

static lv_point_t pt(float x, float y)
{
    return (lv_point_t){ (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y) };
}

/* arrow head at (tx, ty) pointing at `dir` degrees (0 = up) */
static void head(arrow_t *a, float tx, float ty, float dir)
{
    float len = 15 * a->s;
    for (int s = -1, i = 0; s <= 1; s += 2, i += 2) {
        float w = (dir + s * 150) * (float)M_PI / 180;
        a->h[i] = pt(tx + len * sinf(w), ty - len * cosf(w));
    }
    a->h[1] = pt(tx, ty);
    lv_line_set_points(a->head, a->h, 3);
}

static void draw_turn(arrow_t *a, int angle)
{
    const float s = a->s, cx = a->cx, cy = a->cy;
    if (angle >= 160 || angle <= -160) {
        /* U-turn: up, across to the left, back down */
        a->p[0] = pt(cx + 14 * s, cy + 34 * s);
        a->p[1] = pt(cx + 14 * s, cy - 22 * s);
        a->p[2] = pt(cx - 14 * s, cy - 22 * s);
        a->p[3] = pt(cx - 14 * s, cy + 18 * s);
        lv_line_set_points(a->line, a->p, 4);
        head(a, cx - 14 * s, cy + 18 * s, 180);
    } else {
        float r = angle * (float)M_PI / 180;
        float tx = cx + 30 * s * sinf(r), ty = cy - 30 * s * cosf(r);
        a->p[0] = pt(cx, cy + 34 * s);
        a->p[1] = pt(cx, cy);
        a->p[2] = pt(tx, ty);
        lv_line_set_points(a->line, a->p, 3);
        head(a, tx, ty, (float)angle);
    }
}

static void draw_dir(arrow_t *a, int angle)
{
    const float s = a->s, half = 32 * s;
    float r = angle * (float)M_PI / 180;
    float tx = a->cx + half * sinf(r), ty = a->cy - half * cosf(r);
    a->p[0] = pt(a->cx - half * sinf(r), a->cy + half * cosf(r));
    a->p[1] = pt(tx, ty);
    lv_line_set_points(a->line, a->p, 2);
    head(a, tx, ty, (float)angle);
}

void arrow_show(arrow_t *a, const nav_cue_t *c)
{
    if (a->line == NULL) {
        return;
    }
    bool on = c->kind != CUE_NONE;
    if (on) {
        lv_obj_clear_flag(a->line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(a->head, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a->head, LV_OBJ_FLAG_HIDDEN);
        a->key = -1;
        return;
    }
    int key = c->kind * 1000 + c->angle + 500;
    if (key == a->key) {
        return;
    }
    a->key = key;
    if (c->kind == CUE_REJOIN) draw_dir(a, c->angle);
    else                       draw_turn(a, c->angle);
}
