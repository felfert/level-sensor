/* Sensor for home automation, based on WiFi Connection Example using WPA2 Enterprise (EAP-TLS)
 *
 * Original Copyright (C) 2006-2016, ARM Limited, All Rights Reserved, Apache 2.0 License.
 * Additions Copyright (C) Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD, Apache 2.0 License.
 * Additions Copyright (C) 2022 Fritz Elfert, Apache 2.0 License
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string>
#include <cstring>
#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_compiler.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "esp_event_loop.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#include "x509helper.h"
#include "common.h"

static uint8_t basemac[6];
static std::string identity;
static esp_mqtt_client_handle_t client;

// FreeRTOS event group for signaling various application states
EventGroupHandle_t appState;

const int WIFI_CONNECTED = BIT0;
static const int MQTT_CONNECTED = BIT1;
static const int OTA_REQUIRED   = BIT2;
const int OTA_DONE              = BIT3;
const int NTP_SYNCED            = BIT4;
const int SYSLOG_QUEUED         = BIT5;

static const esp_app_desc_t *ad;

static const char* TAG      = "sensor";
static const char* TAG_MEM  = "heap";
static const char* TAG_MQTT = "mqtt";

#include "embed.h"

static unsigned int client_crt_bytes;
static unsigned int client_key_bytes;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
        int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        system_event_sta_disconnected_t *event = (system_event_sta_disconnected_t *)event_data;
        if (event->reason == WIFI_REASON_BASIC_RATE_NOT_SUPPORT) {
            // Switch to 802.11 bgn mode
            esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        }
        esp_wifi_connect();
        xEventGroupClearBits(appState, WIFI_CONNECTED);
        xEventGroupClearBits(appState, NTP_SYNCED);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(appState, WIFI_CONNECTED);
    }
}

/**
 *  Get the CN from our client certificate, because we need it as identity for EAP-TLS
 */
static void init_identity(void) {
    if (identity.empty()) {
        client_crt_bytes = client_crt_end - client_crt_start;
        client_key_bytes = client_key_end - client_key_start;
        mbedtls_x509_crt mycert;
        mbedtls_x509_crt_init(&mycert);
        if (0 > mbedtls_x509_crt_parse(&mycert, client_crt_start, client_crt_bytes)) {
            ESP_LOGE(TAG, "Unable to parse client cert");
            abort();
        }
        getOidByName(&(&mycert)->subject, "CN", identity);
        mbedtls_x509_crt_free(&mycert);
    }
}

static void wifi_init(void)
{
    init_identity();
    ESP_LOGI(TAG, "My MAC: " MACSTR, MAC2STR(basemac));
    ESP_LOGI(TAG, "My CN:  %s", identity.c_str());

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = { ESP_COMPILER_DESIGNATED_INIT_AGGREGATE_TYPE_STR(ssid, CONFIG_WIFI_SSID) }
    };
    ESP_LOGI(TAG, "Connecting to WiFi SSID %s ...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_cert_key(client_crt_start, client_crt_bytes,
                client_key_start, client_key_bytes, nullptr, 0));
    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)identity.c_str(), identity.length()));
    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_enable());
    ESP_ERROR_CHECK(esp_wifi_start());
}

static xQueueHandle gpio_evt_queue = nullptr;

#define GPIO_INPUT GPIO_NUM_4 // GPIO4 aka D2 on NodeMCU or D1 mini

static int last_level = -2;

/**
 * Publish current state of GPIO pin to MQTT.
 * This is in a separate function, so that it can be
 * invoked at boot.
 */
static void publish_gpio(gpio_num_t gpio) {
    int lvl = gpio_get_level(gpio);
    if (last_level != lvl) {
        vTaskDelay(10 / portTICK_PERIOD_MS); // debounce
        if (lvl == gpio_get_level(gpio)) {
            last_level = lvl;
            char topic[50];
            snprintf(topic, sizeof(topic), "esp8266/gpio%d/%d", gpio, gpio_get_level(gpio));
            esp_mqtt_client_publish(client, topic, identity.c_str(), 0, 0, 0);
        }
    }
}

/**
 * Publish current version to MQTT.
 */
static void publish_version() {
    char topic[50];
    snprintf(topic, sizeof(topic), "esp8266/version/%s", ad->version);
    esp_mqtt_client_publish(client, topic, identity.c_str(), 0, 0, 0);
}

/**
 * GPIO task
 * Publishes changes queued by the ISR to MQTT.
 */
static void gpio_task(void * pvParameter) {
    gpio_num_t gpio;
    while (true) {
        if (xQueueReceive(gpio_evt_queue, &gpio, portMAX_DELAY)) {
            ESP_LOGD(TAG, "GPIO[%d] intr", gpio);
            publish_gpio(gpio);
        }
    }
}

