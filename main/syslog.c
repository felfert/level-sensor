/* Implementation of syslog via UDP
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#include "syslog.h"
#include "time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_libc.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "esp_libc.h"
#include "tcpip_adapter.h"

#include "common.h"

#ifndef WDEV_NOW
# define WDEV_NOW() REG_READ(WDEV_COUNT_REG)
#endif

enum syslog_state {
    SYSLOG_NONE,        // not initialized
    SYSLOG_WAIT,        // waiting for Wifi
    SYSLOG_INIT,        // WIFI avail, must initialize
    SYSLOG_INITDONE,
    SYSLOG_READY,       // Wifi established, ready to send
    SYSLOG_SEND,
    SYSLOG_HALTED,      // heap full, discard message
    SYSLOG_ERROR
};

typedef struct syslog_host_t syslog_host_t;
struct syslog_host_t {
    uint32_t           min_heap_size;  // minimum allowed heap size when buffering
    ip_addr_t          addr;
    uint16_t           port;
    struct sockaddr_in dstaddr;
    int                sock;
    int                facility;
    char               *appname;
    char               *hostname;
};

// buffered syslog event - eg. if network stack isn't up and running
typedef struct syslog_entry_t syslog_entry_t;
struct syslog_entry_t {
    syslog_entry_t *next;
    time_t      now;
    uint16_t    pri;
    char        msg[]; // Holds appname followed by message (both 0-terminated)
};

#define TAG "syslog"

static syslog_host_t syslogHost = { .min_heap_size = CONFIG_SYSLOG_MINHEAP };
static syslog_entry_t *syslogQueue = NULL;
static enum syslog_state syslogState = SYSLOG_NONE;

static void syslog_task(void * pvParameter);
static syslog_entry_t *__compose(int facility, int severity, const char *app, const char *fmt, ...);
static void __syslog(int facility, int severity, const char *app, const char *fmt, ...);

static void __syslog_set_hostname(const char *name) {
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    char *old = syslogHost.hostname;
    if (NULL != name && (0 < strlen(name))) {
        syslogHost.hostname = strdup(name);
    } else {
        const char *hn;
        if (ESP_OK == tcpip_adapter_get_hostname(TCPIP_ADAPTER_IF_STA, &hn)) {
            syslogHost.hostname = strdup(hn);
        } else {
            syslogHost.hostname = strdup("unknown");
        }
    }
    if (NULL != old) {
        free(old);
    }
}

static void __syslog_set_appname(const char *name) {
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    char *old = syslogHost.appname;
    if (NULL != name && (0 < strlen(name))) {
        syslogHost.appname = strdup(name);
    } else {
        syslogHost.appname = strdup("-");
    }
    if (NULL != old) {
        free(old);
    }
}

static const char *syslog_get_status(void) {
    switch (syslogState) {
        case SYSLOG_NONE:
            return "SYSLOG_NONE";
        case SYSLOG_WAIT:
            return "SYSLOG_WAIT";
        case SYSLOG_INIT:
            return "SYSLOG_INIT";
        case SYSLOG_INITDONE:
            return "SYSLOG_INITDONE";
        case SYSLOG_READY:
            return "SYSLOG_READY";
        case SYSLOG_SEND:
            return "SYSLOG_SEND";
        case SYSLOG_HALTED:
            return "SYSLOG_HALTED";
        case SYSLOG_ERROR:
            return "SYSLOG_ERROR";
        default:
            break;
    }
    return "UNKNOWN ";
}

static void syslog_set_status(enum syslog_state state) {
    if (syslogState != state) {
        syslogState = state;
        ESP_LOGD(TAG, "%s: %s (%d)", __FUNCTION__, syslog_get_status(), state);
    }
}

#ifndef CONFIG_SYSLOG_SENDDATE
# define CONFIG_SYSLOG_SENDDATE 0
#endif
static void __send_udp() {
    if (syslogQueue == NULL) {
        syslog_set_status(SYSLOG_READY);
        xEventGroupClearBits(appState, SYSLOG_QUEUED);
    } else {
        ESP_LOGD(TAG, "%s", __FUNCTION__);
        syslog_entry_t *pse = syslogQueue;
        char *appname = pse->msg;
        char *msg = pse->msg;
        msg += (strlen(appname) + 1);
        struct tm *tp = gmtime(&pse->now);
        char tstamp[30];
        if (CONFIG_SYSLOG_SENDDATE) {
            strftime(tstamp, 30, "%FT%TZ", tp);
        } else {
            sprintf(tstamp, "-");
        }
        char *p = NULL;
        int len = asprintf(&p, "<%d>1 %s %s %s - - %s", pse->pri, tstamp, syslogHost.hostname, appname, msg);
        if (0 > len) {
            ESP_LOGE(TAG, "%s: out of memory", __FUNCTION__);
            return;
        }
        ESP_LOGD(TAG, "%s: len=%d, dgram='%s'", __FUNCTION__, len, p);
        int res = sendto(syslogHost.sock, (uint8_t *)p, len, 0,
                (struct sockaddr *)&syslogHost.dstaddr, sizeof(syslogHost.dstaddr));
        free(p);
        if (res != len) {
            ESP_LOGE(TAG, "%s: error %d", __FUNCTION__, errno);
            return;
        }
        taskENTER_CRITICAL();
        syslogQueue = syslogQueue->next;
        taskEXIT_CRITICAL();
        free(pse);
        if (NULL == syslogQueue) {
            ESP_LOGD(TAG, "%s Q => NULL", __FUNCTION__);
            xEventGroupClearBits(appState, SYSLOG_QUEUED);
            syslog_set_status(SYSLOG_READY);
        } else {
            syslog_set_status(SYSLOG_SEND);
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

static void __init(const char *destination) {

    ESP_LOGD(TAG, "destination=%s *host=0x%x", destination,
            (NULL == destination) ?  0 : *destination);

    if (NULL == destination) {
        // disable and unregister syslog handler
        syslog_set_status(SYSLOG_HALTED);
        // clean up syslog queue
        syslog_entry_t *pse = syslogQueue;
        while (NULL != pse) {
            syslog_entry_t *next = pse->next;
            os_free(pse);
            pse = next;
        }
        taskENTER_CRITICAL();
        syslogQueue = NULL;
        taskEXIT_CRITICAL();
        ESP_LOGD(TAG, "%s Q => NULL", __FUNCTION__);
        return;
    }

    if (!*destination) {
        syslog_set_status(SYSLOG_HALTED);
        return;
    }

    char *host = strdup(destination);
    char *p = strchr(host, ':');
    if (NULL != p) {
        *p++ = '\0';
        syslogHost.port = atoi(p);
    }
    if (syslogHost.port == 0) {
        syslogHost.port = 514;
    }

    if (NULL == syslogHost.appname) {
        syslogHost.appname = strdup(TAG);
    }

    if (NULL == syslogHost.hostname) {
        __syslog_set_hostname(NULL);
    }

    if (NULL == syslogHost.appname) {
        __syslog_set_appname(NULL);
    }

    __syslog(LOG_USER, LOG_DEBUG, TAG,
            "destination: %s:%d", host, syslogHost.port);

    if (!inet_aton((const char *)host, &syslogHost.addr)) {
        struct hostent *he = gethostbyname(host);
        if (NULL == he) {
            syslog_set_status(SYSLOG_ERROR);
        } else {
            syslogHost.addr = *(ip_addr_t *)he->h_addr;
        }
    }
    syslogHost.dstaddr.sin_addr.s_addr = syslogHost.addr.addr;
    syslogHost.dstaddr.sin_family = AF_INET;
    syslogHost.dstaddr.sin_port = htons(syslogHost.port);
    syslogHost.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (syslogHost.sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        syslog_set_status(SYSLOG_ERROR);
    }
}

/**
 * Main syslog task.
 */
