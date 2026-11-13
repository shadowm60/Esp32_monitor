/*
 * Status line (every page), Home page, Display page.
 *   sys_status                          one line: clock, Wi-Fi, BLE, CAN, REC, GPS, SD
 *   home_info / home_live / home_services
 *   disp_brightness (two-way), disp_brightness_text, disp_timeout_sel (two-way)
 *   sd_text, sys_version
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "lvgl.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bridge.h"
#include "settings.h"
#include "wifi_svc.h"
#include "ble_svc.h"
#include "canmon.h"
#include "io_svc.h"
#include "gps_svc.h"
#include "sd_card.h"

#define EPOCH_VALID 1700000000

static gps_status_t *s_gps;     /* PSRAM: internal RAM is tight */

static const char *gps_short(void)
{
    if (s_gps == NULL && (s_gps = heap_caps_calloc(1, sizeof(*s_gps), MALLOC_CAP_SPIRAM)) == NULL) {
        return "";
    }
    gps_svc_get_status(s_gps);
    if (!s_gps->enabled) return "";
    if (!s_gps->baud_locked) return "GPS -    ";
    if (!s_gps->fix) return "GPS ...    ";
    return s_gps->fix_type == 3 ? "GPS 3D    " : "GPS 2D    ";
}

static uivar_t v_status = UIVAR("sys_status");
static uivar_t v_info = UIVAR("home_info");
static uivar_t v_live = UIVAR("home_live");
static uivar_t v_svcs = UIVAR("home_services");
static uivar_t v_bright = UIVAR("disp_brightness");
static uivar_t v_bright_text = UIVAR("disp_brightness_text");
static uivar_t v_timeout = UIVAR("disp_timeout_sel");
static uivar_t v_sd = UIVAR("sd_text");
static uivar_t v_version = UIVAR("sys_version");

static uint8_t s_bright, s_timeout;
static char    s_sd_msg[64];

/* ---------------------------------------------------------------- SD info
 * f_getfree() can scan the whole FAT on a big card: never in the LVGL task. */
static volatile bool     s_sd_busy;
static volatile uint64_t s_sd_total, s_sd_free;
static volatile bool     s_sd_info_ok;
static volatile bool     s_sd_remount;

static void sd_info_task(void *arg)
{
    if (s_sd_remount) {
        s_sd_remount = false;
        waveshare_sd_card_remount();
    }
    uint64_t total = 0, free_b = 0;
    bool ok = waveshare_sd_card_mounted() && esp_vfs_fat_info(MOUNT_POINT, &total, &free_b) == ESP_OK;
    s_sd_total = total;
    s_sd_free = free_b;
    s_sd_info_ok = ok;
    s_sd_busy = false;
    vTaskDelete(NULL);
}

static void sd_info_refresh(bool remount)
{
    if (s_sd_busy) {
        return;
    }
    s_sd_busy = true;
    s_sd_remount = remount;
    if (xTaskCreatePinnedToCore(sd_info_task, "sd_info", 4096, NULL, 2, NULL, 0) != pdPASS) {
        s_sd_busy = false;
    }
}

/* ---------------------------------------------------------------- status line */

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void view_status_update(void)
{
    char clock[12] = "";
    time_t t = time(NULL);
    if (t > EPOCH_VALID) {
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(clock, sizeof(clock), "%H:%M", &tm);
    }
    wifi_svc_status_t w;
    wifi_svc_get_status(&w);
    ble_svc_status_t b;
    ble_svc_get_status(&b);
    canmon_stats_t c;
    canmon_get_stats(&c);

    char wifi[40], can[40];
    switch (w.state) {
    case WIFI_SVC_CONNECTED:  snprintf(wifi, sizeof(wifi), LV_SYMBOL_WIFI " %s", w.ip); break;
    case WIFI_SVC_CONNECTING: snprintf(wifi, sizeof(wifi), LV_SYMBOL_WIFI " ..."); break;
    default:                  snprintf(wifi, sizeof(wifi), LV_SYMBOL_WIFI " off"); break;
    }
    if (c.running) {
        snprintf(can, sizeof(can), "CAN %" PRIu32 "k %" PRIu32 "/s%s", c.bitrate / 1000, c.fps,
                 c.log_active ? "  " LV_SYMBOL_SAVE " REC" : "");
    } else {
        snprintf(can, sizeof(can), "CAN off");
    }
    uv_strf(&v_status, "%s    %s    %s%s    %s%s    %s",
            wifi, can, LV_SYMBOL_BLUETOOTH, b.state == BLE_SVC_CONNECTED ? " on" : "",
            gps_short(), waveshare_sd_card_mounted() ? LV_SYMBOL_SD_CARD : "no SD", clock);
}

/* ---------------------------------------------------------------- home */

