#include "canmon.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "can_if.h"
#include "i2c_io.h"
#include "sd_card.h"
#include "sdkconfig.h"

static const char *TAG = "canmon";

#define MON_QUEUE_LEN   2048            /* ~250 ms of a full 1 Mbit/s bus */
#define CMD_QUEUE_LEN   8
#define LOG_BUF_SIZE    (64 * 1024)     /* PSRAM */
#define LOG_FLUSH_AT    (32 * 1024)     /* write in big chunks: SD over SPI likes that */
#define LOG_LINE_MAX    48
#define TABLE_SIZE      CONFIG_MON_CAN_ID_TABLE_SIZE
#define LOG_DIR         CONFIG_MON_CANLOG_DIR
#define SYNC_MS         CONFIG_MON_CANLOG_SYNC_MS
#define EPOCH_VALID     1700000000      /* clock is set (2023-11) */

typedef enum { CMD_START, CMD_STOP, CMD_LOG_START, CMD_LOG_STOP, CMD_RESET } cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t   bitrate;
    bool       listen_only;
} cmd_t;

typedef struct {
    uint32_t key;           /* id | ext << 31 */
    uint8_t  dlc;
    uint8_t  data[8];
    uint32_t count;
    uint32_t last_ms;
    uint32_t period_ms;     /* smoothed */
} id_entry_t;

static QueueHandle_t     s_mon_q, s_cmd_q;
static SemaphoreHandle_t s_lock;            /* s_st + the ID table */
static canmon_stats_t    s_st;
static id_entry_t       *s_tab;             /* sorted by key */
static int               s_ntab;

/* per-second counters, task only */
static uint32_t s_sec_frames, s_sec_bits;
static uint32_t s_start_ms;
static uint32_t s_drop_base;                /* can_if mon_dropped at reset */

/* logging, task only */
static FILE    *s_log;
static char    *s_buf;
static size_t   s_buf_len;
static int64_t  s_epoch_off_us;             /* added to uptime for the timestamps, 0 = uptime */
static uint32_t s_log_start_ms, s_last_sync_ms;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------- ID table */

static int tab_find(uint32_t key, bool *found)
{
    int lo = 0, hi = s_ntab;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s_tab[mid].key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = lo < s_ntab && s_tab[lo].key == key;
    return lo;
}

static void tab_update(const can_frame_t *f)
{
    uint32_t key = f->id | (f->ext ? 0x80000000u : 0);
    bool found;
    int i = tab_find(key, &found);
    id_entry_t *e;
    if (found) {
        e = &s_tab[i];
        uint32_t dt = f->ts_ms - e->last_ms;
        /* smoothed period, first value taken as is */
        e->period_ms = e->count == 1 ? dt : (e->period_ms * 7 + dt) / 8;
    } else {
        if (s_ntab >= TABLE_SIZE) {
            s_st.id_table_full = true;
            return;
        }
        memmove(&s_tab[i + 1], &s_tab[i], (s_ntab - i) * sizeof(*s_tab));
        s_ntab++;
        e = &s_tab[i];
        memset(e, 0, sizeof(*e));
        e->key = key;
    }
    e->dlc = f->dlc;
    memcpy(e->data, f->data, f->dlc);
    e->count++;
    e->last_ms = f->ts_ms;
}

/* Approximate bits on the wire incl. stuffing (~10 % of the stuffable part)
 * and the 3 bit interframe space. */
static uint32_t frame_bits(const can_frame_t *f)
{
    uint32_t stuffable = (f->ext ? 54 : 34) + 8u * f->dlc;
    return stuffable + stuffable / 10 + 13 + 3;
}

/* ---------------------------------------------------------------- logging */

static void log_set_error(const char *what, int err)
{
    LOCK();
    snprintf(s_st.log_error, sizeof(s_st.log_error), "%s (%s)", what, strerror(err));
    UNLOCK();
    ESP_LOGE(TAG, "log: %s: %s", what, strerror(err));
}

