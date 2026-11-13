#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void action_nav_home(lv_event_t * e);
extern void action_nav_wifi(lv_event_t * e);
extern void action_nav_ble(lv_event_t * e);
extern void action_nav_can(lv_event_t * e);
extern void action_nav_io(lv_event_t * e);
extern void action_nav_display(lv_event_t * e);
extern void action_wifi_scan(lv_event_t * e);
extern void action_wifi_connect(lv_event_t * e);
extern void action_wifi_disconnect(lv_event_t * e);
extern void action_wifi_forget(lv_event_t * e);
extern void action_ble_scan(lv_event_t * e);
extern void action_ble_connect(lv_event_t * e);
extern void action_ble_disconnect(lv_event_t * e);
extern void action_ble_forget(lv_event_t * e);
extern void action_can_start_stop(lv_event_t * e);
extern void action_can_log_toggle(lv_event_t * e);
extern void action_can_reset(lv_event_t * e);
extern void action_io_toggle_out(lv_event_t * e);
extern void action_io_reset(lv_event_t * e);
extern void action_i2c_scan(lv_event_t * e);
extern void action_sd_remount(lv_event_t * e);
extern void action_screen_off(lv_event_t * e);
extern void action_restart(lv_event_t * e);
extern void action_nav_gps(lv_event_t * e);
extern void action_gps_enable(lv_event_t * e);
extern void action_nav_route(lv_event_t * e);
extern void action_gpx_load(lv_event_t * e);
extern void action_gpx_rescan(lv_event_t * e);
extern void action_route_zoom_in(lv_event_t * e);
extern void action_route_zoom_out(lv_event_t * e);
extern void action_route_view(lv_event_t * e);
extern void action_nav_profile(lv_event_t * e);
extern void action_profile_view(lv_event_t * e);
extern void action_nav_ride(lv_event_t * e);
extern void action_ride_start(lv_event_t * e);
extern void action_ride_stop(lv_event_t * e);
extern void action_gpx_reverse(lv_event_t * e);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/