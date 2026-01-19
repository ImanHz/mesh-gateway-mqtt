
#include "gpio.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static QueueHandle_t gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void *arg) {
  uint32_t gpio_num = (uint32_t)arg;
  xQueueGenericSendFromISR((gpio_evt_queue), (&gpio_num), (((void *)0)),
                           ((BaseType_t)0));
}

static void gpio_task(void *arg) {
  uint32_t io_num;
  for (;;) {
    if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
      ESP_LOGI("GPIO", "GPIO[%" PRIu32 "] intr, val: %d\n", io_num,
               gpio_get_level(io_num));
    }
  }
}

void init_gpio(void) {
  // zero-initialize the config structure.
  gpio_config_t io_conf = {};

  // interrupt of rising edge
  io_conf.intr_type = GPIO_INTR_NEGEDGE;
  // bit mask of the pins, use GPIO4/5 here
  io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
  // set as input mode
  io_conf.mode = GPIO_MODE_INPUT;
  // enable pull-up mode
  io_conf.pull_up_en = 1;
  gpio_config(&io_conf);

  // create a queue to handle gpio event from isr
  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
  // start gpio task
  xTaskCreate(gpio_task, "gpio_task", 2048, NULL, 10, NULL);

  // install gpio isr service
  gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
  // hook isr handler for specific gpio pin
  gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler,
                       (void *)GPIO_INPUT_IO_0);
}
