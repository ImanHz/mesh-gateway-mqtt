idf_component_register(SRCS "main.c"
                               "src/wifi.c"
                               "src/uart.c"
                               "src/mqtt.c"
                               "src/config.c"
                               "src/provision.c"
                       INCLUDE_DIRS "include/"
                       PRIV_REQUIRES esp-mqtt-managed nvs_flash esp_netif
                                     esp_wifi esp_http_server protocomm
                                     esp_driver_gpio esp_driver_uart)
