#include "mqtt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool s_time_synced = false;
static app_config_t s_mqtt_cfg;

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

/*
 * UART packet format from mesh-client (13 bytes):
 *   [0xFF] [3B MPID] [2B distance_mm LE] [2B src_addr LE] [1B rssi+128] [0xFE]
 *
 * The 5-byte mesh payload is: MPID(3) + distance(2 LE)
 * MPID encodes the BLE Mesh Sensor Data Format B header.
 * We only need the 2-byte distance value at offset 4-5.
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

  // Source address: 2 bytes LE at offset 6-7
  uint16_t src_addr = (uint16_t)data[OFF_ADDR] | ((uint16_t)data[OFF_ADDR + 1] << 8);

  // RSSI: byte at offset 8, stored as (128 + rssi)
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

static void sntp_time_sync_cb(struct timeval *tv) {
  s_time_synced = true;
  ESP_LOGI(MQTT_TAG, "SNTP time synchronized");
}

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
    // Subscribe to command topic
    if (s_mqtt_cfg.sub_topic[0] != '\0') {
      int msg_id = esp_mqtt_client_subscribe(mqtt_client,
                                             s_mqtt_cfg.sub_topic, 1);
      ESP_LOGI(MQTT_TAG, "Subscribed to %s, msg_id=%d",
               s_mqtt_cfg.sub_topic, msg_id);
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
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
    ESP_LOGI(MQTT_TAG, "TOPIC=%.*s", event->topic_len, event->topic);
    ESP_LOGI(MQTT_TAG, "DATA=%.*s", event->data_len, event->data);

    // Check if this is a command on our sub_topic
    if (s_mqtt_cfg.sub_topic[0] != '\0' &&
        event->topic_len == strlen(s_mqtt_cfg.sub_topic) &&
        strncmp(event->topic, s_mqtt_cfg.sub_topic, event->topic_len) == 0) {
      ESP_LOGI(MQTT_TAG, "Command received on %s", s_mqtt_cfg.sub_topic);
      // TODO: parse JSON command and forward to UART
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

void mqtt_callback(const uint8_t *msg, size_t len) {
  if (!mqtt_client) {
    ESP_LOGE(MQTT_TAG, "empty client");
    return;
  }

  char json[160];
  if (!parse_payload(msg, len, json, sizeof(json))) {
    return;
  }

  ESP_LOGI(MQTT_TAG, "Publishing: %s", json);

  esp_mqtt5_client_set_user_property(&publish_property.user_property,
                                     user_property_arr, USE_PROPERTY_ARR_SIZE);
  esp_mqtt5_client_set_publish_property(mqtt_client, &publish_property);
  int msg_id = esp_mqtt_client_publish(mqtt_client, s_mqtt_cfg.pub_topic, json,
                                       0, 1, 1);

  esp_mqtt5_client_delete_user_property(publish_property.user_property);
  publish_property.user_property = NULL;
  ESP_LOGI(MQTT_TAG, "sent publish successful, msg_id=%d", msg_id);
}

void mqtt5_stop(void) {
  if (mqtt_client) {
    ESP_LOGI(MQTT_TAG, "Stopping MQTT client");
    esp_mqtt_client_stop(mqtt_client);
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = NULL;
  }
}
