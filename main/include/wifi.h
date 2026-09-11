#ifndef INCLUDE_WIFI_H_
#define INCLUDE_WIFI_H_

#include "config.h"
#include "esp_err.h"
#include "esp_wifi_types_generic.h"

#define CONFIG_WIFI_CONN_MAX_RETRY 6
#define NETIF_DESC_STA "example_netif_sta"

esp_err_t wifi_sta_do_connect(wifi_config_t wifi_config, bool wait);
esp_err_t wifi_sta_do_disconnect(void);
esp_err_t wifi_connect(const app_config_t *cfg);
void wifi_shutdown(void);

#endif // INCLUDE_WIFI_H_
