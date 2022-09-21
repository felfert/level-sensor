// This file was copied from ESP8266_RTOS_SDK/components/esp_https_ota/src/esp_https_ota.c
// in order to improve error handling.
//
// Copyright 2017-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#include "common.h"

#define OTA_BUF_SIZE    CONFIG_OTA_BUF_SIZE
static const char *TAG = "OTA update";

static int invalid_content_type = 0;
static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static esp_err_t https_ota(const esp_http_client_config_t *config)
{
    invalid_content_type = 0;
    if (!config) {
        ESP_LOGE(TAG, "esp_http_client config not found");
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_OTA_ALLOW_HTTP
    if (!config->cert_pem) {
        ESP_LOGE(TAG, "Server certificate not found in esp_http_client config");
        return ESP_FAIL;
    }
#endif

    esp_http_client_handle_t client = esp_http_client_init(config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        return ESP_FAIL;
    }

#if !CONFIG_OTA_ALLOW_HTTP
    if (esp_http_client_get_transport_type(client) != HTTP_TRANSPORT_OVER_SSL) {
        ESP_LOGE(TAG, "Transport is not over HTTPS");
        return ESP_FAIL;
    }
#endif

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "Failed to open HTTP connection: %d", err);
        return err;
    }
    esp_http_client_fetch_headers(client);

    int http_status = esp_http_client_get_status_code(client);
    if (400 <= http_status) {
        ESP_LOGE(TAG, "HTTP request returned error %d", http_status);
        http_cleanup(client);
        return ESP_FAIL;
    }
    if (invalid_content_type) {
        http_cleanup(client);
        return ESP_FAIL;
    }


    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;
    ESP_LOGI(TAG, "Starting OTA...");
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Passive OTA partition not found");
        http_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
        http_cleanup(client);
        return err;
    }
    ESP_LOGI(TAG, "esp_ota_begin succeeded");

    esp_err_t ota_write_err = ESP_OK;
    char *upgrade_data_buf = (char *)malloc(OTA_BUF_SIZE);
    if (!upgrade_data_buf) {
        ESP_LOGE(TAG, "Could not allocate memory to upgrade data buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Please Wait. This may take time");
    int binary_file_len = 0;
    while (1) {
        int data_read = esp_http_client_read(client, upgrade_data_buf, OTA_BUF_SIZE);
        if (data_read == 0) {
            printf("\r\n");
            ESP_LOGD(TAG, "Connection closed,all data received");
            break;
        }
        if (data_read < 0) {
            printf("\r\n");
            ESP_LOGE(TAG, "Error: SSL data read error");
            break;
        }
        if (data_read > 0) {
            ota_write_err = esp_ota_write( update_handle, (const void *)upgrade_data_buf, data_read);
            if (ota_write_err != ESP_OK) {
                break;
            }
            binary_file_len += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_len);
        }
    }
    free(upgrade_data_buf);
    http_cleanup(client); 
    ESP_LOGD(TAG, "Total binary data length writen: %d", binary_file_len);
    
    esp_err_t ota_end_err = esp_ota_end(update_handle);
    if (ota_write_err != ESP_OK) {
        ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%d", err);
        return ota_write_err;
    } else if (ota_end_err != ESP_OK) {
        ESP_LOGE(TAG, "Error: esp_ota_end failed! err=0x%d. Image is invalid", ota_end_err);
        return ota_end_err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%d", err);
        return err;
    }
    ESP_LOGD(TAG, "esp_ota_set_boot_partition succeeded"); 

    return ESP_OK;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            esp_http_client_set_header(evt->client, "User-Agent", "ESP8266 OTA Updater/1.0");
            break;
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADERS_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            if (0 == strcasecmp(evt->header_key, "Content-Type") && strcmp(evt->header_value, "application/octet-stream")) {
                ESP_LOGE(TAG, "Invalid content type %s", evt->header_value);
                invalid_content_type = 1;
                return ESP_FAIL; // This is ignored in esp_http_client - (design flaw?)
            }
            break;
        case HTTP_EVENT_ON_DATA:
            putchar('.');
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

void ota_task(void * pvParameter)
{
    ESP_LOGI(TAG, "Downloading %s", CONFIG_OTA_URI);
    esp_http_client_config_t config = {
        .url = CONFIG_OTA_URI,
        .cert_pem = (char *)ota_crt_start,
        .event_handler = _http_event_handler,
    };
    esp_err_t ret = https_ota(&config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
        xEventGroupSetBits(appState, OTA_DONE);
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
