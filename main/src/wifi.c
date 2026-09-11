
#include "wifi.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
static const char *TAG = "connect";
static esp_netif_t *s_sta_netif = NULL;
static SemaphoreHandle_t s_semph_get_ip_addrs = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

static int s_retry_num = 0;

static void wifi_reconnect_timer_cb(void *arg) {
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
    ESP_LOGE(TAG, "WiFi reconnect failed: %s", esp_err_to_name(err));
  }
}

static void handler_on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
  wifi_event_sta_disconnected_t *disconn = event_data;
  if (disconn->reason == WIFI_REASON_ROAMING) {
    ESP_LOGD(TAG, "station roaming, do nothing");
    return;
  }

  s_retry_num++;
  if (s_retry_num > CONFIG_WIFI_CONN_MAX_RETRY + 1) {
    s_retry_num = CONFIG_WIFI_CONN_MAX_RETRY + 1;
  }

  ESP_LOGI(TAG, "Wi-Fi disconnected (reason %d), attempt %d", disconn->reason,
           s_retry_num);

  /* During initial blocking connect, give up after MAX_RETRY and let caller
   * return.  Don't unregister handlers — the timer keeps trying. */
  if (s_semph_get_ip_addrs && s_retry_num > CONFIG_WIFI_CONN_MAX_RETRY) {
    ESP_LOGI(TAG, "WiFi connect failed %d times during boot", s_retry_num);
    xSemaphoreGive(s_semph_get_ip_addrs);
    return;
  }

  /* Fast retries (immediate) up to MAX_RETRY */
  if (s_retry_num <= CONFIG_WIFI_CONN_MAX_RETRY) {
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
      return;
    }
    ESP_ERROR_CHECK(err);
    return;
  }

  /* Beyond MAX_RETRY: backoff via timer (non-blocking, no event-loop stall) */
  ESP_LOGI(TAG, "WiFi reconnecting in 5s...");
  esp_timer_start_once(s_reconnect_timer, 5000000);
}

static bool is_our_netif(const char *prefix, esp_netif_t *netif) {
  return strncmp(prefix, esp_netif_get_desc(netif), strlen(prefix) - 1) == 0;
}
static void handler_on_wifi_connect(void *esp_netif,
                                    esp_event_base_t event_base,
                                    int32_t event_id, void *event_data) {}

static void handler_on_sta_got_ip(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  s_retry_num = 0;
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  if (!is_our_netif(NETIF_DESC_STA, event->esp_netif)) {
    return;
  }
  ESP_LOGI(TAG, "Got IPv4 event: Interface \"%s\" address: " IPSTR,
           esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
  if (s_semph_get_ip_addrs) {
    xSemaphoreGive(s_semph_get_ip_addrs);
  } else {
    ESP_LOGI(TAG, "- IPv4 address: " IPSTR ",", IP2STR(&event->ip_info.ip));
  }
}

static void wifi_start(void) {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_netif_inherent_config_t esp_netif_config =
      ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
  // Warning: the interface desc is used in tests to capture actual connection
  // details (IP, gw, mask)
  esp_netif_config.if_desc = NETIF_DESC_STA;
  esp_netif_config.route_prio = 128;
  s_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);
  esp_wifi_set_default_wifi_sta_handlers();

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_timer_create_args_t timer_args = {
      .callback = wifi_reconnect_timer_cb,
      .name = "wifi_reconnect",
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));
}

static void wifi_stop(void) {
  if (s_reconnect_timer) {
    esp_timer_stop(s_reconnect_timer);
    esp_timer_delete(s_reconnect_timer);
    s_reconnect_timer = NULL;
  }
  esp_err_t err = esp_wifi_stop();
  if (err == ESP_ERR_WIFI_NOT_INIT) {
    return;
  }
  ESP_ERROR_CHECK(err);
  ESP_ERROR_CHECK(esp_wifi_deinit());
  ESP_ERROR_CHECK(esp_wifi_clear_default_wifi_driver_and_handlers(s_sta_netif));
  esp_netif_destroy(s_sta_netif);
  s_sta_netif = NULL;
}

esp_err_t wifi_sta_do_connect(wifi_config_t wifi_config, bool wait) {
  if (wait) {
    s_semph_get_ip_addrs = xSemaphoreCreateBinary();
    if (s_semph_get_ip_addrs == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  s_retry_num = 0;
  ESP_ERROR_CHECK(
      esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                 &handler_on_wifi_disconnect, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &handler_on_sta_got_ip, NULL));
  ESP_ERROR_CHECK(
      esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                 &handler_on_wifi_connect, s_sta_netif));

  ESP_LOGI(TAG, "Connecting to %s...", wifi_config.sta.ssid);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  esp_err_t ret = esp_wifi_connect();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "WiFi connect failed! ret:%x", ret);
    return ret;
  }
  if (wait) {
    ESP_LOGI(TAG, "Waiting for IP(s)");
    xSemaphoreTake(s_semph_get_ip_addrs, portMAX_DELAY);
    vSemaphoreDelete(s_semph_get_ip_addrs);
    s_semph_get_ip_addrs = NULL;
    if (s_retry_num > CONFIG_WIFI_CONN_MAX_RETRY) {
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

esp_err_t wifi_sta_do_disconnect(void) {
  ESP_ERROR_CHECK(esp_event_handler_unregister(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &handler_on_wifi_disconnect));
  ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &handler_on_sta_got_ip));
  ESP_ERROR_CHECK(esp_event_handler_unregister(
      WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &handler_on_wifi_connect));
#if CONFIG_CONNECT_IPV6
  ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6,
                                               &handler_on_sta_got_ipv6));
#endif
  return esp_wifi_disconnect();
}

void wifi_shutdown(void) {
  wifi_sta_do_disconnect();
  wifi_stop();
}

esp_err_t wifi_connect(void) {
  ESP_LOGI(TAG, "Start connect.");
  wifi_start();
  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = SSID,
              .password = PASSWORD,
              .scan_method = WIFI_ALL_CHANNEL_SCAN,
              .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
              .threshold.rssi = -127,
              .threshold.authmode = WIFI_AUTH_OPEN,
          },
  };
  return wifi_sta_do_connect(wifi_config, true);
}
