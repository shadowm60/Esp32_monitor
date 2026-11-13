#include "gps_svc.h"

#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_attr.h"
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

static const char *TAG = "gps_svc";

#define GPS_UART            UART_NUM_1
#define RX_BUF              1024
#define BAUD_TRY_MS         3000    /* no valid sentence for this long -> next rate */
#define LOST_MS             5000    /* locked rate silent this long -> search again */
#define SAT_TIMEOUT_MS      5000    /* GSV entry not refreshed -> dropped */
#define USED_TIMEOUT_MS     2500    /* GSA "used" mark lifetime */
#define CLOCK_TOLERANCE_S   2
#define MAX_FIELDS          24

#define REQ_NONE            (-2)
#define REQ_STOP            (-1)

static const uint32_t BAUDS[] = { 19200, 9600, 38400, 115200, 4800, 57600 };   /* MOD-GPS: 19200 */
#define NBAUDS (sizeof(BAUDS) / sizeof(BAUDS[0]))

typedef struct {
    gps_sat_t s;
    uint32_t  seen_ms;
} sat_entry_t;

static SemaphoreHandle_t s_lock, s_done;
static volatile int      s_req = REQ_NONE;
/* big state in PSRAM: internal RAM is the tight resource (CLAUDE.md) */
EXT_RAM_BSS_ATTR static gps_status_t s_st;     /* everything except sats / raw / ages */
EXT_RAM_BSS_ATTR static sat_entry_t  s_sats[GPS_MAX_SATS];
EXT_RAM_BSS_ATTR static uint32_t     s_used_ms[256];
EXT_RAM_BSS_ATTR static char         s_raw[GPS_RAW_LINES][GPS_RAW_LEN];
static unsigned          s_raw_next, s_raw_count;
static uint32_t          s_last_ok_ms, s_fix_ms, s_baud_since_ms;
static bool              s_ever_ok, s_ever_fix;
static unsigned          s_baud_idx;
static bool              s_open;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------- UART */

