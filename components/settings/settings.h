/*
 * settings - persistent user choices (NVS).
 *
 * One versioned blob. settings_update() changes RAM at once and saves
 * SAVE_DELAY_MS later, so dragging a slider costs a single flash write.
 * Thread safe. Wi-Fi credentials and BLE bonds are not here: the Wi-Fi
 * driver and NimBLE keep those in NVS themselves.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_PAGE_LEN   24
#define SETTINGS_GPX_LEN    64

typedef struct {
    uint8_t  brightness;                /* 10..100 % */
    uint8_t  screen_timeout_idx;        /* index into the Display page list, 0 = never */
    uint8_t  can_bitrate_idx;           /* index into the CAN page bitrate list */
    bool     can_listen_only;
    bool     can_autostart;             /* start CAN at boot */
    uint8_t  io_mode;                   /* io_svc mode of GPIO6 */
    bool     wifi_autoconnect;          /* connect to the stored network at boot */
    char     page[SETTINGS_PAGE_LEN];   /* EEZ screen name of the last page */
    char     gpx_file[SETTINGS_GPX_LEN];/* file in /sdcard/gpx loaded at boot, "" = none */
    bool     gpx_reverse;               /* ... loaded end to start */
    /* New fields go at the end: a shorter stored blob of the same version is
     * loaded and the new fields keep their defaults. */
} app_settings_t;

/* Initialises NVS (erased if corrupt / from another IDF version) and loads
 * the stored settings, defaults on any error. Must run before Wi-Fi / BLE. */
esp_err_t settings_init(void);

void settings_get(app_settings_t *out);

/* Copy, change, store:  app_settings_t s; settings_get(&s); s.x = 1; settings_put(&s); */
void settings_put(const app_settings_t *in);

/* Write pending changes now (e.g. before a restart). */
esp_err_t settings_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
