/**
 * Common stuff in app.c and ota.c
 */
#pragma once

extern EventGroupHandle_t appState;
extern const int OTA_DONE;
extern const uint8_t *ota_crt_start;


extern void ota_task(void * pvParameter);