/**
 * The gpio ISR
 * Just enqueues an event.
 */
static void gpio_isr(void *arg) {
    // enqueue events only if we are connected to MQTT
    if (xEventGroupWaitBits(appState, MQTT_CONNECTED, pdFALSE, pdFALSE, 0) & MQTT_CONNECTED) {
        uint32_t gpio_num = (uint32_t) arg;
        xQueueSendFromISR(gpio_evt_queue, &gpio_num, nullptr);
    }
}

/**
 * Configure GPIO pin ad setup ISR.
 */
static void init_gpio(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << GPIO_INPUT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    xTaskCreate(&gpio_task, "gpio_task", 2048, nullptr, 10, nullptr);

    // install gpio isr service
    gpio_install_isr_service(0);
    // hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT, gpio_isr, (void *)GPIO_INPUT);
}

static void enable_debug(bool enable) {
    if (enable) {
        esp_log_level_set("wifi", ESP_LOG_DEBUG);
        esp_log_level_set("sensor", ESP_LOG_DEBUG);
        esp_log_level_set("OTA update", ESP_LOG_DEBUG);
        esp_log_level_set("mqtt", ESP_LOG_DEBUG);
        esp_log_level_set("heap", ESP_LOG_DEBUG);
        esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);
        //esp_log_level_set("syslog", ESP_LOG_DEBUG);
        ESP_LOGI(TAG, "debug enabled");
    } else {
        esp_log_level_set("wifi", ESP_LOG_INFO);
        esp_log_level_set("sensor", ESP_LOG_INFO);
        esp_log_level_set("OTA update", ESP_LOG_INFO);
        esp_log_level_set("mqtt", ESP_LOG_INFO);
        esp_log_level_set("heap", ESP_LOG_INFO);
        esp_log_level_set("syslog", ESP_LOG_INFO);
        ESP_LOGI(TAG, "debug disabled");
    }
}

static void mqtt_action(const std::string &topic, const std::string &data) {
    bool match_exact = 0 == data.compare(identity);
    bool match_any = data.empty();
    if (match_exact && (0 == topic.compare("esp8266/nvserase"))) {
        ESP_LOGD(TAG, "Erasing non volatile storage");
        syslogx(LOG_NOTICE, TAG, "Erasing non volatile storage");
        ESP_ERROR_CHECK(nvs_flash_erase());
        return;
    }
    if (0 == topic.compare("esp8266/update")) {
        if (match_exact || match_any) {
            xEventGroupSetBits(appState, OTA_REQUIRED);
        }
        return;
    }
    if ((0 == topic.compare("esp8266/debug")) || (0 == topic.compare("esp8266/nodebug"))) {
        if (match_exact || match_any) {
            enable_debug(0 == topic.compare("esp8266/debug"));
        }
    }
}

/**
 * Callback function gets called, when time has been synced via NTP
 */
static void ntp_sync_cb(struct timeval *tv) {
    if (SNTP_SYNC_STATUS_COMPLETED == sntp_get_sync_status()) {
        xEventGroupSetBits(appState, NTP_SYNCED);
        struct tm _tm;
        time_t now;
        time(&now);
        localtime_r(&now, &_tm);
        char tbuf[50];
        strftime(tbuf, sizeof(tbuf), "%c %Z", &_tm);
        ESP_LOGI(TAG, "Time synchronized to: %s", tbuf);
        syslog(LOG_DEBUG, "Time synchronized to: %s", tbuf);
    } else {
        xEventGroupClearBits(appState, NTP_SYNCED);
    }
}

