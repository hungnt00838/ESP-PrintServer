// Firmware page on the main web server: /firmware (USB flash/print mode
// switch, WiFi update in MODE=debug). See fw_page.c.
#pragma once

#include <stdbool.h>

#include "airprint.h"

// Returns true once a response was sent.
bool fw_page_http(airprint_http_t *h, const char *method, const char *path);
