#include "ride_svc.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "sd_card.h"
#include "gps_svc.h"

static const char *TAG = "ride_svc";

#define DIR_PATH        CONFIG_MON_RIDE_DIR
#define PERIOD_MS       1000
#define FLUSH_MS        10000
#define BUF_SIZE        16384
#define BUF_FLUSH_AT    12288
#define MAX_JUMP_MPS    (150 / 3.6f)
#define CLIMB_HYST_M    4.0f
#define EPOCH_VALID     1700000000
#define EARTH_R         6371000.0
#define DEG             (M_PI / 180.0)

static const char FOOTER[] = "</trkseg></trk></gpx>\n";

typedef enum { CMD_START, CMD_STOP } cmd_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_q;
EXT_RAM_BSS_ATTR static ride_status_t s_st;
EXT_RAM_BSS_ATTR static gps_status_t  s_gps;
EXT_RAM_BSS_ATTR static char          s_buf[BUF_SIZE];

/* task-only state */
static size_t   s_buf_len;
static FILE    *s_f;
static long     s_body_end;         /* file offset where the footer starts */
static int64_t  s_start_us, s_last_flush_us;
static bool     s_have_last;
static double   s_last_lat, s_last_lon;
static float    s_prev_speed;
static bool     s_have_ref;
static float    s_ref_alt;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ---------------------------------------------------------------- file */

static void set_msg(const char *m)
{
    LOCK();
    snprintf(s_st.msg, sizeof(s_st.msg), "%s", m);
    UNLOCK();
}

static bool clock_ok(void)
{
    return time(NULL) > EPOCH_VALID;
}

