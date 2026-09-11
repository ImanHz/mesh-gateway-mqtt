#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "CONFIG";

static void load_str(nvs_handle_t h, const char *key, char *out, size_t max) {
  size_t len = max;
  if (nvs_get_str(h, key, out, &len) != ESP_OK) {
    out[0] = '\0';
  }
}

bool config_read(app_config_t *cfg) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
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