static bool log_flush(bool sync)
{
    if (s_log == NULL) {
        return false;
    }
    if (s_buf_len > 0) {
        size_t n = fwrite(s_buf, 1, s_buf_len, s_log);
        if (n != s_buf_len) {
            log_set_error("write failed", errno);
            return false;
        }
        LOCK();
        s_st.log_bytes += n;
        UNLOCK();
        s_buf_len = 0;
    }
    if (sync) {
        fflush(s_log);
        fsync(fileno(s_log));
        s_last_sync_ms = now_ms();
    }
    return true;
}

static void log_close(void)
{
    if (s_log == NULL) {
        return;
    }
    log_flush(true);
    fclose(s_log);
    s_log = NULL;
    LOCK();
    s_st.log_active = false;
    UNLOCK();
    ESP_LOGI(TAG, "log closed: %s, %" PRIu32 " frames", s_st.log_file, s_st.log_frames);
}

static void log_fail_and_close(void)
{
    if (s_log != NULL) {
        fclose(s_log);
        s_log = NULL;
    }
    s_buf_len = 0;
    LOCK();
    s_st.log_active = false;
    UNLOCK();
}

static int next_log_number(void)
{
    int max = 0;
    DIR *d = opendir(LOG_DIR);
    if (d == NULL) {
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        int n;
        if (sscanf(de->d_name, "can_%d.log", &n) == 1 || sscanf(de->d_name, "CAN_%d.LOG", &n) == 1) {
            if (n > max) {
                max = n;
            }
        }
    }
    closedir(d);
    return max + 1;
}

static void log_open(void)
{
    if (s_log != NULL) {
        return;
    }
    LOCK();
    s_st.log_error[0] = '\0';
    UNLOCK();

    if (!waveshare_sd_card_mounted() && waveshare_sd_card_init() != ESP_OK) {
        LOCK();
        snprintf(s_st.log_error, sizeof(s_st.log_error), "no SD card (FAT32?)");
        UNLOCK();
        return;
    }
    if (mkdir(LOG_DIR, 0777) != 0 && errno != EEXIST) {
        log_set_error("cannot create " LOG_DIR, errno);
        return;
    }

    char name[40];
    time_t t = time(NULL);
    if (t > EPOCH_VALID) {
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(name, sizeof(name), "%Y%m%d_%H%M%S.log", &tm);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        s_epoch_off_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - esp_timer_get_time();
    } else {
        snprintf(name, sizeof(name), "can_%04d.log", next_log_number());
        s_epoch_off_us = 0;
    }
    char path[96];
    snprintf(path, sizeof(path), LOG_DIR "/%s", name);
    s_log = fopen(path, "w");
    if (s_log == NULL) {
        log_set_error("cannot create file", errno);
        return;
    }
    setvbuf(s_log, NULL, _IOFBF, 4096);
    s_buf_len = 0;
    s_log_start_ms = s_last_sync_ms = now_ms();
    LOCK();
    snprintf(s_st.log_file, sizeof(s_st.log_file), "%s", name);
    s_st.log_active = true;
    s_st.log_frames = s_st.log_bytes = s_st.log_dropped = s_st.log_secs = 0;
    UNLOCK();
    ESP_LOGI(TAG, "logging to %s", path);
}

static void log_frame(const can_frame_t *f)
{
    if (s_log == NULL) {
        return;
    }
    if (s_buf_len + LOG_LINE_MAX > LOG_BUF_SIZE) {
        LOCK();
        s_st.log_dropped++;
        UNLOCK();
        return;
    }
    int64_t us = s_epoch_off_us + (int64_t)f->ts_ms * 1000;
    char *p = s_buf + s_buf_len;
    int n = sprintf(p, "(%lld.%06lld) can0 ", us / 1000000, us % 1000000);
    n += sprintf(p + n, f->ext ? "%08" PRIX32 "#" : "%03" PRIX32 "#", f->id);
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < f->dlc; i++) {
        p[n++] = hex[f->data[i] >> 4];
        p[n++] = hex[f->data[i] & 0xF];
    }
    p[n++] = '\n';
    s_buf_len += n;
    LOCK();
    s_st.log_frames++;
    UNLOCK();
}

