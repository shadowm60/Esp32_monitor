idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES board settings hmi wifi_svc ble_svc canmon io_svc gps_svc gpx_svc ride_svc web_svc)
