idf_component_register(SRCS "power_switch_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio
                             esp_event
                             esp_netif
                             esp_wifi
                             led_strip
                             lwip
                             nvs_flash)
