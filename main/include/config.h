#ifndef INCLUDE_CONFIG_H_
#define INCLUDE_CONFIG_H_

#include "esp_err.h"
#include <stdbool.h>

#define NVS_NAMESPACE "prov"
#define NVS_KEY_PROVISIONED "provisioned"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_BROKER_ADDR "broker_addr"
#define NVS_KEY_REPROV "reprov"

typedef struct {
  char ssid[33];
  char password[65];
  char broker_addr[64];   // IP or hostname (user-provided)
  char broker_url[128];   // derived: mqtt://<addr>:1883
  char pub_topic[64];     // derived: site/<mac>/telemetry
  char sub_topic[64];     // derived: site/<mac>/cmd
  char will_topic[64];    // derived: site/<mac>/status
} app_config_t;

// Read config from NVS. Returns true if provisioned, false if using defaults.
bool config_read(app_config_t *cfg);

// Write config to NVS and set provisioned flag.
esp_err_t config_write(const app_config_t *cfg);

// Clear provisioning flag (triggers provisioning on next boot).
esp_err_t config_clear_provisioned(void);

// Set/clear re-provisioning flag for runtime button-triggered provisioning.
// Device restarts into provisioning mode on next boot.
esp_err_t config_set_reprov_flag(void);
bool config_get_reprov_flag(void);
esp_err_t config_clear_reprov_flag(void);

// Build broker_url and topics from broker_addr + device MAC.
// Call after config_read() to populate the derived fields.
void config_build_derived(app_config_t *cfg);

#endif // INCLUDE_CONFIG_H_
