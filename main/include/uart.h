#ifndef PDN_UART_H_
#define PDN_UART_H_

#include "driver/uart.h"
// tag used for mcu connection
// #define UART_TAG(x) GREEN_BG("UART_" x)

// maximum uart rx buffer size
static const int RX_BUF_SIZE = 1024;

// uart number. NUM_0 is used for debugging
#define UART_NUM UART_NUM_1
#define UART_BAUDRATE 115200
// mcu uart connection pinout

#define TXD_PIN 18
#define RXD_PIN 19

// FreeRTOS memory size
#define RX_TASK_SIZE 3072
#define TX_TASK_SIZE 3072

typedef struct {
  int len;
  uint8_t *data;
} uart_msg_t;
// UART rx timeout. The message is assumed as received whether after this
// timeout or RX_BUF_SIZE bytes received
#define UART_RX_TIMEOUT 100 // in milliseconds

// callback for RX
typedef void (*uart_callback_t)(const uint8_t *data, int len);

typedef struct {
  QueueHandle_t queue;
  uart_callback_t callback;
} uart_task_params_t;

// initializes the UART peripheral
void uart_init(uart_callback_t);

// helper function to send data
int uart_send_data(const char *data, uint8_t len);

#endif
