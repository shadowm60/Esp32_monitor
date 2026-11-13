#include "web_svc.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_vfs_fat.h"
#include "sd_card.h"
#include "gpx_svc.h"

static const char *TAG = "web_svc";

#define BLOCK       8192
#define NAME_MAX_   64

typedef struct {
    const char *key;        /* URL / page name */
    const char *path;       /* on the card */
} folder_t;

static const folder_t FOLDERS[] = {
    { "gpx",    CONFIG_MON_GPX_DIR },
    { "rides",  CONFIG_MON_RIDE_DIR },
    { "canlog", CONFIG_MON_CANLOG_DIR },
};
#define NFOLDERS (sizeof(FOLDERS) / sizeof(FOLDERS[0]))

static httpd_handle_t s_srv;

/* ---------------------------------------------------------------- page */

static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Esp32 monitor - SD card</title><style>"
"body{font:15px system-ui,sans-serif;background:#0b0f14;color:#dde3ea;margin:0;padding:16px;max-width:900px}"
"h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:24px 0 8px;color:#8a96a3}"
"table{width:100%;border-collapse:collapse}td{padding:6px 8px;border-bottom:1px solid #253041}"
"td.s{text-align:right;color:#8a96a3;white-space:nowrap;width:1%}"
"a{color:#5aa2ff;text-decoration:none}button{background:#22303f;color:#dde3ea;border:1px solid #253041;"
"border-radius:6px;padding:4px 10px;cursor:pointer}button.del{background:#5a2020}"
".up{margin:8px 0;padding:12px;border:1px dashed #253041;border-radius:8px}#msg{color:#3ecf6e;min-height:20px}"
"</style></head><body><h1>Esp32 monitor &ndash; SD card</h1><div id=card></div><div id=msg></div>"
"<div class=up>Upload GPX files to <b>gpx/</b>: <input type=file id=f multiple accept='.gpx'> "
"<button onclick=up()>Upload</button></div><div id=list></div><script>"
"function sz(b){return b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' kB':(b/1048576).toFixed(1)+' MB'}"
"function esc(s){return s.replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
"function msg(t){document.getElementById('msg').textContent=t}"
"async function load(){let r=await fetch('/api/list');let j=await r.json();"
"document.getElementById('card').textContent=j.sd?('Card: '+sz(j.free)+' free of '+sz(j.total)):'No SD card';"
"let h='';for(const d of j.dirs){h+='<h2>'+d.name+'/ ('+d.files.length+')</h2><table>';"
"if(!d.files.length)h+='<tr><td>(empty)</td></tr>';"
"for(const f of d.files){let u=d.name+'/'+encodeURIComponent(f.n);"
"h+='<tr><td><a href=\"/dl/'+u+'\" download>'+esc(f.n)+'</a></td><td class=s>'+sz(f.s)+'</td>'+"
"'<td class=s><button class=del data-u=\"'+esc(u)+'\" onclick=del(this.dataset.u)>Delete</button></td></tr>'}h+='</table>'}"
"document.getElementById('list').innerHTML=h}"
"async function del(u){if(!confirm('Delete '+decodeURIComponent(u)+'?'))return;"
"let r=await fetch('/del/'+u,{method:'POST'});msg(r.ok?'Deleted':'Delete failed: '+await r.text());load()}"
"async function up(){let fs=document.getElementById('f').files;if(!fs.length)return;"
"for(const f of fs){msg('Uploading '+f.name+' ...');"
"let r=await fetch('/up/gpx/'+encodeURIComponent(f.name),{method:'POST',body:f});"
"if(!r.ok){msg('Upload of '+f.name+' failed: '+await r.text());load();return}}"
"msg(fs.length+' file(s) uploaded');document.getElementById('f').value='';load()}"
"load()</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

/* ---------------------------------------------------------------- helpers */

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "/dl/gpx/My%20ride.gpx" after prefix -> folder + decoded, checked file name */
static const folder_t *parse_target(const char *uri, const char *prefix, char *name, size_t len)
{
    const char *p = uri + strlen(prefix);
    const char *slash = strchr(p, '/');
    if (slash == NULL) return NULL;
    const folder_t *fo = NULL;
    for (size_t i = 0; i < NFOLDERS; i++) {
        if (strlen(FOLDERS[i].key) == (size_t)(slash - p) && strncmp(p, FOLDERS[i].key, slash - p) == 0) {
            fo = &FOLDERS[i];
        }
    }
    if (fo == NULL) return NULL;
    size_t n = 0;
    for (const char *s = slash + 1; *s && *s != '?'; s++) {
        char c = *s;
        if (c == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
            c = (char)(hexv(s[1]) << 4 | hexv(s[2]));
            s += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (n + 1 >= len) return NULL;
        name[n++] = c;
    }
    name[n] = '\0';
    /* plain names only: no path, no control characters, not "." / ".." */
    if (n == 0 || name[0] == '.' || strpbrk(name, "/\\:*?\"<>|") != NULL) return NULL;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)name[i] < 0x20) return NULL;
    }
    return fo;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

/* JSON string with escapes (file names) */
static void json_str(httpd_req_t *req, const char *s)
{
    char buf[NAME_MAX_ * 2 + 4];
    size_t n = 0;
    buf[n++] = '"';
    for (; *s && n < sizeof(buf) - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { buf[n++] = '\\'; buf[n++] = (char)c; }
        else if (c < 0x20)         n += snprintf(buf + n, sizeof(buf) - n, "\\u%04x", c);
        else                       buf[n++] = (char)c;
    }
    buf[n++] = '"';
    httpd_resp_send_chunk(req, buf, n);
}

/* ---------------------------------------------------------------- handlers */

