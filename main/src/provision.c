#include "provision.h"
#include "config.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "PROV";

#define PROV_AP_SSID_PREFIX "PROV_"
#define PROV_TIMEOUT_US (5 * 60 * 1000000ULL) // 5 minutes
#define RESET_GPIO 4
#define MAX_POST_BUF 1024
#define BUTTON_HOLD_MS 5000 // 5 seconds long press

// --- Button task (5s long press detection) ---

static void (*s_on_provision_cb)(void) = NULL;

static void button_task(void *arg) {
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << RESET_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);

  while (1) {
    // Wait for button press (HIGH)
    if (gpio_get_level(RESET_GPIO) == 1) {
      int64_t press_start = esp_timer_get_time();
      ESP_LOGI(TAG, "Button pressed, detecting hold...");
      // Hold detection: sample every 100ms, require continuous HIGH
      while (gpio_get_level(RESET_GPIO) == 1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t held = (esp_timer_get_time() - press_start) / 1000;
        if (held >= BUTTON_HOLD_MS) {
          ESP_LOGW(TAG, "Button held %lldms — entering provisioning",
                   held);
          if (s_on_provision_cb)
            s_on_provision_cb();
          break;
        }
        // Debug: log every 2s
        if ((held % 2000) < 110)
          ESP_LOGI(TAG, "Holding... %lldms", held);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void provision_button_task(void (*on_press_cb)(void)) {
  s_on_provision_cb = on_press_cb;
  xTaskCreate(button_task, "prov_button", 3072, NULL, 5, NULL);
}

// --- SoftAP + HTTP provisioning ---

static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_server = NULL;
static app_config_t s_prov_cfg;

// Captive portal: Android checks /generate_204 → respond 204
static esp_err_t captive_android_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

// Captive portal: iOS checks /hotspot-detect.html → respond 200 with <HTML>
static esp_err_t captive_ios_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                              "<BODY>Success</BODY></HTML>",
                         HTTPD_RESP_USE_STRLEN);
}

// Bilingual provisioning form — 3 fields only (Persian/English)
static const char FORM_HTML_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Padena Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
    "background:#f5f5f5;padding:16px}"
    ".c{max-width:400px;margin:0 auto;background:#fff;border-radius:12px;"
    "padding:24px;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
    "h1{text-align:center;margin-bottom:4px;font-size:20px}"
    ".sub{text-align:center;color:#888;font-size:13px;margin-bottom:20px}"
    ".g{margin-bottom:14px}"
    ".g label{display:block;font-size:13px;font-weight:600;margin-bottom:4px}"
    ".g input{width:100%;padding:10px;border:1px solid #ddd;border-radius:8px;"
    "font-size:14px}"
    ".g input:focus{outline:none;border-color:#4a90d9}"
    "button{width:100%;padding:12px;background:#4a90d9;color:#fff;border:none;"
    "border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;margin-top:8px}"
    "button:active{background:#3a7bc8}"
    ".ap{text-align:center;color:#aaa;font-size:11px;margin-top:12px}"
    "</style></head><body><div class=\"c\">"
    "<h1>Padena</h1>"
    "<div class=\"sub\">\u062a\u0646\u0638\u06cc\u0645 \u062f\u0633\u062a\u06af\u0627\u0647 / Device Setup"
    "</div>"
    "<div class=\"sub\" style=\"font-size:11px;color:#aaa\">ID: ";

static const char FORM_HTML_MID[] =
    "</div>"
    "<form method=\"POST\" action=\"/prov\">"
    "<div class=\"g\">"
    "<label>WiFi SSID / \u0646\u0627\u0645 \u0634\u0628\u0647</label>"
    "<input name=\"ssid\" required placeholder=\"e.g. MyWiFi\">"
    "</div>"
    "<div class=\"g\">"
    "<label>Password / \u06af\u0630\u0631\u0648\u0647</label>"
    "<input name=\"password\" type=\"password\" required>"
    "</div>"
    "<div class=\"g\">"
    "<label>Broker / \u0627\u0631\u0633\u0627\u0644\u06af\u0631</label>"
    "<input name=\"broker_addr\" required placeholder=\"192.168.1.100\">"
    "</div>"
    "<button type=\"submit\">\u0630\u062e\u06cc\u0631\u0647 / Save</button>"
    "</form>"
    "<div class=\"ap\">AP: ";

static const char FORM_HTML_TAIL[] =
    "</div></body></html>";

// GET / → serve bilingual form
static esp_err_t form_get_handler(httpd_req_t *req) {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char ap_ssid[32];
  snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", PROV_AP_SSID_PREFIX,
           mac[4], mac[5]);

  char mac_id[13];
  snprintf(mac_id, sizeof(mac_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send_chunk(req, FORM_HTML_HEAD, strlen(FORM_HTML_HEAD));
  httpd_resp_send_chunk(req, mac_id, strlen(mac_id));
  httpd_resp_send_chunk(req, FORM_HTML_MID, strlen(FORM_HTML_MID));
  httpd_resp_send_chunk(req, ap_ssid, strlen(ap_ssid));
  return httpd_resp_send_chunk(req, FORM_HTML_TAIL, strlen(FORM_HTML_TAIL));
}

// POST /prov → parse form-urlencoded, save config, restart
static esp_err_t form_post_handler(httpd_req_t *req) {
  char buf[MAX_POST_BUF];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  buf[ret] = '\0';

  memset(&s_prov_cfg, 0, sizeof(s_prov_cfg));

  // Parse URL-encoded form: key=value&key=value
  char *saveptr;
  char *pair = strtok_r(buf, "&", &saveptr);
  while (pair) {
    char *eq = strchr(pair, '=');
    if (eq) {
      *eq = '\0';
      char *key = pair;
      char *val = eq + 1;

      // URL-decode the value (handle + as space, %XX hex)
      char *src = val, *dst = val;
      while (*src) {
        if (*src == '+') {
          *dst++ = ' ';
          src++;
        } else if (*src == '%' && src[1] && src[2]) {
          char hex[3] = {src[1], src[2], '\0'};
          *dst++ = (char)strtol(hex, NULL, 16);
          src += 3;
        } else {
          *dst++ = *src++;
        }
      }
      *dst = '\0';

      if (strcmp(key, "ssid") == 0)
        strncpy(s_prov_cfg.ssid, val, sizeof(s_prov_cfg.ssid) - 1);
      else if (strcmp(key, "password") == 0)
        strncpy(s_prov_cfg.password, val, sizeof(s_prov_cfg.password) - 1);
      else if (strcmp(key, "broker_addr") == 0)
        strncpy(s_prov_cfg.broker_addr, val,
                sizeof(s_prov_cfg.broker_addr) - 1);
    }
    pair = strtok_r(NULL, "&", &saveptr);
  }

  // Validate: all 3 fields required
  if (s_prov_cfg.ssid[0] == '\0' || s_prov_cfg.password[0] == '\0' ||
      s_prov_cfg.broker_addr[0] == '\0') {
    ESP_LOGE(TAG, "Missing required fields");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
                           "<html><body><h2>Missing fields</h2>"
                           "<p><a href=\"/\">Back</a></p></body></html>",
                           HTTPD_RESP_USE_STRLEN);
  }

  ESP_LOGI(TAG, "Config received: ssid=%s broker=%s", s_prov_cfg.ssid,
           s_prov_cfg.broker_addr);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req,
                  "<html><head><meta charset=\"utf-8\">"
                  "<meta http-equiv=\"refresh\" content=\"3\">"
                  "</head><body style=\"text-align:center;padding-top:40px\">"
                  "<h2>\u062a\u0646\u0638\u06cc\u0645 \u0627\u0646\u062c\u0627\u0645 \u0634\u062f / Saved!</h2>"
                  "<p>Restarting in 3 seconds...</p>"
                  "</body></html>",
                  HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static void stop_prov_wifi(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
  }
  if (s_ap_netif) {
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = NULL;
  }
  esp_wifi_stop();
  esp_wifi_deinit();
}

static esp_err_t start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    return err;
  }

  httpd_uri_t captive_android = {.uri = "/generate_204",
                                 .method = HTTP_GET,
                                 .handler = captive_android_handler};
  httpd_register_uri_handler(s_server, &captive_android);

  httpd_uri_t captive_ios = {.uri = "/hotspot-detect.html",
                             .method = HTTP_GET,
                             .handler = captive_ios_handler};
  httpd_register_uri_handler(s_server, &captive_ios);

  httpd_uri_t form_get = {.uri = "/",
                          .method = HTTP_GET,
                          .handler = form_get_handler};
  httpd_register_uri_handler(s_server, &form_get);

  httpd_uri_t form_post = {.uri = "/prov",
                           .method = HTTP_POST,
                           .handler = form_post_handler};
  httpd_register_uri_handler(s_server, &form_post);

  ESP_LOGI(TAG, "HTTP server started on port 80");
  return ESP_OK;
}

