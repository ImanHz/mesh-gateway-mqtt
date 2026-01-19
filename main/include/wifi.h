#ifndef INCLUDE_INCLUDE_WIFI_H_
#define INCLUDE_INCLUDE_WIFI_H_

#include "esp_err.h"
#include "esp_wifi_types_generic.h"

#define SSID "TP-Link_7069"
#define PASSWORD "85784918"
#define CONFIG_WIFI_CONN_MAX_RETRY 6
#define NETIF_DESC_STA "example_netif_sta"

esp_err_t wifi_sta_do_connect(wifi_config_t wifi_config, bool wait);
esp_err_t wifi_sta_do_disconnect(void);
esp_err_t wifi_connect(void);
void wifi_shutdown(void);

#endif // INCLUDE_INCLUDE_WIFI_H_
