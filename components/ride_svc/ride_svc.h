/*
 * ride_svc - ride statistics and GPX recording from gps_svc.
 *
 * Once a second while a ride runs:
 *   moving      GPS speed > RIDE_MOVE_KMH starts moving, < RIDE_STOP_KMH stops
 *               (auto-pause: stops count as elapsed, not as moving time)
 *   distance    between fixes while moving only (no drift while parked),
 *               jumps faster than 150 km/h are dropped
 *   max speed   the lower of two consecutive readings (one-sample spikes)
 *   climbed     GPS altitude, 4 m hysteresis (GPS height is noisy)
 *   recording   /sdcard/rides/YYYYMMDD_HHMMSS.gpx (CONFIG_MON_RIDE_DIR),
 *               a point every second while moving. Written in blocks every
 *               10 s; the file always ends with the closing tags (they are
 *               overwritten by the next block), so a power cut leaves a
 *               valid GPX with at most 10 s missing.
 *
 * Thread safe; commands are non-blocking.
 */
#ifndef RIDE_SVC_H
#define RIDE_SVC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIDE_MOVE_KMH   4.0f
#define RIDE_STOP_KMH   2.0f

typedef enum {
    RIDE_IDLE = 0,      /* never started since boot */
    RIDE_RUNNING,
    RIDE_STOPPED,       /* finished: the figures stay until the next start */
} ride_state_t;

typedef struct {
    ride_state_t state;
    bool     fix;
    bool     moving;
    float    speed_kmh;         /* now (0 without a fix) */
    float    max_kmh;
    float    avg_kmh;           /* distance / moving time */
    float    dist_m;
    uint32_t elapsed_s;
    uint32_t moving_s;
    float    climb_m;
    /* recording */
    bool     logging;           /* the GPX file is open */
    char     file[40];          /* name in the rides folder, "" = none */
    uint32_t points;
    char     msg[48];           /* "no SD card", write errors */
} ride_status_t;

esp_err_t ride_svc_init(void);

esp_err_t ride_svc_start(void);     /* new ride: figures reset, new file */
esp_err_t ride_svc_stop(void);

void ride_svc_get_status(ride_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RIDE_SVC_H */
