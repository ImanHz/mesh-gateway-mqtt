#include "mqtt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "uart.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool s_time_synced = false;
static app_config_t s_mqtt_cfg;

// Buffered telemetry for early UART packets (before MQTT connects)
static uint8_t s_telemetry_buf[CMD_PKT_LEN] = {0};
static size_t s_telemetry_len = 0;

extern const uint8_t ca_pem_start[] asm("_binary_ca_pem_start");
extern const uint8_t cert_pem_start[] asm("_binary_cert_pem_start");
extern const uint8_t key_pem_start[] asm("_binary_key_pem_start");

static esp_mqtt5_user_property_item_t user_property_arr[] = {
    {"board", "esp32"}, {"u", "user"}, {"p", "password"}};
#define USE_PROPERTY_ARR_SIZE                                                  \
  sizeof(user_property_arr) / sizeof(esp_mqtt5_user_property_item_t)
static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = MQTT_PUBLISH_MSG_EXPIRY_SEC,
    .topic_alias = 0,
};

/* ── Telemetry parsing (0xFF packets, unchanged) ──────────── */

/*
 * UART packet format from mesh-client (13 bytes):
 *   [0xFF] [3B MPID] [2B distance_mm LE] [2B src_addr LE] [1B rssi+128] [0xFE]
 */
#define PKT_LEN 13
#define HDR 0xff
#define TAIL 0xfe
#define OFF_DATA 1   // sensor data starts here
#define OFF_ADDR 9   // src_addr: bytes 9-10 (1 + DATA_LEN)
#define OFF_RSSI 11  // rssi+128: byte 11 (1 + DATA_LEN + 2)

static bool parse_payload(const uint8_t *data, size_t len, char *json,
                          size_t json_max) {
  if (len != PKT_LEN || data[0] != HDR || data[PKT_LEN - 1] != TAIL) {
    ESP_LOGW(MQTT_TAG, "Invalid packet: len=%d, hdr=0x%02x, tail=0x%02x",
             (int)len, data[0], data[PKT_LEN - 1]);
    return false;
  }

  // Distance: 2 bytes LE at offset 4-5 (after 3-byte MPID)
  uint16_t distance_mm = (uint16_t)data[4] | ((uint16_t)data[5] << 8);

  // Source address: 2 bytes LE at offset 9-10
  uint16_t src_addr = (uint16_t)data[OFF_ADDR] | ((uint16_t)data[OFF_ADDR + 1] << 8);

  // RSSI: byte at offset 11, stored as (128 + rssi)
  int8_t rssi = (int8_t)data[OFF_RSSI] - 128;

  // Use system time (NTP-synced) for timestamp
  time_t now;
  time(&now);
  struct tm ti = {0};
  gmtime_r(&now, &ti);

  snprintf(json, json_max,
           "{\"timestamp\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\","
           "\"distance_mm\":%d,"
           "\"address\":\"0x%04x\","
           "\"rssi\":%d}",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour,
           ti.tm_min, ti.tm_sec, distance_mm, src_addr, rssi);
  return true;
}

/* ── Pending command list (for timeout tracking) ───────────── */

typedef struct {
  uint8_t cmd_id;
  uint8_t sensor_id;
  uint8_t cmd;
  uint16_t target_addr;
  int64_t sent_us;      // esp_timer_get_time() when sent
  bool in_use;
} pending_cmd_t;

static pending_cmd_t s_pending[CMD_PENDING_MAX] = {0};

