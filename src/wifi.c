// WiFi station, with the setup portal (wifi_setup.c) opened alongside it
// whenever there is no network: none configured, the saved one not joined
// within BOOT_WAIT_S of booting, or lost for LOST_S later. The station keeps
// retrying meanwhile; once it is back (or a new network from the setup page
// is joined) the portal closes again. No restart is involved.
#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_config.h"
#include "nvs_flash.h"
#include "wifi_setup.h"

static const char *TAG = "wifi_sta";

#define BOOT_WAIT_S 30          // first connection after power-up
#define LOST_S 7                // a drop shorter than this goes unnoticed
#define PORTAL_RETRY_S 15       // with the portal open, retry the station this often

static volatile bool s_connected;
static volatile bool s_portal;          // setup access point open: no auto-reconnect
static volatile bool s_configured;
static bool s_ever_connected;
static int s_retries;
static esp_netif_t *s_netif;
static const char *s_hostname;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_configured)
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        if (s_retries < 5 || s_retries % 10 == 0)
            ESP_LOGW(TAG, "disconnected from \"%.32s\" (reason %d, try %d)", (const char *)d->ssid, d->reason,
                     s_retries);
        s_retries++;
        // with the portal open, the watch task retries slowly: a station
        // that keeps scanning disturbs the phone on the setup network
        if (!s_portal && s_configured)
            esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_connected = true;
        s_ever_connected = true;
    }
}

static void watch_task(void *arg)
{
    int down_s = 0, retry_s = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_connected) {
            down_s = 0;
            if (s_portal) {
                ESP_LOGI(TAG, "WiFi back: closing the setup portal");
                wifi_setup_close();
                s_portal = false;
            }
            continue;
        }
        down_s++;
        int limit = s_ever_connected ? LOST_S : BOOT_WAIT_S;
        if (!s_portal && (!s_configured || down_s >= limit)) {
            ESP_LOGW(TAG, "%s: opening the setup portal", s_configured ? "no WiFi" : "no WiFi configured");
            s_portal = true;
            retry_s = 0;
            if (wifi_setup_open(s_hostname) != ESP_OK)
                s_portal = false;
        }
        if (s_portal && s_configured && ++retry_s >= PORTAL_RETRY_S) {
            retry_s = 0;
            esp_wifi_connect();
        }
    }
}

static void set_station(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = strlen(pass) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

void wifi_use_network(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "switching to the network saved from the setup page");
    s_configured = true;
    s_retries = 0;
    esp_wifi_disconnect();
    set_station(ssid, pass);
    esp_wifi_connect();
}

esp_err_t wifi_start(const char *hostname)
{
    s_hostname = hostname;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, hostname);

    // the driver's own info logs include the SSID; keep warnings only
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    // the network saved from the setup page, else the one from .env
    char ssid[33] = {0}, pass[65] = {0};
    if (!wifi_setup_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        snprintf(ssid, sizeof(ssid), "%s", WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", WIFI_PASSWORD);
    }
    s_configured = ssid[0] != 0;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    if (s_configured)
        set_station(ssid, pass);
    // no modem sleep: a sleeping station answers mDNS / IPP slowly
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    // real wall clock for IPP date-time attributes
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    xTaskCreate(watch_task, "wifi_watch", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "connecting as %s", hostname);
    return ESP_OK;
}

bool wifi_configured(void)
{
    return s_configured;
}

bool wifi_connected(void)
{
    return s_connected;
}

bool wifi_get_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    if (!s_connected || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        if (n)
            buf[0] = 0;
        return false;
    }
    snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    return true;
}

int wifi_rssi(void)
{
    int rssi = 0;
    if (!s_connected || esp_wifi_sta_get_rssi(&rssi) != ESP_OK)
        return 0;
    return rssi;
}
