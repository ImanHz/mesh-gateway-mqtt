#ifndef INCLUDE_PROVISION_H_
#define INCLUDE_PROVISION_H_

#include "esp_err.h"
#include <stdbool.h>

// Check if reset button (GPIO4) is held low at boot.
bool provision_button_held(void);

// Enter SoftAP provisioning mode. Blocks until config is received or timeout.
// Returns ESP_OK if config was received and stored.
esp_err_t provision_start(void);

#endif // INCLUDE_PROVISION_H_
