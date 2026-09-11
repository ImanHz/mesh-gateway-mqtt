#ifndef INCLUDE_MQTT_H_
#define INCLUDE_MQTT_H_

#include "config.h"
#include <stddef.h>

#define MQTT_TAG "MQTT"

// MQTTv5 connect property defaults
#define MQTT_SESSION_EXPIRY_SEC 10
#define MQTT_MAX_PACKET_SIZE 1024
#define MQTT_RECEIVE_MAXIMUM 65535
#define MQTT_TOPIC_ALIAS_MAXIMUM 2
#define MQTT_WILL_DELAY_SEC 10
#define MQTT_MSG_EXPIRY_SEC 10
#define MQTT_PUBLISH_MSG_EXPIRY_SEC 1000

void mqtt5_app_start(const app_config_t *cfg);
void mqtt_callback(const uint8_t *msg, size_t len);

#endif // INCLUDE_MQTT_H_
