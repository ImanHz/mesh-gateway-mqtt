#ifndef INCLUDE_CONFIG_H_
#define INCLUDE_CONFIG_H_

#include "esp_err.h"
#include <stdbool.h>

#define NVS_NAMESPACE "prov"
#define NVS_KEY_PROVISIONED "provisioned"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_BROKER_URL "broker_url"
#define NVS_KEY_PUB_TOPIC "pub_topic"
#define NVS_KEY_WILL_TOPIC "will_topic"

typedef struct {
  char ssid[33];
  char password[65];
  char broker_url[128];
  char pub_topic[64];
  char will_topic[64];
} app_config_t;

// Read config from NVS. Returns true if provisioned, false if using defaults.
bool config_read(app_config_t *cfg);

// Write config to NVS and set provisioned flag.
esp_err_t config_write(const app_config_t *cfg);

// Clear provisioning flag (triggers provisioning on next boot).
esp_err_t config_clear_provisioned(void);

#endif // INCLUDE_CONFIG_H_
