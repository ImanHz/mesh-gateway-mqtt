
#include "main.h"
#include "esp_log.h"
#include "esp_log_buffer.h"

#include "config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "provision.h"
#include "uart.h"
#include "wifi.h"

static app_config_t s_cfg;

// Combined UART callback — routes by header byte
static void uart_dispatch(const uint8_t *data, size_t len) {
  if (len < 1)
    return;

  if (data[0] == 0xFF) {
    // Telemetry packet from mesh-client
    mqtt_telemetry_callback(data, len);
  } else if (data[0] == 0xFD) {
    // Command response from mesh-client
    mqtt_cmd_response_callback(data, len);
  } else {
    ESP_LOGW("UART_DISPATCH", "Unknown header: 0x%02x", data[0]);
  }
}

// Periodic task to check for timed-out pending commands
static void timeout_task(void *arg) {
  while (1) {
    mqtt_cmd_check_timeouts();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

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
    provision_start();
    // Timeout or cancel — restart to resume normal operation
    ESP_LOGW(TAG, "Provisioning ended, restarting");
    esp_restart();
    return;
  }

  // Load config from NVS (or defaults)
  bool provisioned = config_read(&s_cfg);

  if (!provisioned) {
    ESP_LOGW(TAG, "Not provisioned, entering provisioning mode");
    provision_start();
    // Timeout or cancel — restart to try again
    ESP_LOGW(TAG, "Provisioning ended, restarting");
    esp_restart();
    return;
  }

  // Build broker URL and topics from broker_addr + device MAC
  config_build_derived(&s_cfg);

  // Normal boot: start UART, WiFi, MQTT
  uart_init(uart_dispatch);

  if (wifi_connect(&s_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "WIFI CONNECT ERROR");
    return;
  }
  ESP_ERROR_CHECK(esp_register_shutdown_handler(&wifi_shutdown));

  mqtt5_app_start(&s_cfg);

  // Start timeout checker task (1s interval)
  xTaskCreate(timeout_task, "cmd_timeout", 2048, NULL, 5, NULL);

  // Start background button monitor (5s hold → re-provision via restart)
  provision_button_task(on_button_provision);
}