static int8_t pending_alloc(uint8_t cmd_id, uint8_t sensor_id, uint8_t cmd,
                            uint16_t target_addr) {
  // Find free slot
  for (int i = 0; i < CMD_PENDING_MAX; i++) {
    if (!s_pending[i].in_use) {
      s_pending[i] = (pending_cmd_t){
          .cmd_id = cmd_id,
          .sensor_id = sensor_id,
          .cmd = cmd,
          .target_addr = target_addr,
          .sent_us = esp_timer_get_time(),
          .in_use = true,
      };
      return i;
    }
  }
  // All full — evict oldest
  int oldest = 0;
  int64_t oldest_us = s_pending[0].sent_us;
  for (int i = 1; i < CMD_PENDING_MAX; i++) {
    if (s_pending[i].sent_us < oldest_us) {
      oldest_us = s_pending[i].sent_us;
      oldest = i;
    }
  }
  ESP_LOGW(MQTT_TAG, "pending list full, evicting cmd_id=%d",
           s_pending[oldest].cmd_id);
  s_pending[oldest] = (pending_cmd_t){
      .cmd_id = cmd_id,
      .sensor_id = sensor_id,
      .cmd = cmd,
      .target_addr = target_addr,
      .sent_us = esp_timer_get_time(),
      .in_use = true,
  };
  return oldest;
}

static void pending_remove_by_id(uint8_t cmd_id) {
  for (int i = 0; i < CMD_PENDING_MAX; i++) {
    if (s_pending[i].in_use && s_pending[i].cmd_id == cmd_id) {
      s_pending[i].in_use = false;
      return;
    }
  }
}

/* ── MQTT publish helper ───────────────────────────────────── */

static void mqtt_publish(const char *topic, const char *json) {
  if (!mqtt_client) {
    ESP_LOGE(MQTT_TAG, "mqtt_publish: no client");
    return;
  }
  esp_mqtt5_client_set_user_property(&publish_property.user_property,
                                     user_property_arr, USE_PROPERTY_ARR_SIZE);
  esp_mqtt5_client_set_publish_property(mqtt_client, &publish_property);
  int msg_id = esp_mqtt_client_publish(mqtt_client, topic, json, 0, 1, 1);
  esp_mqtt5_client_delete_user_property(publish_property.user_property);
  publish_property.user_property = NULL;
  ESP_LOGD(MQTT_TAG, "published to %s, msg_id=%d", topic, msg_id);
}

/* ── Command parsing (MQTT JSON → UART 0xFD) ──────────────── */

// Extract mesh address from MQTT topic: "site/<addr>/cmd" → addr
static bool extract_mesh_addr(const char *topic, int topic_len,
                              uint16_t *addr) {
  // Find "site/" prefix
  if (topic_len < 12 || strncmp(topic, "site/", 5) != 0)
    return false;

  // Find second '/' after "site/"
  const char *p = topic + 5;
  const char *end = topic + topic_len;
  const char *slash = NULL;
  while (p < end) {
    if (*p == '/') {
      slash = p;
      break;
    }
    p++;
  }
  if (!slash || (slash - (topic + 5)) != 4)
    return false; // expect exactly 4 hex chars

  char addr_str[5] = {};
  memcpy(addr_str, topic + 5, 4);
  *addr = (uint16_t)strtol(addr_str, NULL, 16);
  return true;
}

static uint8_t s_cmd_seq = 0;

