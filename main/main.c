
#include "main.h"
#include "esp_log.h"
#include "esp_log_buffer.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "gpio.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "uart.h"
#include "wifi.h"

void app_main(void) {
  uart_init(mqtt_callback);
  /* init_gpio(); */
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes",
           esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  if (wifi_connect() != ESP_OK) {
    ESP_LOGE("MAIN", "WIFI CONNECT ERROR");
    return;
  }
  ESP_ERROR_CHECK(esp_register_shutdown_handler(&wifi_shutdown));

  mqtt5_app_start();
}
