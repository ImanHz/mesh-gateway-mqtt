#include "../include/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_log_buffer.h"
#include "freertos/idf_additions.h"
#include "string.h"
const uint16_t c_uart_rx_timeout = UART_RX_TIMEOUT;

static void uart_rx_task(void *arg) {
  uart_task_params_t *params = (uart_task_params_t *)arg;

  uint8_t *buf = (uint8_t *)malloc(RX_BUF_SIZE + 1);
  if (!buf) {
    ESP_LOGE(("RX_TASK"), "Failed to allocate memory");
    vTaskDelete(NULL);
  }

  while (1) {
    int rxBytes = uart_read_bytes(UART_NUM_1, buf, RX_BUF_SIZE,
                                  (c_uart_rx_timeout) / portTICK_PERIOD_MS);
    if (rxBytes > 0) {
      buf[rxBytes] = '\0';
      ESP_LOGI(("RX_TASK"), "Read %d bytes", rxBytes);
      ESP_LOG_BUFFER_HEX("RX", buf, rxBytes);
      uart_msg_t msg = {};
      msg.len = rxBytes;
      /* memcpy(msg.data, buf, rxBytes + 1); */
      msg.data = buf;

      // Send to queue
      if (xQueueSend(params->queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(("RX_TASK"), "Queue full, message dropped");
      }
    }
  }

  free(buf);
  vTaskDelete(NULL);
}

static void uart_callback_task(void *arg) {
  uart_task_params_t *params = (uart_task_params_t *)arg;
  uart_msg_t msg = {};

  while (1) {
    if (xQueueReceive(params->queue, &msg, portMAX_DELAY)) {
      if (params->callback) {
        params->callback(msg.data, msg.len);
      }
    }
  }
}
void uart_init(uart_callback_t callback) {
  const uart_config_t uart_config = {
      .baud_rate = UART_BAUDRATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  // We won't use a buffer for sending data.
  esp_err_t uart_error =
      uart_driver_install(UART_NUM, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
  if (uart_error != ESP_OK) {
    ESP_LOGE(("INIT"), "uart error: %d", uart_error);
  }
  uart_param_config(UART_NUM, &uart_config);
  uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);
  // Allocate parameters
  uart_task_params_t *params = malloc(sizeof(uart_task_params_t));
  params->queue = xQueueCreate(10, sizeof(uart_msg_t)); // queue depth = 10
  params->callback = callback;

  // Start tasks
  xTaskCreate(uart_rx_task, "uart_rx_task", RX_TASK_SIZE, params,
              configMAX_PRIORITIES - 1, NULL);

  xTaskCreate(uart_callback_task, "uart_callback_task", RX_TASK_SIZE, params,
              configMAX_PRIORITIES - 2, NULL);
  ESP_LOGI(("INIT"), "uart init successful");
}

int uart_send_data(const char *data, uint8_t len) {
  const int txBytes = uart_write_bytes(UART_NUM_1, data, len);
  ESP_LOGI(("SEND"), "Wrote %d bytes", txBytes);
  return txBytes;
}
