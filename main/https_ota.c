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
#include "nvs_flash.h"
#include "nvs.h"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#include "common.h"

static const char *wheel_char = "/-\\|";
static int wheel_idx = 0;
static void wheel() {
    printf("%c\r", wheel_char[wheel_idx]);
    fflush(stdout);
    wheel_idx = (wheel_idx + 1) % 4;
}

#define OTA_BUF_SIZE    CONFIG_OTA_BUF_SIZE
static const char *TAG = "OTA update";

static int invalid_content_type = 0;

// Last modified header of last successful download. Stored in NVS
static char if_modified_since[256] = "\0";
static char last_modified[256] = "\0";
#define IF_MODIFIED_SINCE_NVS_KEY "ota_lms"

static void get_if_modified_since() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("my_ota", NVS_READWRITE, &nvs_handle);
    if (ESP_OK == err) {
        size_t sz = sizeof(if_modified_since);
        err = nvs_get_str(nvs_handle, IF_MODIFIED_SINCE_NVS_KEY, if_modified_since, &sz);
        switch (err) {
            case ESP_OK:
                ESP_LOGD(TAG, "got from NVS: \"%s\"", if_modified_since);
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                if_modified_since[0] = '\0';
                break;
            default:
                ESP_LOGE(TAG, "Unable to read NVS: %s", esp_err_to_name(err));
                if_modified_since[0] = '\0';
                break;
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Unable to open NVS: %s", esp_err_to_name(err));
        if_modified_since[0] = '\0';
    }
}

static void set_if_modified_since(const char *value) {
    if ((NULL == value) || 0 == strlen(value)) {
        return;
    }
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("my_ota", NVS_READWRITE, &nvs_handle);
    if (ESP_OK == err) {
        err = nvs_set_str(nvs_handle, IF_MODIFIED_SINCE_NVS_KEY, value);
        switch (err) {
            case ESP_OK:
                ESP_LOGD(TAG, "wrote to NVS: \"%s\"", value);
                break;
            default:
                ESP_LOGE(TAG, "Unable to write NVS: %s", esp_err_to_name(err));
                break;
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Unable to open NVS: %s", esp_err_to_name(err));
    }
}

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

    get_if_modified_since();
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "Failed to open HTTP connection: %d", err);
        return err;
    }
    esp_http_client_fetch_headers(client);

    int http_status = esp_http_client_get_status_code(client);
    if (304 <= http_status) {
        ESP_LOGI(TAG, "No new firmware available");
        http_cleanup(client);
        return ESP_ERR_INVALID_STATE;
    }
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
    ESP_LOGI(TAG, "Downloading ...");
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Passive OTA partition not found");
        http_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
        http_cleanup(client);
        return err;
    }
    ESP_LOGD(TAG, "esp_ota_begin succeeded");

    esp_err_t ota_write_err = ESP_OK;
    char *upgrade_data_buf = (char *)malloc(OTA_BUF_SIZE);
    if (!upgrade_data_buf) {
        ESP_LOGE(TAG, "Could not allocate memory to upgrade data buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Please wait. This may take time");
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
            if (0 < strlen(if_modified_since)) {
                esp_http_client_set_header(evt->client, "If-Modified-Since", if_modified_since);
            }
            break;
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADERS_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            if (0 == strcasecmp(evt->header_key, "Last-Modified")) {
                strcpy(last_modified, evt->header_value);
                return ESP_OK;
            }
            if (0 == strcasecmp(evt->header_key, "Content-Type") && strcmp(evt->header_value, "application/octet-stream")) {
                ESP_LOGE(TAG, "Invalid content type %s", evt->header_value);
                invalid_content_type = 1;
                return ESP_FAIL; // This is ignored in esp_http_client - (design flaw?)
            }
            break;
        case HTTP_EVENT_ON_DATA:
            wheel();
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
    ESP_LOGI(TAG, "Checking %s", CONFIG_OTA_URI);
    esp_http_client_config_t config = {
        .url = CONFIG_OTA_URI,
        .cert_pem = (char *)pvParameter,
        .event_handler = _http_event_handler,
    };
    esp_err_t ret = https_ota(&config);
    if (ESP_OK == ret) {
        if (0 < strlen(last_modified)) {
            set_if_modified_since(last_modified);
        }
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        esp_restart();
    } else {
        if (ESP_ERR_INVALID_STATE != ret) {
            ESP_LOGE(TAG, "Firmware upgrade failed");
        }
        xEventGroupSetBits(appState, OTA_DONE);
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