static void syslog_task(void * pvParameter) {

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(appState, WIFI_CONNECTED, pdFALSE, pdFALSE, 100 / portTICK_PERIOD_MS);
        if (bits & WIFI_CONNECTED) {
            switch (syslogState) {

                case SYSLOG_WAIT:
                    ESP_LOGD(TAG, "%s: Wifi connected", syslog_get_status());
                    syslog_set_status(SYSLOG_INIT);
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    break;

                case SYSLOG_INIT:
                    ESP_LOGD(TAG, "%s: init syslog", syslog_get_status());
                    syslog_set_status(SYSLOG_INITDONE);
                    __init(CONFIG_SYSLOG_HOST);
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                    break;

                case SYSLOG_INITDONE:
                    if (NULL != syslogQueue) {
                        syslog_set_status(SYSLOG_READY);
                    } else {
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                    }
                    break;

                case SYSLOG_READY:
                    bits = xEventGroupWaitBits(appState, SYSLOG_QUEUED, pdFALSE, pdFALSE, 5000 / portTICK_PERIOD_MS);
                    if (bits & SYSLOG_QUEUED) {
                        __send_udp();
                    }
                    break;

                case SYSLOG_SEND:
                    ESP_LOGD(TAG, "%s: start sending", syslog_get_status());
                    __send_udp();
                    break;

                default:
                    ESP_LOGD(TAG, "%s: default", syslog_get_status());
                    vTaskDelay(3000 / portTICK_PERIOD_MS);
                    break;
            }
        } else {
            ESP_LOGD(TAG, "%s: %s (delay 2s)", __FUNCTION__, syslog_get_status());
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
}

static void __add_entry(syslog_entry_t *entry) {
    ESP_LOGD(TAG, "%s: %s E=%p QA=%p", __FUNCTION__, syslog_get_status(), entry, syslogQueue);
    taskENTER_CRITICAL();
    syslog_entry_t *pse = syslogQueue;
    // append msg to syslog_queue
    if (NULL == pse) {
        syslogQueue = entry;
        taskEXIT_CRITICAL();
    } else {
        while (NULL != pse->next) {
            pse = pse->next;
        }
        pse->next = entry;
        taskEXIT_CRITICAL();
        // ensure we have sufficient heap for the rest of the system
        if (esp_get_free_heap_size() < syslogHost.min_heap_size) {
            if (syslogState != SYSLOG_HALTED) {
                ESP_LOGW(TAG, "%s: Warning: queue filled up, halted", __FUNCTION__);
                entry->next = __compose(LOG_USER, LOG_CRIT, TAG, "queue filled up, halted");
                if (syslogState == SYSLOG_READY) {
                    __send_udp();
                }
                syslog_set_status(SYSLOG_HALTED);
            }
        }
        ESP_LOGD(TAG, "%s: append free=%d", __FUNCTION__, esp_get_free_heap_size());
    }
    ESP_LOGD(TAG, "%s: QE=%p", __FUNCTION__, syslogQueue);
    xEventGroupSetBits(appState, SYSLOG_QUEUED);
    
}

static syslog_entry_t *__compose_common(int facility, int severity, const char *app, char *ret, int msglen) {
    int applen = ((NULL == app) ? strlen(syslogHost.appname) : strlen(app)) + 1;
    ret = realloc(ret, msglen + applen + sizeof (syslog_entry_t));
    if (NULL == ret) {
        return NULL;
    }
    ESP_LOGD(TAG, "%s: free=%d", __FUNCTION__, esp_get_free_heap_size());
    char *mdest = ((syslog_entry_t *)ret)->msg + applen;
    memmove(mdest, ret, msglen);
    ESP_LOGD(TAG, "msg=%s", mdest);
    if (NULL == app) {
        strcpy(((syslog_entry_t *)ret)->msg, syslogHost.appname);
    } else {
        strcpy(((syslog_entry_t *)ret)->msg, app);
    }
    ESP_LOGD(TAG, "app=%s", ((syslog_entry_t *)ret)->msg);
    ESP_LOGD(TAG, "sz=%d", msglen + applen + sizeof (syslog_entry_t));
    if (NTP_SYNCED & xEventGroupWaitBits(appState, NTP_SYNCED, pdFALSE, pdFALSE, 0)) {
        time(&(((syslog_entry_t*)ret)->now));
    } else {
        ((syslog_entry_t*)ret)->now = WDEV_NOW() / 1000000;
    }
    ((syslog_entry_t*)ret)->pri = facility | severity;
    ((syslog_entry_t*)ret)->next = NULL;
    return (syslog_entry_t *)ret;
}

static syslog_entry_t * __compose(int facility, int severity, const char *app, const char *fmt, ...) {
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    char *ret = NULL;
    va_list alist;
    va_start(alist, fmt);
    int msglen = vasprintf(&ret, fmt, alist);
    va_end(alist);
    if (0 > msglen) {
        return NULL;
    }
    return __compose_common(facility, severity, app, ret, msglen + 1);
}

static syslog_entry_t * __vcompose(int facility, int severity, const char *app, const char *fmt, va_list alist) {
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    char *ret = NULL;
    int msglen = vasprintf(&ret, fmt, alist);
    if (0 > msglen) {
        return NULL;
    }
    int applen = ((NULL == app) ? strlen(syslogHost.appname) : strlen(app)) + 1;
    ret = realloc(ret, msglen + applen + sizeof (syslog_entry_t));
    if (NULL == ret) {
        return NULL;
    }
    return __compose_common(facility, severity, app, ret, msglen + 1);
}

static void __syslog(int facility, int severity, const char *app, const char *fmt, ...)
{
    ESP_LOGD(TAG, "%s status: %s", __FUNCTION__, syslog_get_status());

    if (strlen(CONFIG_SYSLOG_HOST) == 0 || syslogState == SYSLOG_ERROR || syslogState == SYSLOG_HALTED) {
        return;
    }

    if (severity > CONFIG_SYSLOG_FILTER) {
        return;
    }

    // compose the syslog message
    void *arg = __builtin_apply_args();
    void *res = __builtin_apply((void*)__compose, arg, 128);
    if (NULL == res) {
        return;
    }
    syslog_entry_t *se  = *(syslog_entry_t **)res;
    __add_entry(se);

    if (syslogState == SYSLOG_NONE) {
        syslog_set_status(SYSLOG_WAIT);
    }
}

static void __vsyslog(int facility, int severity, const char *app, const char *fmt, va_list arglist)
{
    ESP_LOGD(TAG, "%s status: %s", __FUNCTION__, syslog_get_status());

    if (strlen(CONFIG_SYSLOG_HOST) == 0 || syslogState == SYSLOG_ERROR || syslogState == SYSLOG_HALTED)
        return;

    if (severity > CONFIG_SYSLOG_FILTER) {
        return;
    }

    // compose the syslog message
    void *res = __vcompose(facility, severity, app, fmt, arglist);
    if (NULL == res) {
        return;
    }
    syslog_entry_t *se  = (syslog_entry_t *)res;
    __add_entry(se);

    if (syslogState == SYSLOG_NONE) {
        syslog_set_status(SYSLOG_WAIT);
    }
}

void syslog(int __pri, const char *__fmt, ...) {
    va_list alist;
    va_start(alist, __fmt);
    __vsyslog(syslogHost.facility, __pri, NULL, __fmt, alist);
    va_end(alist);
}

void vsyslog(int __pri, const char *__fmt, va_list alist) {
    __vsyslog(syslogHost.facility, __pri, NULL, __fmt, alist);
}

void syslogx(int __pri, const char *app, const char *__fmt, ...) {
    va_list alist;
    va_start(alist, __fmt);
    __vsyslog(syslogHost.facility, __pri, app, __fmt, alist);
    va_end(alist);
}

void vsyslogx(int __pri, const char *app, const char *__fmt, va_list alist) {
    __vsyslog(syslogHost.facility, __pri, app, __fmt, alist);
}

void openlog(const char *ident, int option, int facility) {
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    __syslog_set_appname(ident);
    syslogHost.facility = facility;
    xTaskCreate(&syslog_task, "syslog_task", 2048, NULL, 5, NULL);
}

void closelog(void) { /* NOP */ };

void set_syslog_hostname(const char *hostname) {
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    __syslog_set_hostname(hostname);
}
