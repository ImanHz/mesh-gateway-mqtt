#ifndef INCLUDE_PROVISION_H_
#define INCLUDE_PROVISION_H_

#include "esp_err.h"
#include <stdbool.h>

// Start a background task that monitors GPIO4 for a 5-second long press.
// On long press: calls on_press_cb(), then waits for provision completion.
void provision_button_task(void (*on_press_cb)(void));

// Enter SoftAP provisioning mode. Blocks until config is received or timeout.
// Returns ESP_OK if config was received (device will restart).
// Returns ESP_ERR_TIMEOUT if user abandoned (caller should resume services).
esp_err_t provision_start(void);

#endif // INCLUDE_PROVISION_H_
