idf_component_register(SRCS "gps_svc.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_timer)
