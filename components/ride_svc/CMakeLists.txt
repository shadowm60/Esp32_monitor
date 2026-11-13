idf_component_register(SRCS "ride_svc.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES board gps_svc esp_timer fatfs)