static void uart_open(int pin)
{
    const uart_config_t cfg = {
        .baud_rate = (int)BAUDS[s_baud_idx],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(GPS_UART, RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(GPS_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART on GPIO%d: %s", pin, esp_err_to_name(err));
        uart_driver_delete(GPS_UART);
        return;
    }
    gpio_pullup_en(pin);        /* idle high when nothing is connected */
    s_open = true;
    s_baud_since_ms = now_ms();
    LOCK();
    s_st.enabled = true;
    s_st.pin = pin;
    s_st.baud = BAUDS[s_baud_idx];
    s_st.baud_locked = false;
    UNLOCK();
    ESP_LOGI(TAG, "listening on GPIO%d, %" PRIu32 " baud", pin, BAUDS[s_baud_idx]);
}

static void uart_close(void)
{
    if (!s_open) {
        return;
    }
    LOCK();
    int pin = s_st.pin;
    s_st.enabled = false;
    s_st.baud_locked = false;
    s_st.fix = false;
    UNLOCK();
    uart_driver_delete(GPS_UART);
    gpio_reset_pin(pin);
    s_open = false;
    ESP_LOGI(TAG, "stopped, GPIO%d released", pin);
}

static void next_baud(void)
{
    s_baud_idx = (s_baud_idx + 1) % NBAUDS;
    uart_set_baudrate(GPS_UART, BAUDS[s_baud_idx]);
    uart_flush_input(GPS_UART);
    s_baud_since_ms = now_ms();
    LOCK();
    s_st.baud = BAUDS[s_baud_idx];
    s_st.baud_locked = false;
    UNLOCK();
    ESP_LOGI(TAG, "no NMEA, trying %" PRIu32 " baud", BAUDS[s_baud_idx]);
}

/* ---------------------------------------------------------------- NMEA helpers */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "$....*hh" -> true if the checksum matches; cuts the line at '*'. */
static bool checksum_ok(char *s)
{
    if (s[0] != '$') {
        return false;
    }
    uint8_t x = 0;
    char *p = s + 1;
    for (; *p && *p != '*'; p++) {
        x ^= (uint8_t)*p;
    }
    if (*p != '*') {
        return false;
    }
    int h = hexval(p[1]), l = hexval(p[2]);
    if (h < 0 || l < 0) {
        return false;
    }
    *p = '\0';
    return x == (uint8_t)(h << 4 | l);
}

static int split(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++) {
        if (*s == ',') {
            *s = '\0';
            f[n++] = s + 1;
        }
    }
    return n;
}

/* "ddmm.mmmm" / "dddmm.mmmm" + hemisphere -> degrees */
static bool parse_coord(const char *v, const char *hemi, double *out)
{
    if (!*v || !*hemi) {
        return false;
    }
    double x = atof(v);
    int deg = (int)(x / 100);
    double d = deg + (x - deg * 100) / 60.0;
    if (*hemi == 'S' || *hemi == 'W') {
        d = -d;
    }
    *out = d;
    return true;
}

static int two(const char *p)
{
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static char sys_of(const char *talker, int prn)
{
    if (talker[1] == 'P') {
        return prn >= 33 && prn <= 64 ? 'S' : 'P';      /* GPS reports SBAS too */
    }
    if (talker[1] == 'N') {     /* combined: NMEA PRN ranges */
        if (prn >= 65 && prn <= 96) return 'L';
        if (prn >= 33 && prn <= 64) return 'S';
        return 'P';
    }
    return talker[1];           /* L, A, B, ... */
}

/* days since 1970-01-01 (proleptic Gregorian) */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* ---------------------------------------------------------------- sentences
 * All called with the lock held. */

static void maybe_set_clock(void)
{
    if (!s_st.fix || !s_st.time_valid || s_st.year < 2024) {
        return;
    }
    int64_t t = days_from_civil(s_st.year, s_st.month, s_st.day) * 86400
              + s_st.hour * 3600 + s_st.min * 60 + s_st.sec;
    time_t sys = time(NULL);
    if (llabs((long long)(t - (int64_t)sys)) > CLOCK_TOLERANCE_S) {
        struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_st.clock_set = true;
        ESP_LOGI(TAG, "system clock set from GPS: %04u-%02u-%02u %02u:%02u:%02u UTC",
                 s_st.year, s_st.month, s_st.day, s_st.hour, s_st.min, s_st.sec);
    }
}

static void on_rmc(char **f, int n)
{
    if (n < 10) {
        return;
    }
    if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
        s_st.hour = two(f[1]);
        s_st.min = two(f[1] + 2);
        s_st.sec = two(f[1] + 4);
        s_st.day = two(f[9]);
        s_st.month = two(f[9] + 2);
        s_st.year = 2000 + two(f[9] + 4);
        s_st.time_valid = s_st.month >= 1 && s_st.month <= 12 && s_st.day >= 1 && s_st.day <= 31;
    }
    s_st.fix = f[2][0] == 'A';
    if (s_st.fix) {
        double lat, lon;
        if (parse_coord(f[3], f[4], &lat) && parse_coord(f[5], f[6], &lon)) {
            s_st.lat = lat;
            s_st.lon = lon;
        }
        s_st.speed_kmh = f[7][0] ? (float)(atof(f[7]) * 1.852) : 0.0f;
        s_st.course_deg = f[8][0] ? (float)atof(f[8]) : -1.0f;
        s_fix_ms = now_ms();
        s_ever_fix = true;
        maybe_set_clock();
    }
}

static void on_gga(char **f, int n)
{
    if (n < 10) {
        return;
    }
    s_st.quality = (uint8_t)atoi(f[6]);
    s_st.sats_used = (uint8_t)atoi(f[7]);
    if (f[8][0]) s_st.hdop = (float)atof(f[8]);
    if (f[9][0]) s_st.alt_m = (float)atof(f[9]);
}

static void on_gsa(char **f, int n)
{
    if (n < 18) {
        return;
    }
    s_st.fix_type = (uint8_t)atoi(f[2]);
    uint32_t now = now_ms();
    for (int i = 3; i <= 14; i++) {
        int prn = atoi(f[i]);
        if (prn > 0 && prn < 256) {
            s_used_ms[prn] = now;
        }
    }
    if (f[15][0]) s_st.pdop = (float)atof(f[15]);
}

static void on_gsv(const char *talker, char **f, int n)
{
    uint32_t now = now_ms();
    for (int i = 4; i < n; i += 4) {
        int prn = atoi(f[i]);
        if (prn <= 0) {
            continue;
        }
        char sys = sys_of(talker, prn);
        int slot = -1, oldest = 0;
        for (int k = 0; k < GPS_MAX_SATS; k++) {
            if (s_sats[k].s.prn == prn && s_sats[k].s.sys == sys) {
                slot = k;
                break;
            }
            if (s_sats[k].seen_ms < s_sats[oldest].seen_ms) {
                oldest = k;
            }
        }
        if (slot < 0) {
            slot = oldest;
        }
        sat_entry_t *e = &s_sats[slot];
        e->s.sys = sys;
        e->s.prn = (uint8_t)prn;
        e->s.elev = i + 1 < n && f[i + 1][0] ? (int8_t)atoi(f[i + 1]) : -1;
        e->s.azim = i + 2 < n && f[i + 2][0] ? (int16_t)atoi(f[i + 2]) : -1;
        e->s.snr = i + 3 < n && f[i + 3][0] ? (int8_t)atoi(f[i + 3]) : -1;
        e->seen_ms = now;
    }
}

static void on_sentence(char *line)
{
    char copy[GPS_RAW_LEN];
    snprintf(copy, sizeof(copy), "%s", line);
    if (!checksum_ok(line)) {
        LOCK();
        s_st.cksum_err++;
        UNLOCK();
        return;
    }
    char *f[MAX_FIELDS];
    int n = split(line, f, MAX_FIELDS);
    if (strlen(f[0]) != 6) {        /* "$GPRMC" */
        return;
    }
    const char *talker = f[0] + 1;
    const char *type = f[0] + 3;

    LOCK();
    s_st.sentences++;
    s_last_ok_ms = now_ms();
    s_ever_ok = true;
    if (!s_st.baud_locked) {
        s_st.baud_locked = true;
        ESP_LOGI(TAG, "NMEA at %" PRIu32 " baud", s_st.baud);
    }
    snprintf(s_raw[s_raw_next], GPS_RAW_LEN, "%s", copy);
    s_raw_next = (s_raw_next + 1) % GPS_RAW_LINES;
    if (s_raw_count < GPS_RAW_LINES) s_raw_count++;

    if (!strcmp(type, "RMC"))      on_rmc(f, n);
    else if (!strcmp(type, "GGA")) on_gga(f, n);
    else if (!strcmp(type, "GSA")) on_gsa(f, n);
    else if (!strcmp(type, "GSV")) on_gsv(talker, f, n);
    UNLOCK();
}

/* ---------------------------------------------------------------- task */

static void gps_task(void *arg)
{
    uint8_t buf[128];
    char line[GPS_RAW_LEN];
    int len = 0;

    while (1) {
        int req = s_req;
        if (req != REQ_NONE) {
            s_req = REQ_NONE;
            uart_close();
            if (req >= 0) {
                len = 0;
                uart_open(req);
            }
            xSemaphoreGive(s_done);
        }
        if (!s_open) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            LOCK();
            s_st.bytes += n;
            UNLOCK();
        }
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '$') {
                len = 0;
            }
            if (c == '\r' || c == '\n') {
                if (len > 0 && line[0] == '$') {
                    line[len] = '\0';
                    on_sentence(line);
                }
                len = 0;
            } else if (len < (int)sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0;        /* too long: noise or wrong baud */
            }
        }

        uint32_t now = now_ms();
        LOCK();
        bool locked = s_st.baud_locked;
        if (locked && now - s_last_ok_ms > LOST_MS) {
            s_st.baud_locked = locked = false;      /* silent: search again */
            s_baud_since_ms = now;
        }
        UNLOCK();
        if (!locked && now - s_baud_since_ms > BAUD_TRY_MS) {
            next_baud();
        }
    }
}

