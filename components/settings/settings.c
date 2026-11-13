#include "settings.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";

#define NVS_NS          "monitor"
#define NVS_KEY         "cfg"
#define BLOB_VERSION    1
#define SAVE_DELAY_MS   2000

typedef struct {
    uint32_t       version;
    app_settings_t s;
} blob_t;

static app_settings_t     s_cur;
static SemaphoreHandle_t  s_lock;
static esp_timer_handle_t s_save_timer;
static bool               s_dirty;

static void set_defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = 100;
    s->can_bitrate_idx = 2;         /* 500 kbit/s, see view_can.c */
    s->wifi_autoconnect = true;
}

static esp_err_t save_now(void)
{
    blob_t b = { .version = BLOB_VERSION };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    b.s = s_cur;
    s_dirty = false;
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &b, sizeof(b));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void save_timer_cb(void *arg)
{
    save_now();
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, defaults");
        return;
    }
    blob_t b;
    size_t len = sizeof(b);
    memset(&b, 0, sizeof(b));
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &b, &len);
    nvs_close(h);

    /* A blob from an older build of the same version may be shorter. */
    const size_t min_len = offsetof(blob_t, s) + sizeof(uint8_t);
    if (err != ESP_OK || b.version != BLOB_VERSION || len < min_len || len > sizeof(b)) {
        ESP_LOGW(TAG, "stored settings ignored (%s, v%u, %u bytes)",
                 esp_err_to_name(err), (unsigned)b.version, (unsigned)len);
        return;
    }
    app_settings_t s;
    set_defaults(&s);
    memcpy(&s, &b.s, len - offsetof(blob_t, s));
    s.page[SETTINGS_PAGE_LEN - 1] = '\0';
    if (s.brightness < 10 || s.brightness > 100) {
        s.brightness = 100;
    }
    s_cur = s;
    ESP_LOGI(TAG, "loaded");
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    s_lock = xSemaphoreCreateMutex();
    set_defaults(&s_cur);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s, defaults only", esp_err_to_name(err));
        return err;
    }
    load();

    const esp_timer_create_args_t args = { .callback = save_timer_cb, .name = "settings_save" };
    return esp_timer_create(&args, &s_save_timer);
}

void settings_get(app_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

void settings_put(const app_settings_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = memcmp(&s_cur, in, sizeof(s_cur)) != 0;
    if (changed) {
        s_cur = *in;
        s_cur.page[SETTINGS_PAGE_LEN - 1] = '\0';
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);

    if (changed && s_save_timer != NULL) {
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, (uint64_t)SAVE_DELAY_MS * 1000);
    }
}

esp_err_t settings_flush(void)
{
    if (s_save_timer != NULL) {
        esp_timer_stop(s_save_timer);
    }
    return s_dirty ? save_now() : ESP_OK;
}
