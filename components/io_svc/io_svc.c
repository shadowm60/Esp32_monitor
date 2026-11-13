#include "io_svc.h"

#include <string.h>
#include <stdio.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "i2c_io.h"
#include "gps_svc.h"

static const char *TAG = "io_svc";

#define SAMPLE_PERIOD_MS    50
#define ADC_AVG             16
#define TEMP_PERIOD_MS      1000

static SemaphoreHandle_t          s_lock;
static io_svc_status_t            s_st;
static adc_oneshot_unit_handle_t  s_adc;
static adc_cali_handle_t          s_cali;
static adc_channel_t              s_chan;
static temperature_sensor_handle_t s_temp;
static volatile bool              s_scan_req;
static volatile int               s_mode_req = -1;   /* applied by the task: it owns the ADC */

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

const char *io_svc_mode_name(io_mode_t m)
{
    static const char *names[] = { "analog in", "digital in", "digital in, pull-up",
                                   "digital in, pull-down", "digital out",
                                   "GPS (NMEA in)" };
    return m < IO_MODE_COUNT ? names[m] : "?";
}

/* ---------------------------------------------------------------- pin modes */

static void adc_release(void)
{
    if (s_cali != NULL) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_adc != NULL) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
}

static esp_err_t adc_setup(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(IO_SVC_PIN, &unit, &s_chan), TAG, "not an ADC pin");
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&ucfg, &s_adc), TAG, "adc unit");
    const adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &ccfg), TAG, "adc channel");

    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration in eFuse, showing estimated mV");
    }
    return ESP_OK;
}

static esp_err_t apply_mode(io_mode_t mode)
{
    static bool gps_on;
    if (gps_on) {
        gps_svc_enable(-1);         /* waits until the UART let go of the pin */
        gps_on = false;
    }
    adc_release();
    gpio_reset_pin(IO_SVC_PIN);

    esp_err_t err = ESP_OK;
    if (mode == IO_MODE_ADC) {
        err = adc_setup();
    } else if (mode == IO_MODE_GPS) {
        err = gps_svc_enable(IO_SVC_PIN);
        gps_on = err == ESP_OK;
    } else {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << IO_SVC_PIN,
            .mode = mode == IO_MODE_DOUT ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
            .pull_up_en = mode == IO_MODE_DIN_PULLUP,
            .pull_down_en = mode == IO_MODE_DIN_PULLDOWN,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&c);
        if (err == ESP_OK && mode == IO_MODE_DOUT) {
            gpio_set_level(IO_SVC_PIN, 0);
        }
    }
    LOCK();
    s_st.mode = mode;
    s_st.calibrated = s_cali != NULL;
    s_st.mv = s_st.raw = 0;
    s_st.mv_min = 1 << 30;
    s_st.mv_max = -1;
    s_st.edges = 0;
    s_st.level = false;
    UNLOCK();
    ESP_LOGI(TAG, "GPIO%d: %s%s", IO_SVC_PIN, io_svc_mode_name(mode), err == ESP_OK ? "" : " (failed)");
    return err;
}

/* ---------------------------------------------------------------- I2C scan */

static bool on_board(uint8_t a)
{
    /* CH422G answers on 0x20-0x27 and 0x30-0x3F, GT911 on 0x5D (or 0x14). */
    return (a >= 0x20 && a <= 0x27) || (a >= 0x30 && a <= 0x3F) || a == 0x5D || a == 0x14;
}

static void i2c_scan(void)
{
    char buf[sizeof(s_st.i2c_result)];
    int len = 0, found = 0;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        snprintf(buf, sizeof(buf), "I2C bus not initialised");
    } else {
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (on_board(a)) {
                continue;
            }
            if (i2c_master_probe(bus, a, 20) == ESP_OK) {
                found++;
                if (len < (int)sizeof(buf) - 8) {
                    len += snprintf(buf + len, sizeof(buf) - len, "%s0x%02X", found > 1 ? " " : "", a);
                }
            }
        }
        if (found == 0) {
            snprintf(buf, sizeof(buf), "no external devices (on-board CH422G and GT911 skipped)");
        }
    }
    LOCK();
    snprintf(s_st.i2c_result, sizeof(s_st.i2c_result), "%s", buf);
    s_st.i2c_scanning = false;
    s_st.i2c_scan_count++;
    UNLOCK();
    ESP_LOGI(TAG, "I2C scan: %s", buf);
}

