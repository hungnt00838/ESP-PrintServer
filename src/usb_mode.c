#include "usb_mode.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_mode";

#include "sdkconfig.h"

// Lily Touch: volume down / volume up; -1 on boards without them
#define BTN_A CONFIG_USB_MODE_BUTTON_A_GPIO
#define BTN_B CONFIG_USB_MODE_BUTTON_B_GPIO
#define HOLD_MS 2000
#define FLASH_MAGIC 0x464c5348u // "FLSH"

// survives esp_restart(), not a power cycle or the reset after a flash
static RTC_NOINIT_ATTR uint32_t s_request;
static bool s_flash;

static bool pressed(void)
{
    return (BTN_A >= 0 && gpio_get_level(BTN_A) == 0) || (BTN_B >= 0 && gpio_get_level(BTN_B) == 0);
}

static void button_task(void *arg)
{
    // a button already down at boot (the one that switched) must be let go first
    while (pressed())
        vTaskDelay(pdMS_TO_TICKS(50));
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        held = pressed() ? held + 50 : 0;
        if (held >= HOLD_MS)
            usb_mode_set(!s_flash);
    }
}

void usb_mode_init(void)
{
    s_flash = esp_reset_reason() == ESP_RST_SW && s_request == FLASH_MAGIC;
    s_request = 0;
    uint64_t mask = (BTN_A >= 0 ? 1ULL << BTN_A : 0) | (BTN_B >= 0 ? 1ULL << BTN_B : 0);
    if (!mask)
        return; // no switch buttons: always print mode
    gpio_config_t in = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    if (s_flash)
        ESP_LOGW(TAG, "USB flash mode: the USB port stays USB-Serial/JTAG (hold a volume button 2 s for print mode)");
    xTaskCreate(button_task, "usb_mode", 2560, NULL, 4, NULL);
}

void usb_mode_set(bool flash)
{
    if (flash == s_flash)
        return;
    s_request = flash ? FLASH_MAGIC : 0;
    ESP_LOGW(TAG, "restarting into %s mode", flash ? "USB flash" : "print");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

bool usb_mode_flash(void)
{
    return s_flash;
}