void view_home_init(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uv_strf(&v_info,
            "Board:  Waveshare ESP32-S3-Touch-LCD-7\n"
            "Chip:  ESP32-S3 rev %d.%d, %d cores, 240 MHz\n"
            "Flash:  %" PRIu32 " MB    PSRAM:  %u MB\n"
            "Firmware:  %s %s\n"
            "Built:  %s %s\n"
            "ESP-IDF:  %s    LVGL:  %d.%d.%d\n"
            "MAC:  %02X:%02X:%02X:%02X:%02X:%02X",
            ci.revision / 100, ci.revision % 100, ci.cores,
            flash / (1024 * 1024), (unsigned)(esp_psram_get_size() / (1024 * 1024)),
            app->project_name, app->version, app->date, app->time, app->idf_ver,
            LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uv_strf(&v_version, "%s %s, built %s %s\nESP-IDF %s, LVGL %d.%d.%d, EEZ Studio UI",
            app->project_name, app->version, app->date, app->time, app->idf_ver,
            LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
}

void view_home_update(bool visible)
{
    if (!visible) {
        return;
    }
    char up[24], clock[40] = "not set (Wi-Fi or GPS)";
    time_t t = time(NULL);
    if (t > EPOCH_VALID) {
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", &tm);
    }
    io_svc_status_t io;
    io_svc_get_status(&io);
    char temp[16] = "-";
    if (io.temp_ok) {
        snprintf(temp, sizeof(temp), "%.1f C", io.chip_temp_c);
    }
    uv_strf(&v_live,
            "Uptime:  %s\n"
            "Time:  %s\n"
            "Internal RAM free:  %u kB (min %u kB)\n"
            "PSRAM free:  %u kB\n"
            "Largest internal block:  %u kB\n"
            "Die temperature:  %s\n"
            "Reset reason:  %d",
            fmt_duration(uptime_s(), up, sizeof(up)), clock,
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
            temp, (int)esp_reset_reason());

    wifi_svc_status_t w;
    wifi_svc_get_status(&w);
    ble_svc_status_t b;
    ble_svc_get_status(&b);
    canmon_stats_t c;
    canmon_get_stats(&c);

    char wifi[80], ble[80], can[96], gpio[48];
    if (w.state == WIFI_SVC_CONNECTED) {
        snprintf(wifi, sizeof(wifi), "connected to %s, %s, %d dBm", w.ssid, w.ip, w.rssi);
    } else {
        snprintf(wifi, sizeof(wifi), "%s", w.state == WIFI_SVC_CONNECTING ? "connecting..." : "not connected");
    }
    if (b.state == BLE_SVC_CONNECTED) {
        snprintf(ble, sizeof(ble), "connected to %s %s", b.peer_name, b.peer_addr);
    } else {
        snprintf(ble, sizeof(ble), "%s, %d bonds", b.state == BLE_SVC_OFF ? "starting" : "idle", b.nbonds);
    }
    if (c.running) {
        snprintf(can, sizeof(can), "%" PRIu32 " kbit/s, %" PRIu32 " frames/s, %" PRIu32 " IDs, %s%s",
                 c.bitrate / 1000, c.fps, c.ids, c.state, c.log_active ? ", recording" : "");
    } else {
        snprintf(can, sizeof(can), "stopped");
    }
    if (io.mode == IO_MODE_ADC) {
        snprintf(gpio, sizeof(gpio), "%d mV (analog)", io.mv);
    } else if (io.mode == IO_MODE_GPS) {
        gps_short();
        if (s_gps == NULL || !s_gps->baud_locked) {
            snprintf(gpio, sizeof(gpio), "GPS, no data");
        } else if (!s_gps->fix) {
            snprintf(gpio, sizeof(gpio), "GPS, no fix (%u sats seen)", s_gps->sats_view);
        } else {
            snprintf(gpio, sizeof(gpio), "GPS, %uD fix, %u sats", s_gps->fix_type == 2 ? 2 : 3, s_gps->sats_used);
        }
    } else {
        snprintf(gpio, sizeof(gpio), "%s (%s)", io.level ? "HIGH" : "LOW", io_svc_mode_name(io.mode));
    }
    uv_strf(&v_svcs, "Wi-Fi:  %s\nBLE:  %s\nCAN:  %s\nSD card:  %s\nGPIO6:  %s",
            wifi, ble, can, waveshare_sd_card_mounted() ? "mounted" : "not mounted", gpio);
}

/* ---------------------------------------------------------------- display */

void view_display_init(void)
{
    app_settings_t cfg;
    settings_get(&cfg);
    s_bright = cfg.brightness;
    s_timeout = cfg.screen_timeout_idx;
    uv_int(&v_bright, s_bright);
    uv_int(&v_timeout, s_timeout);
    uv_strf(&v_bright_text, "Brightness %u %%", s_bright);
    sd_info_refresh(false);
}

void view_display_update(bool visible)
{
    int b = uv_get_int(&v_bright, s_bright);
    int t = uv_get_int(&v_timeout, s_timeout);
    if (b != s_bright || t != s_timeout) {
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        s_bright = b;
        s_timeout = t;
        brightness_set(s_bright);
        screen_set_timeout(s_timeout);
        uv_strf(&v_bright_text, "Brightness %u %%", s_bright);
        app_settings_t cfg;
        settings_get(&cfg);
        cfg.brightness = s_bright;
        cfg.screen_timeout_idx = s_timeout;
        settings_put(&cfg);
    }
    if (!visible) {
        return;
    }
    if (s_sd_busy) {
        uv_str(&v_sd, "SD card: checking...");
    } else if (s_sd_info_ok) {
        uv_strf(&v_sd, "SD card: mounted at " MOUNT_POINT "\n%.1f GB free of %.1f GB\n%s",
                s_sd_free / 1e9, s_sd_total / 1e9, s_sd_msg);
    } else {
        uv_strf(&v_sd, "SD card: not mounted\nInsert a FAT32 card and press Remount.\n%s", s_sd_msg);
    }
}

void view_display_sd_remount(void)
{
    canmon_stats_t c;
    canmon_get_stats(&c);
    if (c.log_active) {
        snprintf(s_sd_msg, sizeof(s_sd_msg), "Stop the CAN recording first.");
        return;
    }
    s_sd_msg[0] = '\0';
    sd_info_refresh(true);
}

void view_display_restart(void)
{
    canmon_log_stop();
    settings_flush();
    vTaskDelay(pdMS_TO_TICKS(300));     /* let canmon close the log file */
    esp_restart();
}
