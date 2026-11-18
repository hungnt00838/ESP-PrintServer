// WiFi station: the network saved from the setup portal (wifi_setup.c),
// else the one from .env. With neither, or one it cannot join within 45 s,
// opens the setup portal and returns an error (no AirPrint until restart).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Starts connecting in the background (reconnects forever).
esp_err_t wifi_start(const char *hostname);
bool wifi_connected(void);
// A network is set (saved or from .env).
bool wifi_configured(void);
// The station's IPv4 address as text; false (and "") while not connected.
bool wifi_get_ip(char *buf, size_t n);
// Signal of the access point in dBm, 0 when unknown.
int wifi_rssi(void);

// IPv4 settings, each address in network byte order (esp_ip4_addr_t.addr).
// ip == 0 means DHCP.
typedef struct {
    uint32_t ip, mask, gw, dns;
} wifi_ipv4_t;

// The static address saved on the board (false = DHCP).
bool wifi_static_get(wifi_ipv4_t *out);
// A usable static address: contiguous mask, gateway another host of the subnet.
bool wifi_static_valid(const wifi_ipv4_t *cfg);
// Saves a static address (NULL or ip == 0: back to DHCP). Checked first:
// ESP_ERR_INVALID_ARG for a bad mask, or a gateway outside the subnet.
// Takes effect at the next restart. Writes flash: not from a task whose
// stack is in PSRAM (the HTTP connection threads).
esp_err_t wifi_static_set(const wifi_ipv4_t *cfg);
// What the station uses now (DHCP lease or static); false while not connected.
bool wifi_current_ipv4(wifi_ipv4_t *out);
