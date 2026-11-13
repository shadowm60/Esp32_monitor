/*
 * gpx_svc - GPX tracks from the SD card, matched against the GPS position.
 *
 * Files: /sdcard/gpx/<name>.gpx (CONFIG_MON_GPX_DIR). The service lists them,
 * parses one in its own task (streaming, any size up to GPX_MAX_POINTS) and,
 * once a second, matches the gps_svc position against the track: nearest
 * segment, distance done / to go, off-route distance.
 *
 * Track points (<trkpt>) are used; a file with only a route (<rtept>) uses
 * those. Points are kept in PSRAM in a local metric frame (equirectangular
 * projection around the first point, x = east, y = north, metres), which is
 * all a map display needs.
 *
 * Elevation (when the file has <ele>): resampled every GPX_PROFILE_STEP_M or
 * more (at most GPX_PROFILE_MAX samples), smoothed over +-50 m. From that:
 * total ascent / descent, the climbs (>= 300 m, >= 15 m up, >= 3 % average,
 * flat run-in / run-out trimmed, a drop of < 10 m does not end a climb), and
 * live: elevation and gradient at the matched position, current / next climb,
 * ascent still to do.
 *
 * Turns are detected from the track's shape (there are no instructions in
 * a GPX track): heading change over +-20 m (and +-35 m for the angle) of a
 * resampled, lightly smoothed copy, >= 35 degrees; same-direction turns
 * closer than 30 m are merged. Junctions where the road goes straight on
 * are not seen; sharp bends are reported as turns.
 *
 * Joining: the first match after a load (anywhere on the track) is the join
 * point; progress counts from there. Off route, rejoin_bearing points to
 * the nearest point of the track. Wrong way: moving against the track's
 * direction for WRONG_WAY_S seconds. A track can be loaded reversed.
 *
 * Waypoints (<wpt> with <name>) become POIs, placed along the track when
 * they are within GPX_POI_NEAR_M of it.
 *
 * Thread safe. The point and profile arrays are shared read-only between
 * gpx_svc_points_lock() and gpx_svc_points_unlock(); a new load waits for
 * the unlock.
 */
#ifndef GPX_SVC_H
#define GPX_SVC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPX_MAX_FILES       32
#define GPX_NAME_LEN        64
#define GPX_MAX_POINTS      100000      /* 1.6 MB of PSRAM */
#define GPX_OFF_ROUTE_M     30.0f
#define GPX_PROFILE_STEP_M  10.0f
#define GPX_PROFILE_MAX     6000
#define GPX_MAX_CLIMBS      48
#define GPX_MAX_POIS        64
#define GPX_POI_NAME_LEN    24
#define GPX_POI_NEAR_M      300.0f

typedef struct {
    float x, y;                     /* track frame */
    float along;                    /* distance along the track, -1 = not near it */
    float off;                      /* distance from the track */
    char  name[GPX_POI_NAME_LEN];
} gpx_poi_t;

typedef struct {
    float start_m;      /* distance along the track */
    float len_m;
    float gain_m;
    float avg_pct;
    float max_pct;      /* steepest 100 m */
    float top_ele;
} gpx_climb_t;

/* Strava-like category from length x average gradient:
 * 5 = HC, 1..4 = category, 0 = smaller */
static inline int gpx_climb_category(const gpx_climb_t *c)
{
    float score = c->len_m * c->avg_pct;
    return score >= 80000 ? 5 : score >= 64000 ? 1 : score >= 32000 ? 2 : score >= 16000 ? 3 :
           score >= 8000 ? 4 : 0;
}

typedef struct {
    float   dist;       /* along the track */
    float   x, y;       /* track frame */
    int16_t angle;      /* degrees, + = right, - = left */
} gpx_turn_t;

/* "slight left", "left", "sharp left", "U-turn" ... */
static inline const char *gpx_turn_text(int angle)
{
    int a = angle < 0 ? -angle : angle;
    if (a >= 160) return "U-turn";
    if (angle < 0) return a < 60 ? "slight left" : a < 120 ? "left" : "sharp left";
    return a < 60 ? "slight right" : a < 120 ? "right" : "sharp right";
}

typedef struct {
    float x, y;         /* metres east / north of the origin */
    float ele;          /* metres, NAN when the file has none */
    float dist;         /* metres along the track from the first point */
} gpx_point_t;