static void handle_command_topic(const char *topic, int topic_len,
                                 const char *data, int data_len) {
  // Parse JSON: {"sensor": N, "cmd": N, "value": N}
  // Simple manual parse — avoids cJSON dependency
  int sensor_id = -1, cmd = -1, value = 0;
  bool has_value = false;

  // Find "sensor": N
  const char *p = data;
  const char *end = data + data_len;
  while (p < end) {
    if (strncmp(p, "\"sensor\"", 8) == 0) {
      p += 8;
      while (p < end && (*p == ':' || *p == ' '))
        p++;
      sensor_id = strtol(p, NULL, 10);
      break;
    }
    p++;
  }

  // Find "cmd": N
  p = data;
  while (p < end) {
    if (strncmp(p, "\"cmd\"", 5) == 0) {
      p += 5;
      while (p < end && (*p == ':' || *p == ' '))
        p++;
      cmd = strtol(p, NULL, 10);
      break;
    }
    p++;
  }

  // Find "value": N
  p = data;
  while (p < end) {
    if (strncmp(p, "\"value\"", 7) == 0) {
      p += 7;
      while (p < end && (*p == ':' || *p == ' '))
        p++;
      value = strtol(p, NULL, 10);
      has_value = true;
      break;
    }
    p++;
  }

  if (sensor_id < 0 || cmd < 0) {
    ESP_LOGW(MQTT_TAG, "Invalid command JSON: sensor=%d cmd=%d", sensor_id,
             cmd);
    return;
  }

  // Extract target mesh address from topic
  uint16_t target_addr = 0;
  if (!extract_mesh_addr(topic, topic_len, &target_addr)) {
    ESP_LOGW(MQTT_TAG, "Cannot extract mesh addr from topic: %.*s",
             topic_len, topic);
    return;
  }

  // Build 0xFD command UART packet
  uint8_t pkt[CMD_PKT_LEN] = {};
  pkt[0] = CMD_HDR;
  pkt[CMD_OFF_CMD_ID] = ++s_cmd_seq;
  pkt[CMD_OFF_TARGET] = target_addr & 0xFF;
  pkt[CMD_OFF_TARGET + 1] = (target_addr >> 8) & 0xFF;
  pkt[CMD_OFF_SENSOR] = (uint8_t)sensor_id;
  pkt[CMD_OFF_CMD] = (uint8_t)cmd;
  int16_t val16 = has_value ? (int16_t)value : 0;
  pkt[CMD_OFF_VALUE] = val16 & 0xFF;
  pkt[CMD_OFF_VALUE + 1] = (val16 >> 8) & 0xFF;
  pkt[CMD_PKT_LEN - 1] = CMD_TAIL;

  // Track in pending list
  pending_alloc(s_cmd_seq, (uint8_t)sensor_id, (uint8_t)cmd, target_addr);

  // Send to mesh-client via UART
  ESP_LOGI(MQTT_TAG, "CMD → UART: id=%d target=0x%04x sensor=%d cmd=%d val=%d",
           s_cmd_seq, target_addr, sensor_id, cmd, val16);
  ESP_LOG_BUFFER_HEX(MQTT_TAG, pkt, CMD_PKT_LEN);
  uart_send_data((const char *)pkt, CMD_PKT_LEN);
}

/* ── Response handling (UART 0xFD → MQTT) ──────────────────── */

void mqtt_cmd_response_callback(const uint8_t *msg, size_t len) {
  if (len < CMD_PKT_LEN || msg[0] != CMD_HDR || msg[CMD_PKT_LEN - 1] != CMD_TAIL) {
    ESP_LOGW(MQTT_TAG, "Invalid response packet: len=%d", (int)len);
    return;
  }

  uint8_t cmd_id = msg[RSP_OFF_CMD_ID];
  uint8_t sensor_id = msg[RSP_OFF_SENSOR];
  uint8_t cmd = msg[RSP_OFF_CMD];
  uint8_t result = msg[RSP_OFF_RESULT];
  uint16_t src_addr = (uint16_t)msg[RSP_OFF_SOURCE] |
                      ((uint16_t)msg[RSP_OFF_SOURCE + 1] << 8);

  // Find matching pending command
  pending_cmd_t *pc = NULL;
  for (int i = 0; i < CMD_PENDING_MAX; i++) {
    if (s_pending[i].in_use && s_pending[i].cmd_id == cmd_id) {
      pc = &s_pending[i];
      break;
    }
  }

  // Build response topic: site/<source_addr_hex>/cmd/resp
  char resp_topic[48];
  snprintf(resp_topic, sizeof(resp_topic), "site/%04X/cmd/resp", src_addr);

  // Build JSON response
  char json[128];
  snprintf(json, sizeof(json),
           "{\"cmd_id\":%d,\"sensor\":%d,\"cmd\":%d,\"result\":%d,"
           "\"address\":\"0x%04x\"}",
           cmd_id, sensor_id, cmd, result, src_addr);

  ESP_LOGI(MQTT_TAG, "RESP ← UART: id=%d result=%d addr=0x%04x → %s", cmd_id,
           result, src_addr, resp_topic);
  mqtt_publish(resp_topic, json);

  // Remove from pending list
  pending_remove_by_id(cmd_id);
  if (pc) {
    pc->in_use = false;
  }
}

/* ── Timeout checker (call from a periodic task or the main loop) ── */

