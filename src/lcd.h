// Colour status screen for the ostb-xiaozhi-3st board (sold as "Lily Touch"):
// an NV3023 296 x 240 SPI TFT. Shows the firmware name, the battery and the
// same status as the OLED screen (oled_ui), drawn twice as large.
#pragma once

#include <stdbool.h>

void lcd_start(const char *hostname);
