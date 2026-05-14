idf_component_register(SRCS "espnow_example_main.c"
                    PRIV_REQUIRES button led_rgb espnow_manager pairing nvs_flash
                    INCLUDE_DIRS ".")