void mqtt_cmd_check_timeouts(void) {
  int64_t now = esp_timer_get_time();

  for (int i = 0; i < CMD_PENDING_MAX; i++) {
    if (!s_pending[i].in_use)
      continue;

    if ((now - s_pending[i].sent_us) > (CMD_TIMEOUT_MS * 1000)) {
      // Timeout — publish error
      char resp_topic[48];
      snprintf(resp_topic, sizeof(resp_topic), "site/%04X/cmd/resp",
               s_pending[i].target_addr);

      char json[128];
      snprintf(json, sizeof(json),
               "{\"cmd_id\":%d,\"sensor\":%d,\"cmd\":%d,\"result\":-1,"
               "\"error\":\"timeout\"}",
               s_pending[i].cmd_id, s_pending[i].sensor_id, s_pending[i].cmd);

      ESP_LOGW(MQTT_TAG, "CMD TIMEOUT: id=%d, publishing error", s_pending[i].cmd_id);
      mqtt_publish(resp_topic, json);

      s_pending[i].in_use = false;
    }
  }
}

/* ── SNTP callback ─────────────────────────────────────────── */

static void sntp_time_sync_cb(struct timeval *tv) {
  s_time_synced = true;
  ESP_LOGI(MQTT_TAG, "SNTP time synchronized");
}

/* ── MQTT event handler ────────────────────────────────────── */

static void mqtt5_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
  ESP_LOGD(MQTT_TAG,
           "Event dispatched from event loop base=%s, event_id=%" PRIi32, base,
           event_id);
  esp_mqtt_event_handle_t event = event_data;

  ESP_LOGD(MQTT_TAG, "free heap size is %" PRIu32 ", minimum %" PRIu32,
           esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
    mqtt_client = event->client;
    // Subscribe to command topic (wildcard: site/+/cmd)
    if (s_mqtt_cfg.sub_topic[0] != '\0') {
      int msg_id = esp_mqtt_client_subscribe(mqtt_client,
                                             s_mqtt_cfg.sub_topic, 1);
      ESP_LOGI(MQTT_TAG, "Subscribed to %s, msg_id=%d",
               s_mqtt_cfg.sub_topic, msg_id);
    }
    // Publish any telemetry buffered before MQTT was ready
    if (s_telemetry_len > 0) {
      ESP_LOGI(MQTT_TAG, "Publishing buffered telemetry (%d bytes)",
               (int)s_telemetry_len);
      char json[160];
      if (parse_payload(s_telemetry_buf, s_telemetry_len, json, sizeof(json))) {
        mqtt_publish(s_mqtt_cfg.pub_topic, json);
      }
      s_telemetry_len = 0;
    }
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
    mqtt_client = NULL;
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(MQTT_TAG, "TOPIC=%.*s", event->topic_len, event->topic);
    ESP_LOGI(MQTT_TAG, "DATA=%.*s", event->data_len, event->data);

    // Check if this is a command on site/+/cmd
    if (event->topic_len > 5 && strncmp(event->topic, "site/", 5) == 0) {
      // Check if topic ends with "/cmd"
      if (event->topic_len > 4 &&
          strncmp(event->topic + event->topic_len - 4, "/cmd", 4) == 0) {
        handle_command_topic(event->topic, event->topic_len,
                             event->data, event->data_len);
      }
    }
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");
    ESP_LOGI(MQTT_TAG, "MQTT5 return code is %d",
             event->error_handle->connect_return_code);
    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
      ESP_LOGI(MQTT_TAG, "Last errno string (%s)",
               strerror(event->error_handle->esp_transport_sock_errno));
    }
    break;
  default:
    ESP_LOGI(MQTT_TAG, "Other event id:%d", event->event_id);
    break;
  }
}

/* ── Public API ────────────────────────────────────────────── */

