idf_component_register(SRCS "web_svc.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES board gpx_svc esp_http_server fatfs)