esp_err_t provision_start(void) {
  ESP_LOGW(TAG, "Entering provisioning mode (5 min timeout)");

  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

  s_ap_netif = esp_netif_create_default_wifi_ap();

  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char ap_ssid[32];
  snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", PROV_AP_SSID_PREFIX,
           mac[4], mac[5]);

  wifi_config_t ap_cfg = {
      .ap =
          {
              .ssid_len = strlen(ap_ssid),
              .channel = 1,
              .authmode = WIFI_AUTH_OPEN,
              .max_connection = 4,
          },
  };
  memcpy(ap_cfg.ap.ssid, ap_ssid, strlen(ap_ssid));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "AP started: %s (open)", ap_ssid);

  if (start_http_server() != ESP_OK) {
    stop_prov_wifi();
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Connect to %s and open http://192.168.4.1", ap_ssid);

  int64_t start = esp_timer_get_time();
  while (esp_timer_get_time() - start < PROV_TIMEOUT_US) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Button press during provisioning → cancel, restart to normal
    if (gpio_get_level(RESET_GPIO) == 1) {
      ESP_LOGW(TAG, "Button pressed during provisioning — restarting");
      stop_prov_wifi();
      esp_restart();
      return ESP_OK; // unreachable
    }
    if (s_prov_cfg.ssid[0] != '\0') {
      ESP_LOGI(TAG, "Config received, saving and restarting");
      config_write(&s_prov_cfg);
      stop_prov_wifi();
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
      return ESP_OK; // unreachable
    }
  }

  ESP_LOGW(TAG, "Provisioning timeout, resuming normal operation");
  stop_prov_wifi();
  return ESP_ERR_TIMEOUT;
}