/* ---------------------------------------------------------------- API */

esp_err_t gps_svc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_st.pin = -1;
    s_st.course_deg = -1;
    /* stack in PSRAM: the task never writes flash (would need the cache) */
    if (xTaskCreatePinnedToCoreWithCaps(gps_task, "gps_svc", 4096, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t gps_svc_enable(int pin)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_done, 0);      /* drop a stale signal */
    s_req = pin >= 0 ? pin : REQ_STOP;
    return xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static int sat_cmp(const void *a, const void *b)
{
    const gps_sat_t *x = a, *y = b;
    if (x->sys != y->sys) return x->sys - y->sys;
    return x->prn - y->prn;
}

void gps_svc_get_status(gps_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->pin = -1;
        return;
    }
    uint32_t now = now_ms();
    LOCK();
    *out = s_st;
    out->rx_age_ms = s_ever_ok ? now - s_last_ok_ms : UINT32_MAX;
    out->fix_age_ms = s_ever_fix ? now - s_fix_ms : UINT32_MAX;
    out->nsats = 0;
    for (int k = 0; k < GPS_MAX_SATS; k++) {
        const sat_entry_t *e = &s_sats[k];
        if (e->s.prn == 0 || now - e->seen_ms > SAT_TIMEOUT_MS) {
            continue;
        }
        gps_sat_t s = e->s;
        s.used = now - s_used_ms[s.prn] < USED_TIMEOUT_MS && s_used_ms[s.prn] != 0;
        out->sats[out->nsats++] = s;
    }
    out->nraw = (uint8_t)s_raw_count;
    for (unsigned i = 0; i < s_raw_count; i++) {
        unsigned idx = (s_raw_next + GPS_RAW_LINES - s_raw_count + i) % GPS_RAW_LINES;
        memcpy(out->raw[i], s_raw[idx], GPS_RAW_LEN);
    }
    UNLOCK();
    qsort(out->sats, out->nsats, sizeof(out->sats[0]), sat_cmp);
    out->sats_view = out->nsats;
    if (out->fix_age_ms > 3000) {
        out->fix = false;       /* no RMC with status A lately */
    }
    if (!out->enabled) {
        out->fix = false;
    }
}