static esp_err_t list_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char b[160];
    bool sd = waveshare_sd_card_mounted();
    uint64_t total = 0, free_b = 0;
    if (sd) esp_vfs_fat_info(MOUNT_POINT, &total, &free_b);
    snprintf(b, sizeof(b), "{\"sd\":%s,\"total\":%llu,\"free\":%llu,\"dirs\":[", sd ? "true" : "false",
             (unsigned long long)total, (unsigned long long)free_b);
    httpd_resp_sendstr_chunk(req, b);
    for (size_t i = 0; i < NFOLDERS; i++) {
        snprintf(b, sizeof(b), "%s{\"name\":\"%s\",\"files\":[", i ? "," : "", FOLDERS[i].key);
        httpd_resp_sendstr_chunk(req, b);
        DIR *d = sd ? opendir(FOLDERS[i].path) : NULL;
        int k = 0;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR || e->d_name[0] == '.') continue;
            char path[sizeof(CONFIG_MON_CANLOG_DIR) + sizeof(CONFIG_MON_RIDE_DIR) + sizeof(CONFIG_MON_GPX_DIR)
                      + sizeof(e->d_name) + 2];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", FOLDERS[i].path, e->d_name);
            long size = stat(path, &st) == 0 ? (long)st.st_size : 0;
            httpd_resp_sendstr_chunk(req, k++ ? ",{\"n\":" : "{\"n\":");
            json_str(req, e->d_name);
            snprintf(b, sizeof(b), ",\"s\":%ld}", size);
            httpd_resp_sendstr_chunk(req, b);
        }
        if (d) closedir(d);
        httpd_resp_sendstr_chunk(req, "]}");
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t dl_get(httpd_req_t *req)
{
    char name[NAME_MAX_], path[160];
    const folder_t *fo = parse_target(req->uri, "/dl/", name, sizeof(name));
    if (fo == NULL) return fail(req, "400 Bad Request", "bad file name");
    snprintf(path, sizeof(path), "%s/%s", fo->path, name);
    FILE *f = fopen(path, "r");
    if (f == NULL) return fail(req, "404 Not Found", "no such file");
    char *buf = heap_caps_malloc(BLOCK, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        return fail(req, "500 Internal Server Error", "out of memory");
    }
    const char *ext = strrchr(name, '.');
    httpd_resp_set_type(req, ext && strcasecmp(ext, ".gpx") == 0 ? "application/gpx+xml" : "application/octet-stream");
    char disp[NAME_MAX_ + 48];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    size_t r;
    esp_err_t err = ESP_OK;
    while ((r = fread(buf, 1, BLOCK, f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, r)) != ESP_OK) break;
    }
    fclose(f);
    heap_caps_free(buf);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "download %s/%s", fo->key, name);
    return err;
}

static esp_err_t up_post(httpd_req_t *req)
{
    char name[NAME_MAX_], path[160], tmp[168];
    const folder_t *fo = parse_target(req->uri, "/up/", name, sizeof(name));
    if (fo == NULL) return fail(req, "400 Bad Request", "bad file name");
    if (strcmp(fo->key, "gpx") != 0) return fail(req, "403 Forbidden", "uploads go to gpx/ only");
    if (!waveshare_sd_card_mounted()) return fail(req, "503 Service Unavailable", "no SD card");
    mkdir(fo->path, 0775);
    snprintf(path, sizeof(path), "%s/%s", fo->path, name);
    snprintf(tmp, sizeof(tmp), "%s/upload.tmp", fo->path);     /* renamed when complete */
    FILE *f = fopen(tmp, "w");
    if (f == NULL) return fail(req, "500 Internal Server Error", "cannot create the file");
    char *buf = heap_caps_malloc(BLOCK, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        return fail(req, "500 Internal Server Error", "out of memory");
    }
    size_t left = req->content_len;
    bool ok = true;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < BLOCK ? left : BLOCK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t)r) {
            ok = false;
            break;
        }
        left -= r;
    }
    heap_caps_free(buf);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        unlink(path);                   /* replace an existing file of that name */
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        unlink(tmp);
        return fail(req, "500 Internal Server Error", "write failed (card full?)");
    }
    ESP_LOGI(TAG, "upload %s/%s, %u bytes", fo->key, name, (unsigned)req->content_len);
    gpx_svc_rescan();
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t del_post(httpd_req_t *req)
{
    char name[NAME_MAX_], path[160];
    const folder_t *fo = parse_target(req->uri, "/del/", name, sizeof(name));
    if (fo == NULL) return fail(req, "400 Bad Request", "bad file name");
    snprintf(path, sizeof(path), "%s/%s", fo->path, name);
    if (unlink(path) != 0) return fail(req, "404 Not Found", "cannot delete (open, or not there)");
    ESP_LOGI(TAG, "deleted %s/%s", fo->key, name);
    if (strcmp(fo->key, "gpx") == 0) gpx_svc_rescan();
    return httpd_resp_sendstr(req, "ok");
}

/* ---------------------------------------------------------------- API */

esp_err_t web_svc_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;    /* SD + sockets only, never writes flash */
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.core_id = 0;
    esp_err_t err = httpd_start(&s_srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_srv = NULL;
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/list", .method = HTTP_GET,  .handler = list_get },
        { .uri = "/dl/*",     .method = HTTP_GET,  .handler = dl_get },
        { .uri = "/up/*",     .method = HTTP_POST, .handler = up_post },
        { .uri = "/del/*",    .method = HTTP_POST, .handler = del_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_srv, &uris[i]);
    }
    ESP_LOGI(TAG, "file manager on port %d", cfg.server_port);
    return ESP_OK;
}

bool web_svc_running(void)
{
    return s_srv != NULL;
}
