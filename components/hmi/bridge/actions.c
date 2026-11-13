/*
 * EEZ Studio native actions (declared in the generated src/ui/actions.h).
 * Every action in the EEZ project needs a function here or the link fails.
 * Keep them thin: the work is in the views.
 */
#include "actions.h"
#include "bridge.h"

void action_nav_home(lv_event_t *e)        { nav_go("home"); }
void action_nav_wifi(lv_event_t *e)        { nav_go("wifi"); }
void action_nav_ble(lv_event_t *e)         { nav_go("ble"); }
void action_nav_can(lv_event_t *e)         { nav_go("can"); }
void action_nav_io(lv_event_t *e)          { nav_go("io"); }
void action_nav_gps(lv_event_t *e)         { nav_go("gps"); }
void action_nav_route(lv_event_t *e)       { nav_go("route"); }
void action_nav_profile(lv_event_t *e)     { nav_go("profile"); }
void action_nav_ride(lv_event_t *e)        { nav_go("ride"); }
void action_nav_display(lv_event_t *e)     { nav_go("display"); }

void action_wifi_scan(lv_event_t *e)       { view_wifi_scan(); }
void action_wifi_connect(lv_event_t *e)    { view_wifi_connect(); }
void action_wifi_disconnect(lv_event_t *e) { view_wifi_disconnect(); }
void action_wifi_forget(lv_event_t *e)     { view_wifi_forget(); }

void action_ble_scan(lv_event_t *e)        { view_ble_scan(); }
void action_ble_connect(lv_event_t *e)     { view_ble_connect(); }
void action_ble_disconnect(lv_event_t *e)  { view_ble_disconnect(); }
void action_ble_forget(lv_event_t *e)      { view_ble_forget(); }

void action_can_start_stop(lv_event_t *e)  { view_can_start_stop(); }
void action_can_log_toggle(lv_event_t *e)  { view_can_log_toggle(); }
void action_can_reset(lv_event_t *e)       { view_can_reset(); }

void action_io_toggle_out(lv_event_t *e)   { view_io_toggle_out(); }
void action_io_reset(lv_event_t *e)        { view_io_reset(); }
void action_i2c_scan(lv_event_t *e)        { view_io_i2c_scan(); }

void action_gps_enable(lv_event_t *e)      { view_gps_enable(); }

void action_gpx_load(lv_event_t *e)        { view_route_load(); }
void action_gpx_rescan(lv_event_t *e)      { view_route_rescan(); }
void action_gpx_reverse(lv_event_t *e)     { view_route_reverse(); }
void action_route_zoom_in(lv_event_t *e)   { view_route_zoom(+1); }
void action_route_zoom_out(lv_event_t *e)  { view_route_zoom(-1); }
void action_route_view(lv_event_t *e)      { view_route_toggle_view(); }
void action_profile_view(lv_event_t *e)    { view_profile_toggle_view(); }
void action_ride_start(lv_event_t *e)      { view_ride_start(); }     /* SHORT_CLICKED */
void action_ride_stop(lv_event_t *e)       { view_ride_stop(); }      /* LONG_PRESSED */

void action_sd_remount(lv_event_t *e)      { view_display_sd_remount(); }
void action_screen_off(lv_event_t *e)      { screen_off(); }
void action_restart(lv_event_t *e)         { view_display_restart(); }
