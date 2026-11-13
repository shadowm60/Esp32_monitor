# UI: EEZ Studio generated code (src/ui, never edit by hand: change the EEZ
# project and run tools/eez-build.sh) + bridge (hand written, the only code
# touching EEZ variables / LVGL).
set(var_table "${CMAKE_CURRENT_BINARY_DIR}/eez_var_table.c")

idf_component_register(SRCS "./src/ui/images.c"
                            "./src/ui/screens.c"
                            "./src/ui/styles.c"
                            "./src/ui/ui.c"
                            "./src/ui/eez-flow.cpp"
                            "./bridge/eez_vars.cpp"
                            "./bridge/uivar.c"
                            "./bridge/hmi.c"
                            "./bridge/nav.c"
                            "./bridge/screen.c"
                            "./bridge/brightness.c"
                            "./bridge/actions.c"
                            "./bridge/view_sys.c"
                            "./bridge/view_wifi.c"
                            "./bridge/view_ble.c"
                            "./bridge/view_can.c"
                            "./bridge/view_io.c"
                            "./bridge/view_gps.c"
                            "./bridge/view_route.c"
                            "./bridge/view_profile.c"
                            "./bridge/view_ride.c"
                            "./bridge/sun.c"
                            "./bridge/navcue.c"
                            "${var_table}"
                    INCLUDE_DIRS "./bridge"
                    PRIV_INCLUDE_DIRS "./src/ui"
                    REQUIRES lvgl
                    PRIV_REQUIRES board settings wifi_svc ble_svc canmon io_svc gps_svc gpx_svc ride_svc web_svc
                                  esp_app_format esp_driver_ledc esp_psram esp_timer fatfs spi_flash)

# EEZ variable and screen names -> indices, from the generated vars.h / screens.h
idf_build_get_property(python PYTHON)
set(gen_script "${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_eez_var_table.py")
set(vars_h "${CMAKE_CURRENT_LIST_DIR}/src/ui/vars.h")
set(screens_h "${CMAKE_CURRENT_LIST_DIR}/src/ui/screens.h")
add_custom_command(OUTPUT "${var_table}"
                   COMMAND ${python} "${gen_script}" "${vars_h}" "${screens_h}" "${var_table}"
                   DEPENDS "${vars_h}" "${screens_h}" "${gen_script}"
                   COMMENT "Generating EEZ variable / screen tables"
                   VERBATIM)
