/*
 * web_svc - SD card file manager over Wi-Fi (HTTP, port 80).
 *
 * Open http://<board IP>/ in a browser on the same network:
 *   - lists the folders gpx/, rides/, canlog/ (name, size, free space)
 *   - download, delete; upload into gpx/ (several files at once)
 *
 *   GET  /                      the page (one HTML file, no external assets)
 *   GET  /api/list              JSON: folders, files, card size / free
 *   GET  /dl/<folder>/<file>    download
 *   POST /up/<folder>/<file>    upload, body = the file
 *   POST /del/<folder>/<file>   delete
 *
 * Only those three folders, plain file names (no '/', no ".."). No
 * password: anyone on the same network can use it. The server task's stack
 * is in PSRAM; files are streamed in 8 KB blocks. After an upload into gpx/
 * the GPX list is rescanned.
 */
#ifndef WEB_SVC_H
#define WEB_SVC_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_svc_init(void);

bool web_svc_running(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SVC_H */