/* ---------------------------------------------------------------- commands */

static void reset_counters(void)
{
    can_if_reset_stats();
    can_if_stats_t cs;
    can_if_get_stats(&cs);
    s_drop_base = cs.mon_dropped;
    LOCK();
    s_ntab = 0;
    s_st.frames = s_st.fps = s_st.fps_peak = 0;
    s_st.std_frames = s_st.ext_frames = 0;
    s_st.load_pct = 0;
    s_st.ids = 0;
    s_st.id_table_full = false;
    s_st.dropped = 0;
    UNLOCK();
    s_sec_frames = s_sec_bits = 0;
    s_start_ms = now_ms();
}

static void do_cmd(const cmd_t *c)
{
    switch (c->type) {
    case CMD_START: {
        board_select_can(true);
        can_if_config_t cfg = {
            .bitrate = c->bitrate, .filter_id = 0, .filter_mask = 0, .listen_only = c->listen_only,
        };
        esp_err_t err = can_if_start(&cfg);
        if (err != ESP_OK) {
            board_select_can(false);
        }
        can_if_set_monitor(err == ESP_OK ? s_mon_q : NULL);
        reset_counters();
        LOCK();
        s_st.running = err == ESP_OK;
        s_st.bitrate = c->bitrate;
        s_st.listen_only = c->listen_only;
        UNLOCK();
        break;
    }
    case CMD_STOP:
        log_close();
        can_if_set_monitor(NULL);
        can_if_stop();
        board_select_can(false);    /* native USB port usable again */
        xQueueReset(s_mon_q);
        LOCK();
        s_st.running = false;
        s_st.fps = 0;
        s_st.load_pct = 0;
        UNLOCK();
        break;
    case CMD_LOG_START:
        log_open();
        break;
    case CMD_LOG_STOP:
        log_close();
        break;
    case CMD_RESET:
        reset_counters();
        break;
    }
}

/* ---------------------------------------------------------------- task */

static const char *state_name(can_if_bus_state_t s)
{
    switch (s) {
    case CAN_IF_BUS_ACTIVE:  return "active";
    case CAN_IF_BUS_WARNING: return "warning";
    case CAN_IF_BUS_PASSIVE: return "error passive";
    case CAN_IF_BUS_OFF:     return "bus-off";
    default:                 return "stopped";
    }
}

static void each_second(void)
{
    can_if_stats_t cs;
    can_if_get_stats(&cs);
    uint32_t bitrate = s_st.bitrate ? s_st.bitrate : 1;
    uint32_t load = (uint32_t)((uint64_t)s_sec_bits * 100 / bitrate);

    LOCK();
    s_st.fps = s_sec_frames;
    if (s_sec_frames > s_st.fps_peak) {
        s_st.fps_peak = s_sec_frames;
    }
    s_st.load_pct = load > 100 ? 100 : load;
    s_st.state = state_name(cs.state);
    s_st.bus_errors = cs.bus_errors;
    s_st.tec = cs.tec;
    s_st.rec = cs.rec;
    s_st.dropped = cs.mon_dropped - s_drop_base;
    s_st.uptime_s = s_st.running ? (now_ms() - s_start_ms) / 1000 : s_st.uptime_s;
    if (s_log != NULL) {
        s_st.log_secs = (now_ms() - s_log_start_ms) / 1000;
    }
    UNLOCK();
    s_sec_frames = s_sec_bits = 0;
}

