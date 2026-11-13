idf_component_register(SRCS "settings.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash
                    PRIV_REQUIRES esp_timer)
