/*
 * IO page: GPIO6 on the Sensor AD header, chip temperature, I2C scan.
 *   io_mode_sel (two-way) -> io_svc mode + settings (also set by the GPS page)
 *   io_is_adc / io_is_out  for hidden flags
 *   io_value_text, io_detail_text, io_adc_mv, io_out_text, io_chip_text, i2c_text
 */
#include <stdio.h>
#include <inttypes.h>
#include "bridge.h"
#include "io_svc.h"
#include "settings.h"

static uivar_t v_mode = UIVAR("io_mode_sel");
static uivar_t v_is_adc = UIVAR("io_is_adc");
static uivar_t v_is_out = UIVAR("io_is_out");
static uivar_t v_value = UIVAR("io_value_text");
static uivar_t v_detail = UIVAR("io_detail_text");
static uivar_t v_mv = UIVAR("io_adc_mv");
static uivar_t v_out = UIVAR("io_out_text");
static uivar_t v_chip = UIVAR("io_chip_text");
static uivar_t v_i2c = UIVAR("i2c_text");

static int s_mode;

void view_io_init(void)
{
    app_settings_t cfg;
    settings_get(&cfg);
    s_mode = cfg.io_mode < IO_MODE_COUNT ? cfg.io_mode : IO_MODE_ADC;
    uv_int(&v_mode, s_mode);
}

void view_io_update(bool visible)
{
    int m = uv_get_int(&v_mode, s_mode);
    if (m != s_mode && m >= 0 && m < IO_MODE_COUNT) {
        s_mode = m;
        io_svc_set_mode((io_mode_t)m);
        app_settings_t cfg;
        settings_get(&cfg);
        cfg.io_mode = m;
        settings_put(&cfg);
    }

    if (!visible) {
        return;
    }
    io_svc_status_t st;
    io_svc_get_status(&st);
    bool switching = st.mode != (io_mode_t)s_mode;     /* applied by the io task shortly */

    uv_bool(&v_is_adc, s_mode == IO_MODE_ADC);
    uv_bool(&v_is_out, s_mode == IO_MODE_DOUT);
    if (switching) {
        uv_str(&v_value, "...");
    } else if (st.mode == IO_MODE_ADC) {
        uv_strf(&v_value, "%d mV", st.mv);
        uv_int(&v_mv, st.mv);
        if (st.mv_max >= 0) {
            uv_strf(&v_detail, "raw %d   min %d mV   max %d mV\n%s", st.raw, st.mv_min, st.mv_max,
                    st.calibrated ? "eFuse calibrated" : "not calibrated (estimate)");
        } else {
            uv_str(&v_detail, "");
        }
    } else if (st.mode == IO_MODE_GPS) {
        uv_str(&v_value, "GPS");
        uv_str(&v_detail, "UART1 RX for the GPS receiver: see the GPS page");
    } else {
        uv_str(&v_value, st.level ? "HIGH" : "LOW");
        if (st.mode == IO_MODE_DOUT) {
            uv_str(&v_detail, "Output, 3.3 V logic, max ~20 mA");
        } else {
            uv_strf(&v_detail, "%" PRIu32 " level changes seen (sampled at 20 Hz)", st.edges);
        }
    }
    uv_str(&v_out, st.level ? "Set LOW" : "Set HIGH");
    if (st.temp_ok) {
        uv_strf(&v_chip, "ESP32-S3 die temperature: %.1f C", st.chip_temp_c);
    }
    uv_str(&v_i2c, st.i2c_scanning ? "scanning..." : st.i2c_result);
}

void view_io_toggle_out(void)
{
    io_svc_status_t st;
    io_svc_get_status(&st);
    io_svc_set_output(!st.level);
}

void view_io_reset(void)
{
    io_svc_reset_minmax();
}

void view_io_i2c_scan(void)
{
    io_svc_i2c_scan();
}
