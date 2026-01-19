#ifndef INCLUDE_INCLUDE_MQTT_H_
#define INCLUDE_INCLUDE_MQTT_H_
#include "stdint.h"

#define CONFIG_BROKER_URL "mqtt://192.168.1.9:1883"
#define MQTT_TAG "MQTT"

void mqtt5_app_start(void);
void mqtt_callback(const uint8_t *msg, int len);
#endif // INCLUDE_INCLUDE_MQTT_H_
