/*
 * gps_svc - NMEA 0183 GPS receiver on one UART RX pin (listen only).
 *
 * Wiring (Olimex MOD-GPS, u-blox 7, UEXT connector): UEXT pin 1 VCC -> J8 3V3,
 * pin 2 GND -> J8 GND, pin 4 "RXD" (host-side name: it carries the module's
 * NMEA output) -> J8 AD = GPIO6. Pin 3 (the module's input) stays free.
 *
 * io_svc owns GPIO6 and enables this service in its GPS mode. The UART runs
 * in this service's task. Baud rate: 19200 (MOD-GPS default) first, else
 * auto-detected (no valid sentence for 3 s -> next rate). Parses RMC, GGA, GSA, GSV from any talker
 * (GP, GL, GN, GA, GB). With a valid fix it also sets the system clock when it
 * is more than 2 s off (no Wi-Fi needed).
 *
 * Thread safe; poll gps_svc_get_status().
 */
#ifndef GPS_SVC_H
#define GPS_SVC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPS_MAX_SATS    32
#define GPS_RAW_LINES   6
#define GPS_RAW_LEN     84      /* NMEA max 82 + CR LF */

typedef struct {
    char    sys;            /* 'P' GPS, 'L' GLONASS, 'A' Galileo, 'B' BeiDou, 'N' other */
    uint8_t prn;
    int8_t  elev;           /* degrees, -1 = unknown */
    int16_t azim;           /* degrees, -1 = unknown */
    int8_t  snr;            /* dB-Hz, -1 = not tracked */
    bool    used;           /* in the solution (GSA) */
} gps_sat_t;

typedef struct {
    bool     enabled;
    int      pin;
    uint32_t baud;
    bool     baud_locked;       /* valid sentences seen at this rate */
    /* counters */
    uint32_t bytes;
    uint32_t sentences;         /* checksum OK */
    uint32_t cksum_err;
    uint32_t rx_age_ms;         /* since the last valid sentence, UINT32_MAX = never */
    /* solution */
    bool     fix;               /* RMC status A */
    uint8_t  fix_type;          /* GSA: 1 none, 2 = 2D, 3 = 3D */
    uint8_t  quality;           /* GGA: 0 none, 1 GPS, 2 DGPS/SBAS, ... */
    uint8_t  sats_used;
    uint8_t  sats_view;
    double   lat, lon;          /* degrees, + = N / E */
    float    alt_m;             /* above mean sea level */
    float    speed_kmh;
    float    course_deg;        /* -1 = unknown (not moving) */
    float    hdop, pdop;
    uint32_t fix_age_ms;        /* since the last RMC with status A, UINT32_MAX = never */
    /* UTC from RMC */
    bool     time_valid;
    uint16_t year;
    uint8_t  month, day, hour, min, sec;
    bool     clock_set;         /* the system clock was set from GPS */
    /* satellites (GSV), sorted by system then PRN */
    uint8_t   nsats;
    gps_sat_t sats[GPS_MAX_SATS];
    /* the last sentences, oldest first */
    uint8_t  nraw;
    char     raw[GPS_RAW_LINES][GPS_RAW_LEN];
} gps_status_t;

esp_err_t gps_svc_init(void);

/* Start (pin >= 0) or stop (pin < 0) the receiver. Blocks until the service
 * task has opened / closed the UART (at most ~0.5 s): after a stop the pin is
 * free again. Not from the LVGL task. */
esp_err_t gps_svc_enable(int pin);

void gps_svc_get_status(gps_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GPS_SVC_H */
