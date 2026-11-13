/*
 * io_svc - the user-accessible IO of the board.
 *
 * On the ESP32-S3-Touch-LCD-7 almost every GPIO drives the RGB panel. What is
 * left for the outside world (schematic V1.2):
 *
 *   Sensor AD header (J8: 3V3, GND, AD)  GPIO6 = ADC1 channel 5, no divider:
 *                                        0..3.3 V only. Used here as analog
 *                                        input, digital input or output, or
 *                                        as the RX line of a GPS (gps_svc).
 *   I2C header (H1)                      GPIO8/9, shared with touch + CH422G
 *   CAN (J6) / RS485 (J7)                GPIO19/20, GPIO15/16
 *   UART header (H3)                     GPIO43/44 = the console (switch S1)
 *
 * The CH422G has no free pins (IO0, IO7 and OC0-3 are not connected).
 *
 * A task samples GPIO6 at 20 Hz in the chosen mode and reads the chip's
 * temperature sensor. Thread safe; poll io_svc_get_status().
 */
#ifndef IO_SVC_H
#define IO_SVC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IO_SVC_PIN      6

/* Order = the mode list on the IO page (io_mode_sel). */
typedef enum {
    IO_MODE_ADC = 0,
    IO_MODE_DIN,
    IO_MODE_DIN_PULLUP,
    IO_MODE_DIN_PULLDOWN,
    IO_MODE_DOUT,
    IO_MODE_GPS,                /* UART1 RX for an NMEA GPS (gps_svc) */
    IO_MODE_COUNT
} io_mode_t;

typedef struct {
    io_mode_t mode;
    /* analog */
    int      mv;                /* calibrated, averaged over 16 samples */
    int      raw;
    int      mv_min, mv_max;    /* since mode change / reset */
    bool     calibrated;        /* eFuse calibration available */
    /* digital */
    bool     level;             /* input level, or the output level */
    uint32_t edges;             /* input level changes seen (20 Hz sampling) */
    /* chip */
    float    chip_temp_c;
    bool     temp_ok;
    /* I2C scan */
    bool     i2c_scanning;
    uint32_t i2c_scan_count;
    char     i2c_result[160];
} io_svc_status_t;

esp_err_t io_svc_init(io_mode_t mode);

esp_err_t io_svc_set_mode(io_mode_t mode);
esp_err_t io_svc_set_output(bool level);      /* IO_MODE_DOUT only */
void      io_svc_reset_minmax(void);

/* Probes 0x08..0x77 on the shared bus, skipping the on-board CH422G and GT911
 * addresses; result text in the status. */
esp_err_t io_svc_i2c_scan(void);

void io_svc_get_status(io_svc_status_t *out);

const char *io_svc_mode_name(io_mode_t m);

#ifdef __cplusplus
}
#endif

#endif /* IO_SVC_H */
