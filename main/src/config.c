#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "CONFIG";

static void load_str(nvs_handle_t h, const char *key, char *out, size_t max) {
  char tmp[128];
  size_t len = sizeof(tmp);
  if (nvs_get_str(h, key, tmp, &len) == ESP_OK && tmp[0] != '\0') {
    strncpy(out, tmp, max);
  }
}

bool config_read(app_config_t *cfg) {
  // Start with Kconfig defaults
  strncpy(cfg->ssid, CONFIG_DEFAULT_WIFI_SSID, sizeof(cfg->ssid));
  strncpy(cfg->password, CONFIG_DEFAULT_WIFI_PASSWORD, sizeof(cfg->password));
  strncpy(cfg->broker_addr, CONFIG_DEFAULT_BROKER_ADDR,
          sizeof(cfg->broker_addr));

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
    ESP_LOGI(TAG, "Using Kconfig defaults");
    return false;
  }

  uint8_t provisioned = 0;
  if (nvs_get_u8(h, NVS_KEY_PROVISIONED, &provisioned) != ESP_OK ||
      !provisioned) {
    nvs_close(h);
    return false;
  }

  load_str(h, NVS_KEY_SSID, cfg->ssid, sizeof(cfg->ssid));
  load_str(h, NVS_KEY_PASSWORD, cfg->password, sizeof(cfg->password));
  load_str(h, NVS_KEY_BROKER_ADDR, cfg->broker_addr,
           sizeof(cfg->broker_addr));
  nvs_close(h);

  ESP_LOGI(TAG, "Loaded config from NVS: ssid=%s broker=%s", cfg->ssid,
           cfg->broker_addr);
  return true;
}

esp_err_t config_write(const app_config_t *cfg) {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));

  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_SSID, cfg->ssid));
  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_PASSWORD, cfg->password));
  ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_BROKER_ADDR, cfg->broker_addr));
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

esp_err_t config_set_reprov_flag(void) {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  ESP_ERROR_CHECK(nvs_set_u8(h, NVS_KEY_REPROV, 1));
  ESP_ERROR_CHECK(nvs_commit(h));
  nvs_close(h);
  ESP_LOGW(TAG, "Re-provisioning flag set");
  return ESP_OK;
}

bool config_get_reprov_flag(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    return false;
  uint8_t flag = 0;
  nvs_get_u8(h, NVS_KEY_REPROV, &flag);
  nvs_close(h);
  return flag == 1;
}

esp_err_t config_clear_reprov_flag(void) {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  ESP_ERROR_CHECK(nvs_set_u8(h, NVS_KEY_REPROV, 0));
  ESP_ERROR_CHECK(nvs_commit(h));
  nvs_close(h);
  return ESP_OK;
}

void config_build_derived(app_config_t *cfg) {
  // Build broker URL
  snprintf(cfg->broker_url, sizeof(cfg->broker_url), "mqtt://%s:1883",
           cfg->broker_addr);

  // Build topics from device MAC: site/<mac12hex>/...
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char mac_str[13];
  snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);

  snprintf(cfg->pub_topic, sizeof(cfg->pub_topic), "site/%s/telemetry",
           mac_str);
  snprintf(cfg->sub_topic, sizeof(cfg->sub_topic), "site/%s/cmd", mac_str);
  snprintf(cfg->will_topic, sizeof(cfg->will_topic), "site/%s/status",
           mac_str);

  ESP_LOGI(TAG, "Derived: url=%s pub=%s sub=%s will=%s", cfg->broker_url,
           cfg->pub_topic, cfg->sub_topic, cfg->will_topic);
}
