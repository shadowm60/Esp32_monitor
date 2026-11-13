/*
 * Esp32_monitor - general board monitor for the Waveshare ESP32-S3-Touch-LCD-7.
 * Only wiring and init order live here.
 *
 *   settings  user choices (NVS); NVS itself, needed by Wi-Fi and BLE too
 *   board     LCD, touch, CH422G IO expander, SD card, LVGL port
 *   wifi_svc  Wi-Fi station, scan / connect, SNTP clock
 *   ble_svc   Bluetooth LE central (NimBLE): scan, connect, pair
 *   canmon    CAN bus monitor + raw candump logger
 *   io_svc    GPIO6 (Sensor AD header), die temperature, I2C scan
 *   gps_svc   NMEA GPS on GPIO6 (io_svc mode "GPS")
 *   gpx_svc   GPX tracks from the SD card, position along the track
 *   ride_svc  ride statistics, GPX recording to the SD card
 *   web_svc   SD card file manager over Wi-Fi (http://<IP>/)
 *   hmi       EEZ Studio UI + bridge (runs inside the LVGL task)
 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "waveshare_rgb_lcd_port.h"
#include "sd_card.h"
#include "i2c_io.h"
#include "settings.h"
#include "hmi.h"
#include "wifi_svc.h"
#include "ble_svc.h"
#include "canmon.h"
#include "io_svc.h"
#include "gps_svc.h"
#include "gpx_svc.h"
#include "ride_svc.h"
#include "web_svc.h"

static const char *TAG = "app_main";

/* index = can_bitrate_sel, same table as hmi/bridge/view_can.c */
static const uint32_t BITRATES[] = { 125000, 250000, 500000, 1000000 };

static void log_heap(const char *step)
{
    ESP_LOGI(TAG, "after %-8s internal free %3u KB (largest block %3u KB), PSRAM free %u KB", step,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void app_main(void)
{
    settings_init();
    app_settings_t cfg;
    settings_get(&cfg);

    i2c_io_init();              /* CH422G: all outputs low = USB on GPIO19/20 */
    if (waveshare_sd_card_init() != ESP_OK) {
        ESP_LOGW(TAG, "no SD card; insert one and use Display > Remount SD");
    }

    log_heap("board");

    /* Display first: the RGB bounce buffers and the LVGL render buffer need
     * large blocks of internal RAM, which Wi-Fi and the BT controller would
     * otherwise fragment / use up. The UI copes with services that are not
     * up yet (their status reads as "off"/"starting"). */
    ESP_ERROR_CHECK(gps_svc_init());        /* before io_svc: its GPS mode starts it */
    ESP_ERROR_CHECK(io_svc_init((io_mode_t)cfg.io_mode));
    ESP_ERROR_CHECK(gpx_svc_init(cfg.gpx_file, cfg.gpx_reverse));     /* loads the last track in the background */
    ESP_ERROR_CHECK(ride_svc_init());
    ESP_ERROR_CHECK(canmon_init());
    lvgl_port_set_ui_hooks(hmi_init, hmi_tick);
    waveshare_esp32_s3_rgb_lcd_init();      /* starts the LVGL task -> hmi_init() */
    wavesahre_rgb_lcd_bl_on();
    log_heap("display");

    if (wifi_svc_init(cfg.wifi_autoconnect) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed");
    }
    if (web_svc_init() != ESP_OK) {
        ESP_LOGE(TAG, "web file manager failed to start");
    }
    log_heap("wifi");
    if (ble_svc_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed");
    }
    log_heap("ble");

    if (cfg.can_autostart) {
        uint8_t i = cfg.can_bitrate_idx < 4 ? cfg.can_bitrate_idx : 2;
        canmon_start(BITRATES[i], cfg.can_listen_only);
    }
    log_heap("running");
}