static void canmon_task(void *arg)
{
    can_frame_t f;
    cmd_t c;
    uint32_t next_sec = now_ms() + 1000;

    while (1) {
        while (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE) {
            do_cmd(&c);
        }

        /* Drain the monitor queue in batches; the lock is taken per batch. */
        if (xQueueReceive(s_mon_q, &f, pdMS_TO_TICKS(50)) == pdTRUE) {
            int n = 0;
            LOCK();
            do {
                if (!f.tx) {
                    tab_update(&f);
                }
                s_st.frames++;
                if (f.ext) {
                    s_st.ext_frames++;
                } else {
                    s_st.std_frames++;
                }
                s_sec_frames++;
                s_sec_bits += frame_bits(&f);
                UNLOCK();
                log_frame(&f);
                LOCK();
            } while (++n < 256 && xQueueReceive(s_mon_q, &f, 0) == pdTRUE);
            s_st.ids = s_ntab;
            UNLOCK();
        }

        uint32_t now = now_ms();
        if (s_log != NULL) {
            bool sync = now - s_last_sync_ms >= SYNC_MS;
            if ((s_buf_len >= LOG_FLUSH_AT || sync) && !log_flush(sync)) {
                log_fail_and_close();
            }
        }
        if ((int32_t)(now - next_sec) >= 0) {
            next_sec += 1000;
            if ((int32_t)(now - next_sec) > 1000) {
                next_sec = now + 1000;      /* we were stalled, don't catch up */
            }
            each_second();
        }
    }
}

/* ---------------------------------------------------------------- API */

static esp_err_t post(cmd_t c)
{
    if (s_cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t canmon_start(uint32_t bitrate, bool listen_only)
{
    return post((cmd_t){ .type = CMD_START, .bitrate = bitrate, .listen_only = listen_only });
}

esp_err_t canmon_stop(void)      { return post((cmd_t){ .type = CMD_STOP }); }
esp_err_t canmon_log_start(void) { return post((cmd_t){ .type = CMD_LOG_START }); }
esp_err_t canmon_log_stop(void)  { return post((cmd_t){ .type = CMD_LOG_STOP }); }
esp_err_t canmon_reset(void)     { return post((cmd_t){ .type = CMD_RESET }); }

void canmon_get_stats(canmon_stats_t *out)
{
    LOCK();
    *out = s_st;
    UNLOCK();
}

int canmon_format_ids(char *buf, size_t len, int max_rows)
{
    size_t o = 0;
    o += snprintf(buf + o, len - o, "ID        DLC DATA                     COUNT  PERIOD\n");
    LOCK();
    int n = s_ntab;
    for (int i = 0; i < n && i < max_rows && o + 64 < len; i++) {
        const id_entry_t *e = &s_tab[i];
        bool ext = e->key & 0x80000000u;
        uint32_t id = e->key & 0x1FFFFFFFu;
        o += snprintf(buf + o, len - o, ext ? "%08" PRIX32 "  %u   " : "%03" PRIX32 "       %u   ", id, e->dlc);
        for (int b = 0; b < 8; b++) {
            o += snprintf(buf + o, len - o, b < e->dlc ? "%02X " : "   ", e->data[b]);
        }
        if (e->count > 1) {
            o += snprintf(buf + o, len - o, "%6" PRIu32 " %5" PRIu32 "ms\n", e->count, e->period_ms);
        } else {
            o += snprintf(buf + o, len - o, "%6" PRIu32 "      -\n", e->count);
        }
    }
    UNLOCK();
    if (n > max_rows && o + 32 < len) {
        snprintf(buf + o, len - o, "... %d more IDs\n", n - max_rows);
    }
    return n;
}

esp_err_t canmon_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_t));
    s_mon_q = xQueueCreateWithCaps(MON_QUEUE_LEN, sizeof(can_frame_t), MALLOC_CAP_SPIRAM);
    s_tab = heap_caps_calloc(TABLE_SIZE, sizeof(id_entry_t), MALLOC_CAP_SPIRAM);
    s_buf = heap_caps_malloc(LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_cmd_q || !s_mon_q || !s_tab || !s_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_st.state = "stopped";
    if (xTaskCreatePinnedToCore(canmon_task, "canmon", 4096, NULL, 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
