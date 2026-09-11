
#include "main.h"
#include "esp_log.h"
#include "esp_log_buffer.h"

#include "config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "gpio.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "provision.h"
#include "uart.h"
#include "wifi.h"

void app_main(void) {
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes",
           esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Check if reset button is held → force provisioning
  bool force_provision = provision_button_held();
  if (force_provision) {
    ESP_LOGW(TAG, "Reset button held, clearing provisioning config");
    config_clear_provisioned();
  }

  // Load config from NVS (or defaults)
  app_config_t cfg = {0};
  bool provisioned = config_read(&cfg);

  if (!provisioned) {
    // No config in NVS → enter SoftAP provisioning
    ESP_LOGW(TAG, "Not provisioned, entering provisioning mode");
    provision_start(); // blocks until done, then restarts
    return;           // unreachable
  }

  // Normal boot: start UART, WiFi, MQTT
  uart_init(mqtt_callback);

  if (wifi_connect(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "WIFI CONNECT ERROR");
    return;
  }
  ESP_ERROR_CHECK(esp_register_shutdown_handler(&wifi_shutdown));

  mqtt5_app_start(&cfg);
}
