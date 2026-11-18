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
#include "lwip/inet.h"
#include "nvs.h"
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
static wifi_ipv4_t s_static;            // ip == 0: DHCP

#define NET_NS "net_cfg"

// ---------------------------------------------------------------------------
// static IPv4 address (NVS "net_cfg"), set from the /firmware page

static bool load_static(wifi_ipv4_t *out)
{
    nvs_handle_t h;
    *out = (wifi_ipv4_t){0};
    if (nvs_open(NET_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = nvs_get_u32(h, "ip", &out->ip) == ESP_OK && out->ip && nvs_get_u32(h, "mask", &out->mask) == ESP_OK &&
              nvs_get_u32(h, "gw", &out->gw) == ESP_OK;
    if (ok && nvs_get_u32(h, "dns", &out->dns) != ESP_OK)
        out->dns = 0;
    nvs_close(h);
    if (!ok)
        *out = (wifi_ipv4_t){0};
    return ok;
}

bool wifi_static_valid(const wifi_ipv4_t *c)
{
    uint32_t mask = ntohl(c->mask), ip = ntohl(c->ip), gw = ntohl(c->gw);
    if (!mask || (~mask & (~mask + 1)) != 0)
        return false;   // not a run of ones followed by zeros
    uint32_t host = ~mask;
    if ((ip & mask) != (gw & mask) || ip == gw)
        return false;   // the gateway must be another address of the same subnet
    if ((ip & host) == 0 || (ip & host) == host || (gw & host) == 0 || (gw & host) == host)
        return false;   // network or broadcast address
    return true;
}

// The address loaded at startup (what the station uses): no flash access, so
// the HTTP connection threads (stacks in PSRAM) may call it.
bool wifi_static_get(wifi_ipv4_t *out)
{
    *out = s_static;
    return s_static.ip != 0;
}

esp_err_t wifi_static_set(const wifi_ipv4_t *cfg)
{
    nvs_handle_t h;
    bool dhcp = !cfg || !cfg->ip;
    if (!dhcp && !wifi_static_valid(cfg))
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_open(NET_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    if (dhcp) {
        err = nvs_erase_all(h);
    } else {
        err = nvs_set_u32(h, "ip", cfg->ip);
        if (err == ESP_OK)
            err = nvs_set_u32(h, "mask", cfg->mask);
        if (err == ESP_OK)
            err = nvs_set_u32(h, "gw", cfg->gw);
        if (err == ESP_OK)
            err = nvs_set_u32(h, "dns", cfg->dns);
    }
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "saved: %s (from the next restart)", dhcp ? "DHCP" : "static address");
    return err;
}

bool wifi_current_ipv4(wifi_ipv4_t *out)
{
    esp_netif_ip_info_t ip;
    esp_netif_dns_info_t dns = {0};
    *out = (wifi_ipv4_t){0};
    if (!s_connected || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK)
        return false;
    out->ip = ip.ip.addr;
    out->mask = ip.netmask.addr;
    out->gw = ip.gw.addr;
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
        out->dns = dns.ip.u_addr.ip4.addr;
    return true;
}

// On every association, after the default handler (which starts DHCP): as
// in ESP-IDF's static_ip example. Setting the address posts IP_EVENT_STA_GOT_IP.
static void apply_static(void)
{
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "static address: cannot stop DHCP (%s)", esp_err_to_name(err));
        return;
    }
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = s_static.ip;
    ip.netmask.addr = s_static.mask;
    ip.gw.addr = s_static.gw;
    err = esp_netif_set_ip_info(s_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static address: %s", esp_err_to_name(err));
        return;
    }
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = s_static.dns ? s_static.dns : s_static.gw;
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_configured)
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        if (s_static.ip)
            apply_static();
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
    if (s_static.ip) {
        // a static address belongs to the old network: DHCP on the new one
        ESP_LOGW(TAG, "new network: static address dropped, back to DHCP");
        s_static = (wifi_ipv4_t){0};
        wifi_static_set(NULL);
        esp_netif_dhcpc_start(s_netif);
    }
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

    if (load_static(&s_static)) {
        if (wifi_static_valid(&s_static)) {
            esp_ip4_addr_t a = {.addr = s_static.ip}, g = {.addr = s_static.gw};
            ESP_LOGI(TAG, "static address " IPSTR ", gateway " IPSTR, IP2STR(&a), IP2STR(&g));
        } else {
            ESP_LOGW(TAG, "saved static address is invalid: using DHCP");
            s_static = (wifi_ipv4_t){0};
        }
    }

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
