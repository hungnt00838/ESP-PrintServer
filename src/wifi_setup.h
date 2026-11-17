// WiFi setup portal: an open access point "<hostname>-Setup" whose page
// (served by the AirPrint web server) lists the networks around and saves
// the one picked, with its password, in NVS. Opened and closed at run time
// by wifi.c; no restart is involved.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "airprint.h"
#include "esp_err.h"

// The network saved from the setup page; false when there is none.
bool wifi_setup_load(char *ssid, size_t ssid_n, char *pass, size_t pass_n);
// Opens / closes the access point (WiFi must be started).
esp_err_t wifi_setup_open(const char *hostname);
void wifi_setup_close(void);
// The setup network's name while the portal is open, else NULL.
const char *wifi_setup_ap_name(void);
// While the portal is open, every HTTP request goes here (from the AirPrint
// web server's http_handler). Returns true once a response was sent.
bool wifi_setup_http(airprint_http_t *h, const char *method, const char *path);

// wifi.c: join this network now (saved from the setup page).
void wifi_use_network(const char *ssid, const char *pass);
