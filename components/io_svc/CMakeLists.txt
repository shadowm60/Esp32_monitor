idf_component_register(SRCS "io_svc.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES board gps_svc esp_adc esp_driver_gpio esp_driver_tsens esp_driver_i2c)