typedef enum {
    GPX_NONE = 0,
    GPX_LOADING,
    GPX_LOADED,
    GPX_FAILED,
} gpx_state_t;

typedef struct {
    /* directory */
    bool     listing;
    uint32_t list_gen;                  /* +1 per finished listing */
    uint8_t  nfiles;
    char     files[GPX_MAX_FILES][GPX_NAME_LEN];
    char     list_msg[48];              /* "no SD card", "folder empty", ... */
    /* loaded track */
    gpx_state_t state;
    uint32_t gen;                       /* +1 per load / unload: points changed */
    char     file[GPX_NAME_LEN];
    char     name[48];                  /* <name> from the file, else "" */
    char     msg[64];                   /* error / warning text */
    uint32_t npoints;
    bool     truncated;                 /* more than GPX_MAX_POINTS */
    float    length_m;
    bool     has_ele;
    float    ele_min, ele_max, climb_m;
    float    min_x, max_x, min_y, max_y;
    double   lat0, lon0;                /* origin of the metric frame */
    /* elevation profile (has_ele only) */
    bool     has_profile;
    float    ascent_m, descent_m;       /* smoothed totals */
    uint8_t  nclimbs;
    gpx_climb_t climbs[GPX_MAX_CLIMBS];
    uint16_t nturns;
    uint8_t  npois;
    bool     reversed;                  /* loaded end to start */
    /* navigation (needs a GPS fix) */
    bool     fix;
    float    pos_x, pos_y;              /* GPS position in the track frame */
    float    course_deg;                /* -1 = unknown */
    float    speed_kmh;
    bool     matched;                   /* a nearest segment was found */
    uint32_t seg;                       /* nearest segment = points seg..seg+1 */
    float    near_x, near_y;            /* nearest point on the track */
    float    off_m;                     /* distance to the track */
    bool     off_route;                 /* off_m > GPX_OFF_ROUTE_M */
    float    done_m, togo_m;
    bool     joined;                    /* a join point is set */
    float    start_m;                   /* join point: where this ride met the track */
    uint32_t start_seg;
    float    rejoin_bearing;            /* off route: direction to the track, 0 = north */
    bool     wrong_way;
    int8_t   poi_next;                  /* index, -1 = none ahead */
    float    poi_in_m;
    /* elevation at the matched position (has_profile && matched) */
    float    ele_here;
    float    grade_pct;                 /* over +-50 m */
    float    ascent_left_m;             /* climbing still to do to the end */
    int8_t   climb_cur;                 /* index into climbs, -1 = not on a climb */
    float    climb_left_m;              /* to the top of the current climb */
    float    climb_left_up_m;
    int8_t   climb_next;                /* next climb ahead, -1 = none */
    float    climb_next_in_m;
    /* turns ahead (matched only) */
    int16_t  turn_next;                 /* index, -1 = none left */
    float    turn_in_m;                 /* negative just after passing it */
    int16_t  turn_angle;
    float    turn_x, turn_y;
    bool     turn2;                     /* the turn after it is close (< 200 m) */
    float    turn2_after_m;             /* from the next turn */
    int16_t  turn2_angle;
} gpx_status_t;

/* autoload: file name in the GPX folder to load at start, NULL / "" = none */
esp_err_t gpx_svc_init(const char *autoload, bool reverse);

/* Non-blocking commands, done by the service task. */
esp_err_t gpx_svc_rescan(void);
esp_err_t gpx_svc_load(const char *file, bool reverse);
esp_err_t gpx_svc_rejoin(void);             /* forget the join point: next match sets it */
esp_err_t gpx_svc_unload(void);

void gpx_svc_get_status(gpx_status_t *out);

/* Read access to the points. Hold the lock briefly (one redraw). */
const gpx_point_t *gpx_svc_points_lock(uint32_t *n, uint32_t *gen);
void               gpx_svc_points_unlock(void);

/* Smoothed elevation every *step metres; only while holding the points lock.
 * NULL when the track has no elevation. */
const float *gpx_svc_profile(uint32_t *n, float *step);

/* POIs (sorted along the track, the ones not near it last); points lock held */
const gpx_poi_t *gpx_svc_pois(uint32_t *n);

/* lat/lon (degrees) -> track frame, false when nothing is loaded */
bool gpx_svc_to_xy(double lat, double lon, float *x, float *y);

#ifdef __cplusplus
}
#endif

#endif /* GPX_SVC_H */
