#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "CONFIG";

static void load_str(nvs_handle_t h, const char *key, char *out, size_t max) {
  char tmp[128];
  size_t len = sizeof(tmp);
  if (nvs_get_str(h, key, tmp, &len) == ESP_OK && tmp[0] != '\0') {
    strncpy(out, tmp, max);
  }
  // Otherwise keep the Kconfig default already in 'out'
}

bool config_read(app_config_t *cfg) {
  // Start with Kconfig defaults
  strncpy(cfg->ssid, CONFIG_DEFAULT_WIFI_SSID, sizeof(cfg->ssid));
  strncpy(cfg->password, CONFIG_DEFAULT_WIFI_PASSWORD, sizeof(cfg->password));
  strncpy(cfg->broker_url, CONFIG_DEFAULT_BROKER_URL, sizeof(cfg->broker_url));
  strncpy(cfg->pub_topic, CONFIG_DEFAULT_PUB_TOPIC, sizeof(cfg->pub_topic));
  strncpy(cfg->will_topic, CONFIG_DEFAULT_WILL_TOPIC, sizeof(cfg->will_topic));

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
    ESP_LOGI(TAG, "Using Kconfig defaults");
    return true;
  }

  // uint8_t provisioned = 0;
  // if (nvs_get_u8(h, NVS_KEY_PROVISIONED, &provisioned) != ESP_OK ||
  //     !provisioned) {
  //   nvs_close(h);
  //   return false;
  // }

  load_str(h, NVS_KEY_SSID, cfg->ssid, sizeof(cfg->ssid));
  load_str(h, NVS_KEY_PASSWORD, cfg->password, sizeof(cfg->password));
  load_str(h, NVS_KEY_BROKER_URL, cfg->broker_url, sizeof(cfg->broker_url));
  load_str(h, NVS_KEY_PUB_TOPIC, cfg->pub_topic, sizeof(cfg->pub_topic));
  load_str(h, NVS_KEY_WILL_TOPIC, cfg->will_topic, sizeof(cfg->will_topic));
  nvs_close(h);

  ESP_LOGI(TAG, "Loaded config from NVS: ssid=%s broker=%s", cfg->ssid,
           cfg->broker_url);
  return true;
}

esp_err_t config_write(const app_config_t *cfg) {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));

  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_SSID, cfg->ssid));
  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_PASSWORD, cfg->password));
  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_BROKER_URL, cfg->broker_url));
  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_PUB_TOPIC, cfg->pub_topic));
  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_WILL_TOPIC, cfg->will_topic));
  ESP_ERROR_CHECK(nvs_set_u8(h, NVS_KEY_PROVISIONED, 1));
  ESP_ERROR_CHECK(nvs_commit(h));
  nvs_close(h);

  ESP_LOGI(TAG, "Config saved to NVS");
  return ESP_OK;
}

esp_err_t config_clear_provisioned(void) {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  ESP_ERROR_CHECK(nvs_set_u8(h, NVS_KEY_PROVISIONED, 0));
  ESP_ERROR_CHECK(nvs_commit(h));
  nvs_close(h);

  ESP_LOGW(TAG, "Provisioning flag cleared");
  return ESP_OK;
}