void mqtt5_app_start(const app_config_t *cfg) {
  memcpy(&s_mqtt_cfg, cfg, sizeof(s_mqtt_cfg));
  esp_mqtt5_connection_property_config_t connect_property = {
      .session_expiry_interval = MQTT_SESSION_EXPIRY_SEC,
      .maximum_packet_size = MQTT_MAX_PACKET_SIZE,
      .receive_maximum = MQTT_RECEIVE_MAXIMUM,
      .topic_alias_maximum = MQTT_TOPIC_ALIAS_MAXIMUM,
      .will_delay_interval = MQTT_WILL_DELAY_SEC,
      .payload_format_indicator = true,
      .message_expiry_interval = MQTT_MSG_EXPIRY_SEC,
  };

  if (strncmp(cfg->broker_url, "mqtts://", 8) != 0 &&
      strncmp(cfg->broker_url, "wss://", 6) != 0) {
    ESP_LOGW(MQTT_TAG, "Broker URL is not TLS: %s", cfg->broker_url);
  }

  esp_mqtt_client_config_t mqtt5_cfg = {
      .broker.address.uri = cfg->broker_url,
      .session.protocol_ver = MQTT_PROTOCOL_V_5,
      .network.disable_auto_reconnect = false,
      .session.last_will.topic = cfg->will_topic,
      .session.last_will.msg = "offline",
      .session.last_will.msg_len = 7,
      .session.last_will.qos = 1,
      .session.last_will.retain = true,
  };

  bool use_tls = (strncmp(cfg->broker_url, "mqtts://", 8) == 0 ||
                  strncmp(cfg->broker_url, "wss://", 6) == 0);
  if (use_tls) {
    mqtt5_cfg.broker.verification.certificate = (const char *)ca_pem_start;
    mqtt5_cfg.credentials.authentication.certificate =
        (const char *)cert_pem_start;
    mqtt5_cfg.credentials.authentication.key = (const char *)key_pem_start;
  }

  esp_sntp_config_t sntp_config = {
      .server_from_dhcp = false,
      .smooth_sync = true,
      .start = true,
      .wait_for_sync = false,
      .sync_cb = sntp_time_sync_cb,
      .num_of_servers = 1,
      .servers = {"pool.ntp.org"},
  };
  esp_err_t err = esp_netif_sntp_init(&sntp_config);
  if (err != ESP_OK) {
    ESP_LOGE(MQTT_TAG, "SNTP init error: %s", esp_err_to_name(err));
  }

  if (use_tls && !s_time_synced) {
    ESP_LOGI(MQTT_TAG, "Waiting for NTP sync...");
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(MQTT_NTP_SYNC_TIMEOUT_MS));
    if (!s_time_synced) {
      ESP_LOGW(MQTT_TAG, "NTP sync timeout, MQTT will retry after sync");
    }
  }

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt5_cfg);

  esp_mqtt5_client_set_user_property(&connect_property.user_property,
                                     user_property_arr, USE_PROPERTY_ARR_SIZE);
  esp_mqtt5_client_set_user_property(&connect_property.will_user_property,
                                     user_property_arr, USE_PROPERTY_ARR_SIZE);
  esp_mqtt5_client_set_connect_property(client, &connect_property);

  esp_mqtt5_client_delete_user_property(connect_property.user_property);
  esp_mqtt5_client_delete_user_property(connect_property.will_user_property);

  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler,
                                 NULL);
  esp_mqtt_client_start(client);
}

void mqtt_telemetry_callback(const uint8_t *msg, size_t len) {
  // Always buffer latest telemetry (for publish on MQTT connect)
  size_t copy_len = len < CMD_PKT_LEN ? len : CMD_PKT_LEN;
  memcpy(s_telemetry_buf, msg, copy_len);
  s_telemetry_len = copy_len;

  if (!mqtt_client) {
    ESP_LOGD(MQTT_TAG, "telemetry: no client yet, buffered %d bytes",
             (int)copy_len);
    return;
  }

  char json[160];
  if (!parse_payload(msg, len, json, sizeof(json))) {
    return;
  }

  ESP_LOGI(MQTT_TAG, "Publishing: %s", json);
  mqtt_publish(s_mqtt_cfg.pub_topic, json);
}

void mqtt5_stop(void) {
  if (mqtt_client) {
    ESP_LOGI(MQTT_TAG, "Stopping MQTT client");
    esp_mqtt_client_stop(mqtt_client);
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = NULL;
  }
}
