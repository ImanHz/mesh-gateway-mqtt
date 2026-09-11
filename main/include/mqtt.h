#ifndef INCLUDE_MQTT_H_
#define INCLUDE_MQTT_H_

#include "config.h"
#include "stdint.h"

#define MQTT_TAG "MQTT"

void mqtt5_app_start(const app_config_t *cfg);
void mqtt_callback(const uint8_t *msg, int len);

#endif // INCLUDE_MQTT_H_
