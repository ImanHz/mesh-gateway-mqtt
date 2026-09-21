#ifndef INCLUDE_MQTT_H_
#define INCLUDE_MQTT_H_

#include "config.h"
#include <stddef.h>
#include <stdint.h>

#define MQTT_TAG "MQTT"

// MQTTv5 connect property defaults
#define MQTT_SESSION_EXPIRY_SEC 10
#define MQTT_MAX_PACKET_SIZE 1024
#define MQTT_RECEIVE_MAXIMUM 65535
#define MQTT_TOPIC_ALIAS_MAXIMUM 2
#define MQTT_WILL_DELAY_SEC 10
#define MQTT_MSG_EXPIRY_SEC 10
#define MQTT_PUBLISH_MSG_EXPIRY_SEC 1000
#define MQTT_NTP_SYNC_TIMEOUT_MS 30000

// ── Command packet protocol (must match mesh-client-server) ──
#define CMD_HDR       0xFD
#define CMD_TAIL      0xFE
#define CMD_PKT_LEN   13   // total UART command packet length
#define CMD_DATA_LEN  8    // data region size

// Command packet layout (13 bytes):
// [0xFD] [cmd_id 1B] [target_addr 2B LE] [sensor_id 1B] [cmd 1B] [value 2B LE] [0xFE]
#define CMD_OFF_CMD_ID     1
#define CMD_OFF_TARGET     2   // 2 bytes
#define CMD_OFF_SENSOR     4
#define CMD_OFF_CMD        5
#define CMD_OFF_VALUE      6   // 2 bytes

// Response packet layout (13 bytes):
// [0xFD] [cmd_id 1B] [sensor_id 1B] [cmd 1B] [result 1B] [source_addr 2B LE] [0xFE]
#define RSP_OFF_CMD_ID     1
#define RSP_OFF_SENSOR     2
#define RSP_OFF_CMD        3
#define RSP_OFF_RESULT     4
#define RSP_OFF_SOURCE     5   // 2 bytes

// Pending command timeout
#define CMD_TIMEOUT_MS     5000
#define CMD_PENDING_MAX    4

void mqtt5_app_start(const app_config_t *cfg);
void mqtt5_stop(void);

// UART callback for 0xFF telemetry packets
void mqtt_telemetry_callback(const uint8_t *msg, size_t len);

// UART callback for 0xFD command response packets
void mqtt_cmd_response_callback(const uint8_t *msg, size_t len);

// Check for timed-out pending commands (call periodically)
void mqtt_cmd_check_timeouts(void);

#endif // INCLUDE_MQTT_H_
