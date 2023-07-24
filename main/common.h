/**
 * Common stuff in app.c and ota.c
 */
#pragma once

#include "syslog.h"

extern EventGroupHandle_t appState;
extern const int NTP_SYNCED;
extern const int OTA_DONE;
extern const int WIFI_CONNECTED;


#ifdef __cplusplus
extern "C" {
#endif

extern void ota_task(void * pvParameter);

#ifdef __cplusplus
}
#endif
