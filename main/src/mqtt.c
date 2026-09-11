#include "mqtt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"
#include <time.h>

static esp_mqtt_client_handle_t mqtt_client = NULL;
// Written by SNTP sync callback, read by mqtt_callback — different tasks, hence volatile
static volatile bool s_time_synced = false;

/* extern const uint8_t cert_pem_start[] asm("_binary_cert_pem_start"); */
/* extern const uint8_t cert_pem_end[] asm("_binary_cert_pem_end"); */

/* Set connection properties and user properties */
static esp_mqtt5_user_property_item_t user_property_arr[] = {
    {"board", "esp32"}, {"u", "user"}, {"p", "password"}};
#define USE_PROPERTY_ARR_SIZE                                                  \
  sizeof(user_property_arr) / sizeof(esp_mqtt5_user_property_item_t)
static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
    .response_topic = "/topic/test/response",
    .correlation_data = "123456",
    .correlation_data_len = 6,
};

static esp_mqtt5_disconnect_property_config_t disconnect_property = {
    .session_expiry_interval = 60,
    .disconnect_reason = 0,
};

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
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;

  ESP_LOGD(MQTT_TAG, "free heap size is %" PRIu32 ", minimum %" PRIu32,
           esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
    mqtt_client = event->client;

    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
    mqtt_client = NULL;
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    esp_mqtt5_client_set_publish_property(client, &publish_property);
    msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
    ESP_LOGI(MQTT_TAG, "sent publish successful, msg_id=%d", msg_id);
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    esp_mqtt5_client_set_user_property(&disconnect_property.user_property,
                                       user_property_arr,
                                       USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_disconnect_property(client, &disconnect_property);
    esp_mqtt5_client_delete_user_property(disconnect_property.user_property);
    disconnect_property.user_property = NULL;
    esp_mqtt_client_disconnect(client);
    break;
  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
    ESP_LOGI(MQTT_TAG, "payload_format_indicator is %d",
             event->property->payload_format_indicator);
    ESP_LOGI(MQTT_TAG, "response_topic is %.*s",
             event->property->response_topic_len,
             event->property->response_topic);
    ESP_LOGI(MQTT_TAG, "correlation_data is %.*s",
             event->property->correlation_data_len,
             event->property->correlation_data);
    ESP_LOGI(MQTT_TAG, "content_type is %.*s",
             event->property->content_type_len, event->property->content_type);
    ESP_LOGI(MQTT_TAG, "TOPIC=%.*s", event->topic_len, event->topic);
    ESP_LOGI(MQTT_TAG, "DATA=%.*s", event->data_len, event->data);
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
void mqtt5_app_start(void) {
  esp_mqtt5_connection_property_config_t connect_property = {
      .session_expiry_interval = 10,
      .maximum_packet_size = 1024,
      .receive_maximum = 65535,
      .topic_alias_maximum = 2,
      .request_resp_info = true,
      .request_problem_info = true,
      .will_delay_interval = 10,
      .payload_format_indicator = true,
      .message_expiry_interval = 10,
      .response_topic = "/test/response",
      .correlation_data = "123456",
      .correlation_data_len = 6,

  };

  esp_mqtt_client_config_t mqtt5_cfg = {
      .broker.address.uri = CONFIG_BROKER_URL,
      /* .broker.verification.certificate = (const char *)cert_pem_start, */
      /* .broker.verification.skip_cert_common_name_check = true, */
      /* .broker.verification.use_global_ca_store = false, */
      /* .credentials.client_id = "pisys1", */
      .session.protocol_ver = MQTT_PROTOCOL_V_5,
      .network.disable_auto_reconnect = false,
      /* .credentials.username = "123", */
      /* .credentials.authentication.password = "456", */
      .session.last_will.topic = "/topic/will",
      .session.last_will.msg = "i will leave",
      .session.last_will.msg_len = 12,
      .session.last_will.qos = 1,
      .session.last_will.retain = true,
  };

  esp_sntp_config_t sntp_config = {.server_from_dhcp = true,
                                   .smooth_sync = true,
                                   .start = true,
                                   .wait_for_sync = false,
                                   .sync_cb = sntp_time_sync_cb};
  esp_err_t err = esp_netif_sntp_init(&sntp_config);
  if (err != ESP_OK) {

    ESP_LOGE("sntp", "error init. sntp, %s", esp_err_to_name(err));
  }
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt5_cfg);

  esp_mqtt5_client_set_user_property(&connect_property.user_property,
                                     user_property_arr, USE_PROPERTY_ARR_SIZE);
  esp_mqtt5_client_set_user_property(&connect_property.will_user_property,
                                     user_property_arr, USE_PROPERTY_ARR_SIZE);
  esp_mqtt5_client_set_connect_property(client, &connect_property);

  /* If you call esp_mqtt5_client_set_user_property to set user properties, DO
   * NOT forget to delete them. esp_mqtt5_client_set_connect_property will
   * malloc buffer to store the user_property and you can delete it after
   */
  esp_mqtt5_client_delete_user_property(connect_property.user_property);
  esp_mqtt5_client_delete_user_property(connect_property.will_user_property);

  /* The last argument may be used to pass data to the event handler, in this
   * example mqtt_event_handler */
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler,
                                 NULL);
  esp_mqtt_client_start(client);
}

void mqtt_callback(const uint8_t *msg, int len) {
  if (!mqtt_client) {
    ESP_LOGE("MQTT callback", "empty client");
    return;
  }

  if (!s_time_synced) {
    ESP_LOGW(MQTT_TAG, "NTP not synced yet, skipping publish");
    return;
  }

  time_t now = 0;
  struct tm timeinfo = {0};
  time(&now);
  localtime_r(&now, &timeinfo);
  ESP_LOGI("TIME", "%d %d %d %d %d", timeinfo.tm_year + 1900,
           timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
           timeinfo.tm_min);
  esp_mqtt5_client_set_user_property(&publish_property.user_property,
                                     user_property_arr, USE_PROPERTY_ARR_SIZE);
  esp_mqtt5_client_set_publish_property(mqtt_client, &publish_property);
  int msg_id = esp_mqtt_client_publish(mqtt_client, "/topic/qos1",
                                       (const char *)msg, len, 1, 1);

  esp_mqtt5_client_delete_user_property(publish_property.user_property);
  publish_property.user_property = NULL;
  ESP_LOGI(MQTT_TAG, "sent publish successful, msg_id=%d", msg_id);
}
