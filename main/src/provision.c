#include "provision.h"
#include "config.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocomm.h"
#include "protocomm_httpd.h"
#include "protocomm_security0.h"
#include <string.h>

static const char *TAG = "PROV";

#define PROV_AP_SSID_PREFIX "PROV_"
#define PROV_TIMEOUT_US (5 * 60 * 1000000ULL) // 5 minutes
#define RESET_GPIO 4

// --- Minimal JSON string extractor (no cJSON dependency) ---

static bool json_extract_str(const char *json, const char *key, char *out,
                             size_t max) {
  char needle[32];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if (!p)
    return false;
  p = strchr(p + strlen(needle), ':');
  if (!p)
    return false;
  p++;
  while (*p == ' ')
    p++;
  if (*p != '"')
    return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < max - 1) {
    if (*p == '\\' && *(p + 1)) {
      p++;
    }
    out[i++] = *p++;
  }
  out[i] = '\0';
  return i > 0;
}

// --- Button detection ---

bool provision_button_held(void) {
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << RESET_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);
  vTaskDelay(pdMS_TO_TICKS(50));
  return gpio_get_level(RESET_GPIO) == 0;
}

// --- SoftAP + HTTP provisioning ---

static esp_netif_t *s_ap_netif = NULL;
static protocomm_t *s_pc = NULL;
static app_config_t s_prov_cfg;

static esp_err_t prov_handler(uint32_t session_id, const uint8_t *inbuf,
                               ssize_t inlen, uint8_t **outbuf,
                               ssize_t *outlen, void *priv_data) {
  const char *json = (const char *)inbuf;

  memset(&s_prov_cfg, 0, sizeof(s_prov_cfg));

  if (!json_extract_str(json, "ssid", s_prov_cfg.ssid,
                        sizeof(s_prov_cfg.ssid)) ||
      !json_extract_str(json, "password", s_prov_cfg.password,
                        sizeof(s_prov_cfg.password)) ||
      !json_extract_str(json, "broker_url", s_prov_cfg.broker_url,
                        sizeof(s_prov_cfg.broker_url)) ||
      !json_extract_str(json, "pub_topic", s_prov_cfg.pub_topic,
                        sizeof(s_prov_cfg.pub_topic)) ||
      !json_extract_str(json, "will_topic", s_prov_cfg.will_topic,
                        sizeof(s_prov_cfg.will_topic))) {
    ESP_LOGE(TAG, "Invalid config JSON");
    const char *err = "{\"status\":\"error\",\"msg\":\"missing fields\"}";
    *outbuf = (uint8_t *)strdup(err);
    *outlen = strlen(err);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Config received: ssid=%s broker=%s", s_prov_cfg.ssid,
           s_prov_cfg.broker_url);

  const char *ok = "{\"status\":\"ok\"}";
  *outbuf = (uint8_t *)strdup(ok);
  *outlen = strlen(ok);
  return ESP_OK;
}

static void stop_prov_wifi(void) {
  if (s_pc) {
    protocomm_httpd_stop(s_pc);
    protocomm_set_security(s_pc, "prov", NULL, NULL);
    protocomm_delete(s_pc);
    s_pc = NULL;
  }
  if (s_ap_netif) {
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = NULL;
  }
}

esp_err_t provision_start(void) {
  ESP_LOGW(TAG, "Entering provisioning mode (5 min timeout)");

  // Create AP netif
  s_ap_netif = esp_netif_create_default_wifi_ap();

  // Generate AP SSID from MAC
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char ap_ssid[32];
  snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", PROV_AP_SSID_PREFIX,
           mac[4], mac[5]);

  wifi_config_t ap_cfg = {
      .ap =
          {
              .ssid_len = strlen(ap_ssid),
              .channel = 1,
              .authmode = WIFI_AUTH_OPEN,
              .max_connection = 4,
          },
  };
  memcpy(ap_cfg.ap.ssid, ap_ssid, strlen(ap_ssid));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "AP started: %s (open)", ap_ssid);

  // Start protocomm with HTTP transport
  s_pc = protocomm_new();
  if (!s_pc) {
    ESP_LOGE(TAG, "Failed to create protocomm");
    stop_prov_wifi();
    return ESP_FAIL;
  }

  ESP_ERROR_CHECK(protocomm_set_security(s_pc, "prov", &protocomm_security0,
                                         NULL));
  ESP_ERROR_CHECK(
      protocomm_add_endpoint(s_pc, "prov", prov_handler, NULL));

  protocomm_httpd_config_t http_cfg = {
      .ext_handle_provided = false,
      .data.config = PROTOCOMM_HTTPD_DEFAULT_CONFIG(),
  };
  ESP_ERROR_CHECK(protocomm_httpd_start(s_pc, &http_cfg));

  ESP_LOGI(TAG, "Provisioning endpoint ready at http://192.168.4.1/prov");

  // Wait for config or timeout
  int64_t start = esp_timer_get_time();
  while (esp_timer_get_time() - start < PROV_TIMEOUT_US) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Check if config was received (prov_handler sets ssid)
    if (s_prov_cfg.ssid[0] != '\0') {
      ESP_LOGI(TAG, "Config received, saving and restarting");
      config_write(&s_prov_cfg);
      stop_prov_wifi();
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
      return ESP_OK; // unreachable
    }
  }

  ESP_LOGW(TAG, "Provisioning timeout, restarting");
  stop_prov_wifi();
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_ERR_TIMEOUT; // unreachable
}