static void check_ntpserver() {
    const ip_addr_t* ntpserver = sntp_getserver(0);
    if (nullptr != ntpserver) {
        ESP_LOGI(TAG, "NTP:  " IPSTR, IP2STR(ntpserver));
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
        sntp_set_time_sync_notification_cb(ntp_sync_cb);
        sntp_init();
    } else {
        ESP_LOGW(TAG, "NTP:  NONE");
    }
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_CONNECTED");
            syslogx(LOG_INFO, TAG_MQTT, "Connected to broker %s", CONFIG_MQTTS_URI);
            msg_id = esp_mqtt_client_subscribe(client, "esp8266/#", 0);
            ESP_LOGD(TAG_MQTT, "sent subscribe successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_publish(client, "esp8266/start", identity.c_str(), 0, 0, 0);
            ESP_LOGD(TAG_MQTT, "sent publish successful, msg_id=%d", msg_id);
            publish_version();
            publish_gpio(GPIO_INPUT);
            xEventGroupSetBits(appState, MQTT_CONNECTED);
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(appState, MQTT_CONNECTED);
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_DATA");
            ESP_LOGD(TAG_MQTT, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGD(TAG_MQTT, "DATA=%.*s", event->data_len, event->data);
            if (0 < event->topic_len) {
                std::string topic(event->topic, event->topic_len);
                std::string data(event->data, event->data_len);
                mqtt_action(topic, data);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG_MQTT, "MQTT_EVENT_ERROR");
            break;
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_BEFORE_CONNECT");
            break;
        default:
            ESP_LOGW(TAG, "Other event id:%d", event->event_id);
            break;
    }
    ESP_LOGD(TAG_MEM, "Free memory: %d bytes", esp_get_free_heap_size());
    return ESP_OK;
}

static void mqtt_init(void)
{
    char idbuf[100];
    snprintf(idbuf, sizeof(idbuf), "esp8266-%02x%02x%02x%02x%02x%02x", MAC2STR(basemac));
    const esp_mqtt_client_config_t mqtt_cfg = {
        .event_handle = mqtt_event_handler,
        .uri = CONFIG_MQTTS_URI,
        .client_id = idbuf,
        .lwt_topic = "esp8266/dead",
        .lwt_msg = identity.c_str(),
        .cert_pem = (const char *)ca_crt_start,
        .client_cert_pem = (const char *)client_crt_start,
        .client_key_pem = (const char *)client_key_start,
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
}

/**
 * Check, if update is requested. If yes, terminate MQTT connection
 * and start OTA task.
 */
static void update_check_task(void * pvParameter) {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(appState, OTA_REQUIRED,
                pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & OTA_REQUIRED) {
            ESP_LOGI(TAG, "Firmware update requested, shutting down MQTT");
            syslog(LOG_NOTICE, "Firmware update requested, shutting down MQTT");
            ESP_ERROR_CHECK(esp_mqtt_client_stop(client));
            ESP_LOGD(TAG_MEM, "Free memory: %d bytes", esp_get_free_heap_size());
            xTaskCreate(&ota_task, "ota_task", 8192, ca_crt_start, 5, nullptr);
            while (true) {
                bits = xEventGroupWaitBits(appState, OTA_DONE, pdTRUE, pdFALSE, portMAX_DELAY);
                if (bits & OTA_DONE) {
                    // If we arrive here, OTA has failed prematurely (e.g. 404 or somethin similar)
                    ESP_LOGD(TAG_MEM, "Free memory: %d bytes", esp_get_free_heap_size());
                    ESP_LOGI(TAG, "Restarting MQTT");
                    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
                    break;
                }
            }
        }
    }
}

extern "C" {
    void app_main();
}

void app_main()
{
    setenv("TZ", CONFIG_TZ, 1);
    tzset();
    esp_log_level_set("*", ESP_LOG_INFO);
    enable_debug(false);
    ad = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "APP version: %s", ad->version);
    ESP_LOGI(TAG, "APP build: %s %s", ad->date, ad->time);
    ESP_LOGI(TAG, "IDF version: %s", ad->idf_ver);
    appState = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    // Get rid of stupid "Base MAC address is not set ..." message by
    // explicitely setting base MAC addr from EFUSE.
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(basemac));
    ESP_ERROR_CHECK(esp_base_mac_addr_set(basemac));

    // Allow setting NTP server via DHCP
    // This also needs LWIP_DHCP_GET_NTP_SRV=1 defined
    sntp_servermode_dhcp(1);
    init_identity();
    set_syslog_hostname(identity.c_str());
    openlog("sensor", 0, LOG_USER);
    wifi_init();
    mqtt_init();
    tcpip_adapter_ip_info_t ip;
    memset(&ip, 0, sizeof(tcpip_adapter_ip_info_t));
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(appState, WIFI_CONNECTED,
                pdFALSE, pdFALSE, 2000 / portTICK_PERIOD_MS);
        if (bits & WIFI_CONNECTED) {
            printf("\r\n"); // WiFi connected message does not have a linefeed
            if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == 0) {
                ESP_LOGI(TAG, "IP:   " IPSTR, IP2STR(&ip.ip));
                ESP_LOGI(TAG, "MASK: " IPSTR, IP2STR(&ip.netmask));
                ESP_LOGI(TAG, "GW:   " IPSTR, IP2STR(&ip.gw));
                check_ntpserver();
            }

            if (ESP_OK == esp_mqtt_client_start(client)) {
                xTaskCreate(&update_check_task, "update_check_task", 2048, nullptr, 2, nullptr);
                init_gpio();
                break;
            }
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
}