static void iso_now(char *b, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(b, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* writes the buffer at the end of the body, then the footer again */
static bool file_flush(void)
{
    if (s_f == NULL) {
        return false;
    }
    bool ok = fseek(s_f, s_body_end, SEEK_SET) == 0;
    if (ok && s_buf_len > 0) {
        ok = fwrite(s_buf, 1, s_buf_len, s_f) == s_buf_len;
    }
    if (ok) {
        s_body_end = ftell(s_f);
        ok = fwrite(FOOTER, 1, sizeof(FOOTER) - 1, s_f) == sizeof(FOOTER) - 1;
    }
    if (ok) {
        ok = fflush(s_f) == 0 && fsync(fileno(s_f)) == 0;
    }
    s_buf_len = 0;
    s_last_flush_us = esp_timer_get_time();
    if (!ok) {
        ESP_LOGE(TAG, "write failed, recording stopped");
        fclose(s_f);
        s_f = NULL;
        LOCK();
        s_st.logging = false;
        snprintf(s_st.msg, sizeof(s_st.msg), "SD write failed, recording stopped");
        UNLOCK();
    }
    return ok;
}

static void buf_add(const char *s, size_t n)
{
    if (s_buf_len + n > sizeof(s_buf)) {
        file_flush();
    }
    if (s_f != NULL && n <= sizeof(s_buf)) {
        memcpy(s_buf + s_buf_len, s, n);
        s_buf_len += n;
    }
}

static void file_open(void)
{
    if (!waveshare_sd_card_mounted()) {
        set_msg("no SD card: not recording");
        return;
    }
    mkdir(DIR_PATH, 0775);
    char name[40];
    if (clock_ok()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(name, sizeof(name), "%Y%m%d_%H%M%S.gpx", &tm);
    } else {
        int n = 0;
        struct stat st;
        char p[96];
        do {
            snprintf(name, sizeof(name), "ride_%04d.gpx", n++);
            snprintf(p, sizeof(p), "%s/%s", DIR_PATH, name);
        } while (stat(p, &st) == 0 && n < 10000);
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, name);
    s_f = fopen(path, "w");
    if (s_f == NULL) {
        set_msg("cannot create the GPX file");
        ESP_LOGE(TAG, "fopen %s failed", path);
        return;
    }
    s_body_end = 0;
    s_buf_len = 0;
    char iso[32] = "", head[512];
    if (clock_ok()) iso_now(iso, sizeof(iso));
    int n = snprintf(head, sizeof(head),
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<gpx version=\"1.1\" creator=\"Esp32_monitor\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
                     "<metadata>%s%s%s</metadata>\n"
                     "<trk><name>%s</name><type>cycling</type><trkseg>\n",
                     iso[0] ? "<time>" : "", iso, iso[0] ? "</time>" : "", name);
    buf_add(head, n);
    file_flush();
    LOCK();
    s_st.logging = s_f != NULL;
    snprintf(s_st.file, sizeof(s_st.file), "%s", name);
    UNLOCK();
    ESP_LOGI(TAG, "recording to %s", path);
}

static void file_close(void)
{
    if (s_f == NULL) {
        return;
    }
    file_flush();
    if (s_f != NULL) {
        fclose(s_f);
        s_f = NULL;
    }
    LOCK();
    s_st.logging = false;
    UNLOCK();
    ESP_LOGI(TAG, "recording closed, %u points", (unsigned)s_st.points);
}

static void add_point(const gps_status_t *g)
{
    char iso[32] = "", line[160];
    if (clock_ok()) iso_now(iso, sizeof(iso));
    int n = snprintf(line, sizeof(line), "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele>%s%s%s</trkpt>\n",
                     g->lat, g->lon, g->alt_m, iso[0] ? "<time>" : "", iso, iso[0] ? "</time>" : "");
    buf_add(line, n);
    LOCK();
    s_st.points++;
    UNLOCK();
}

/* ---------------------------------------------------------------- ride */

static void do_start(void)
{
    file_close();
    LOCK();
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = RIDE_RUNNING;
    UNLOCK();
    s_start_us = esp_timer_get_time();
    s_have_last = s_have_ref = false;
    s_prev_speed = 0;
    file_open();
}

static void do_stop(void)
{
    file_close();
    LOCK();
    if (s_st.state == RIDE_RUNNING) {
        s_st.state = RIDE_STOPPED;
    }
    s_st.moving = false;
    UNLOCK();
    ESP_LOGI(TAG, "ride stopped: %.2f km, %u s moving", s_st.dist_m / 1000, (unsigned)s_st.moving_s);
}

static float dist_m(double lat1, double lon1, double lat2, double lon2)
{
    double x = (lon2 - lon1) * DEG * cos((lat1 + lat2) / 2 * DEG);
    double y = (lat2 - lat1) * DEG;
    return (float)(sqrt(x * x + y * y) * EARTH_R);
}

static void tick(float dt)
{
    gps_svc_get_status(&s_gps);
    const gps_status_t *g = &s_gps;

    LOCK();
    s_st.fix = g->fix;
    s_st.speed_kmh = g->fix ? g->speed_kmh : 0;
    if (s_st.state != RIDE_RUNNING) {
        UNLOCK();
        return;
    }
    s_st.elapsed_s = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000);

    if (!g->fix) {
        s_st.moving = false;
        UNLOCK();
        return;
    }
    if (s_st.moving ? g->speed_kmh < RIDE_STOP_KMH : g->speed_kmh > RIDE_MOVE_KMH) {
        s_st.moving = !s_st.moving;
    }
    bool moving = s_st.moving;
    if (moving) {
        s_st.moving_s += (uint32_t)lroundf(dt);
        float spd = fminf(g->speed_kmh, s_prev_speed);      /* a single-sample spike doesn't count */
        if (spd > s_st.max_kmh) s_st.max_kmh = spd;
        if (s_have_last) {
            float d = dist_m(s_last_lat, s_last_lon, g->lat, g->lon);
            if (d <= MAX_JUMP_MPS * dt) {
                s_st.dist_m += d;
            }
        }
        if (g->hdop > 0 && g->hdop < 5) {
            if (!s_have_ref) {
                s_ref_alt = g->alt_m;
                s_have_ref = true;
            } else if (g->alt_m - s_ref_alt >= CLIMB_HYST_M) {
                s_st.climb_m += g->alt_m - s_ref_alt;
                s_ref_alt = g->alt_m;
            } else if (s_ref_alt - g->alt_m >= CLIMB_HYST_M) {
                s_ref_alt = g->alt_m;
            }
        }
        s_st.avg_kmh = s_st.moving_s > 0 ? s_st.dist_m / s_st.moving_s * 3.6f : 0;
    }
    s_prev_speed = g->speed_kmh;
    UNLOCK();

    /* the last position also follows while stopped: no jump when moving off */
    s_last_lat = g->lat;
    s_last_lon = g->lon;
    s_have_last = true;
    if (moving && s_f != NULL) {
        add_point(g);
    }
}

static void ride_task(void *arg)
{
    int64_t last = esp_timer_get_time();
    while (1) {
        cmd_t c;
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(PERIOD_MS)) == pdTRUE) {
            if (c == CMD_START) do_start();
            else                do_stop();
        }
        int64_t now = esp_timer_get_time();
        if (now - last >= PERIOD_MS * 1000LL) {
            tick((now - last) / 1e6f);
            last = now;
        }
        if (s_f != NULL && (s_buf_len >= BUF_FLUSH_AT || now - s_last_flush_us >= FLUSH_MS * 1000LL)) {
            file_flush();
        }
    }
}

/* ---------------------------------------------------------------- API */

esp_err_t ride_svc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreateWithCaps(4, sizeof(cmd_t), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* stack in PSRAM: SD card only, never writes flash */
    if (xTaskCreatePinnedToCoreWithCaps(ride_task, "ride_svc", 4096, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t send(cmd_t c)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_q, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ride_svc_start(void)
{
    return send(CMD_START);
}

esp_err_t ride_svc_stop(void)
{
    return send(CMD_STOP);
}

void ride_svc_get_status(ride_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    LOCK();
    *out = s_st;
    UNLOCK();
}
