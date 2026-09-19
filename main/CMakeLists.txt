idf_component_register(SRCS "main.c"
                               "src/wifi.c"
                               "src/uart.c"
                               "src/mqtt.c"
                               "src/config.c"
                               "src/provision.c"
                       INCLUDE_DIRS "include/"
                       EMBED_TXTFILES "${CMAKE_SOURCE_DIR}/certs/ca.pem"
                                     "${CMAKE_SOURCE_DIR}/certs/cert.pem"
                                     "${CMAKE_SOURCE_DIR}/certs/key.pem"
                       PRIV_REQUIRES mqtt nvs_flash esp_netif
                                     esp_wifi esp_http_server protocomm
                                     esp_driver_gpio esp_driver_uart)