/* ---------------------------------------------------------------- task */

static void sample(void)
{
    LOCK();
    io_mode_t mode = s_st.mode;
    UNLOCK();

    if (mode == IO_MODE_ADC && s_adc != NULL) {
        int sum = 0, n = 0, raw;
        for (int i = 0; i < ADC_AVG; i++) {
            if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) {
                sum += raw;
                n++;
            }
        }
        if (n == 0) {
            return;
        }
        raw = sum / n;
        int mv;
        if (s_cali == NULL || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            mv = raw * 3100 / 4095;     /* rough, 12 dB attenuation */
        }
        LOCK();
        s_st.raw = raw;
        s_st.mv = mv;
        if (mv < s_st.mv_min) s_st.mv_min = mv;
        if (mv > s_st.mv_max) s_st.mv_max = mv;
        UNLOCK();
    } else if (mode != IO_MODE_ADC && mode != IO_MODE_GPS) {
        bool lvl = gpio_get_level(IO_SVC_PIN);
        LOCK();
        if (lvl != s_st.level) {
            s_st.edges++;
        }
        s_st.level = lvl;
        UNLOCK();
    }
}

static void io_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t temp_acc = 0;
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        int req = s_mode_req;
        if (req >= 0) {
            s_mode_req = -1;
            apply_mode((io_mode_t)req);
        }
        sample();

        temp_acc += SAMPLE_PERIOD_MS;
        if (temp_acc >= TEMP_PERIOD_MS && s_temp != NULL) {
            temp_acc = 0;
            float t;
            bool ok = temperature_sensor_get_celsius(s_temp, &t) == ESP_OK;
            LOCK();
            s_st.temp_ok = ok;
            if (ok) {
                s_st.chip_temp_c = t;
            }
            UNLOCK();
        }
        if (s_scan_req) {
            s_scan_req = false;
            i2c_scan();
        }
    }
}

/* ---------------------------------------------------------------- API */

esp_err_t io_svc_init(io_mode_t mode)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_st.i2c_result, sizeof(s_st.i2c_result), "not scanned yet");

    const temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_temp) != ESP_OK || temperature_sensor_enable(s_temp) != ESP_OK) {
        ESP_LOGW(TAG, "temperature sensor not available");
        s_temp = NULL;
    }

    if (mode >= IO_MODE_COUNT) {
        mode = IO_MODE_ADC;
    }
    apply_mode(mode);
    /* stack in PSRAM: the task never writes flash (would need the cache) */
    if (xTaskCreatePinnedToCoreWithCaps(io_task, "io_svc", 3072, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t io_svc_set_mode(io_mode_t mode)
{
    if (mode >= IO_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mode_req = mode;      /* the task switches between two samples */
    return ESP_OK;
}

esp_err_t io_svc_set_output(bool level)
{
    LOCK();
    bool out = s_st.mode == IO_MODE_DOUT;
    if (out) {
        s_st.level = level;
    }
    UNLOCK();
    if (!out) {
        return ESP_ERR_INVALID_STATE;
    }
    return gpio_set_level(IO_SVC_PIN, level);
}

void io_svc_reset_minmax(void)
{
    LOCK();
    s_st.mv_min = 1 << 30;
    s_st.mv_max = -1;
    s_st.edges = 0;
    UNLOCK();
}

esp_err_t io_svc_i2c_scan(void)
{
    LOCK();
    bool busy = s_st.i2c_scanning;
    s_st.i2c_scanning = true;
    UNLOCK();
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_req = true;
    return ESP_OK;
}

void io_svc_get_status(io_svc_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    LOCK();
    *out = s_st;
    UNLOCK();
}
