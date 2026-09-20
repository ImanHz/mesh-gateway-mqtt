
#include "main.h"
#include "esp_log.h"
#include "esp_log_buffer.h"

#include "config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "provision.h"
#include "uart.h"
#include "wifi.h"

static app_config_t s_cfg;

// Called by button task when held 5s — stop all services, set flag, restart
static void on_button_provision(void) {
  ESP_LOGW(TAG, "Button held — stopping services, restarting into provisioning");
  mqtt5_stop();
  uart_stop();
  wifi_shutdown();
  config_set_reprov_flag();
  esp_restart();
}

void app_main(void) {
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes",
           esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Check if reprov flag is set (button-triggered restart)
  if (config_get_reprov_flag()) {
    config_clear_reprov_flag();
    ESP_LOGW(TAG, "Re-provisioning flag set, entering provisioning mode");
    provision_start(); // blocks until form submitted or button cancel, then restarts
    return;           // unreachable
  }

  // Load config from NVS (or defaults)
  bool provisioned = config_read(&s_cfg);

  if (!provisioned) {
    ESP_LOGW(TAG, "Not provisioned, entering provisioning mode");
    provision_start(); // blocks until form submitted or button cancel, then restarts
    return;           // unreachable
  }

  // Build broker URL and topics from broker_addr + device MAC
  config_build_derived(&s_cfg);

  // Normal boot: start UART, WiFi, MQTT
  uart_init(mqtt_callback);

  if (wifi_connect(&s_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "WIFI CONNECT ERROR");
    return;
  }
  ESP_ERROR_CHECK(esp_register_shutdown_handler(&wifi_shutdown));

  mqtt5_app_start(&s_cfg);

  // Start background button monitor (5s hold → re-provision via restart)
  provision_button_task(on_button_provision);
}
