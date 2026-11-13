idf_component_register(SRCS "canmon.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES can_if board esp_timer fatfs)
