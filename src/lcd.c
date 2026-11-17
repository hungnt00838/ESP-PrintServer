// NV3023 colour status screen (ostb-xiaozhi-3st / "Lily Touch" board).
//
// Pins and init sequence from github.com/MKtuzi/ostb-xiaozhi-3st-esphome:
// SPI CLK 9, MOSI 10, CS 14, DC 8, RST 18, backlight 13, charging GPIO47.
// The battery ADC pin is not documented: with CONFIG_LCD_BATTERY_ADC_GPIO
// = -1 the ADC1 pins that are free on this board (GPIO1..3) are logged every
// 10 s so the right one can be picked.
//
// Layout (296 x 240, the 128 x 64 monochrome screens drawn at 2x):
//   y   0..41   header: firmware name, battery line (white on dark grey)
//   y  44..45   green rule
//   y  52..179  oled_ui status screen (host, IP, WiFi, printer, job)
//   y 188..235  touch buttons: NAP FW (USB flash mode) | CHE DO IN (print mode)
#include "lcd.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled.h"
#include "oled_ui.h"
#include "qrcodegen.h"
#include "sdkconfig.h"
#include "usb_mode.h"
#include "wifi_setup.h"

static const char *TAG = "lcd";

#define PIN_CLK 9
#define PIN_MOSI 10
#define PIN_CS 14
#define PIN_DC 8
#define PIN_RST 18
#define PIN_BL 13
#define PIN_CHARGING 47

#define W 296
#define H 240
#define CHUNK_ROWS 20
#define X0 20               // left margin of the 2x screens

#define FW_TITLE "AirPrint TS207"
#define FW_VERSION "v0.2.0"

// RGB565, byte-swapped for the SPI bus (high byte first)
#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#define SWAP(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
// red and blue may be swapped on this panel: the colours used here are
// greys and green, which look the same either way
static const uint16_t C_BLACK = 0x0000, C_WHITE = 0xffff;
#define C_HEADER SWAP(RGB(40, 40, 48))
#define C_GREEN SWAP(RGB(0, 200, 60))
#define C_GREY SWAP(RGB(110, 110, 110))

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_buf;
static oled_state_t s_state;
static uint8_t s_status[OLED_FB_SIZE], s_header[OLED_FB_SIZE];

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int s_batt_mv = -1, s_batt_pct = -1;
static bool s_charging;

// CST816 touch controller on I2C (SDA 12, SCL 11, address 0x15). Its
// coordinates already match this landscape frame (checked by tapping the four
// corners: x 41..281 left to right, y 10..224 top to bottom)
#define TOUCH_SDA 12
#define TOUCH_SCL 11
#define TOUCH_ADDR 0x15
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0
#define TOUCH_ACTIONS 1     // 0 while calibrating: taps are only logged
static i2c_master_dev_handle_t s_touch;

// the two buttons along the bottom
#define BTN_Y0 188
#define BTN_Y1 236
#define BTN_L0 6
#define BTN_L1 145
#define BTN_R0 151
#define BTN_R1 290

// WiFi setup screen (while the setup access point is open): a QR code that
// joins it, the steps, and the page address
#define QR_MAX_VERSION 3        // 29 x 29: the box fits left of the text
#define QR_MOD 5                // pixels per module
#define QR_QUIET 8              // white margin, pixels
#define QR_X 8
#define QR_Y 52
static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static int s_qr_size;           // 0: not made yet
static bool s_setup;

typedef struct {
    uint8_t bits[OLED_WIDTH];   // one OLED page: columns of 8 rows, at 2x
    int x, y, width;
    uint16_t color;
} text_line_t;
#define MAX_LINES 8
static text_line_t s_lines[MAX_LINES];
static int s_nlines;
// free analog pins on this board (the others drive the LCD, audio, I2C);
// GPIO17 is on ADC2
static const int PROBE_PINS[] = {1, 2, 3, 17};
#define NPROBE (int)(sizeof(PROBE_PINS) / sizeof(PROBE_PINS[0]))
static int s_batt_gpio = CONFIG_LCD_BATTERY_ADC_GPIO;  // -1: looking for it
static adc_oneshot_unit_handle_t s_adc2;

// ---------------------------------------------------------------------------
// panel

static void cmd(uint8_t c, const uint8_t *data, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, c, data, n);
}

static void cmd1(uint8_t c, uint8_t v)
{
    cmd(c, &v, 1);
}

static void panel_init(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x11, NULL, 0);     // sleep out
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd1(0xff, 0xa5);       // vendor commands unlocked
    cmd1(0x3e, 0x09);
    cmd1(0x3a, 0x05);       // 16 bit RGB565
    cmd1(0x82, 0x00);
    cmd1(0x98, 0x00);
    cmd1(0x63, 0x0f);
    cmd1(0x64, 0x0f);
    cmd1(0xb4, 0x34);
    cmd1(0xb5, 0x30);
    cmd1(0x83, 0x03);
    cmd1(0x86, 0x04);
    cmd1(0x87, 0x16);
    cmd1(0x88, 0x0a);
    cmd1(0x89, 0x27);
    cmd1(0x93, 0x63);
    cmd1(0x96, 0x81);
    cmd1(0xc3, 0x10);
    cmd1(0xe6, 0x00);
    cmd1(0x99, 0x01);
    static const uint8_t g1[16] = {0x09, 0x1D, 0x14, 0x0A, 0x11, 0x16, 0x38, 0x0B,
                                   0x08, 0x3E, 0x07, 0x0D, 0x16, 0x0F, 0x14, 0x05};
    static const uint8_t g2[16] = {0x04, 0x28, 0x0C, 0x11, 0x0B, 0x23, 0x45, 0x07,
                                   0x0A, 0x3B, 0x0D, 0x18, 0x14, 0x0F, 0x19, 0x08};
    cmd(0x70, g1, 16);
    cmd(0xa0, g2, 16);
    cmd1(0xff, 0x00);       // vendor commands locked
    // the panel is 240 x 296 portrait, mounted landscape: MV swaps rows and
    // columns so that the frame is 296 x 240 (MX picks which way is up)
    cmd1(0x36, CONFIG_LCD_MADCTL);
    cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    cmd(0x29, NULL, 0);     // display on
    vTaskDelay(pdMS_TO_TICKS(10));
}

static bool fb_bit(const uint8_t *fb, int col, int row)
{
    if (col < 0 || col >= OLED_WIDTH || row < 0 || row >= OLED_HEIGHT)
        return false;
    return (fb[(row / 8) * OLED_WIDTH + col] >> (row % 8)) & 1;
}

static void add_line(int x, int y, const char *text, uint16_t color)
{
    if (s_nlines >= MAX_LINES)
        return;
    text_line_t *l = &s_lines[s_nlines++];
    memset(l->bits, 0, sizeof(l->bits));
    l->width = oled_text(l->bits, 0, 0, text, 1);   // rows 0..7: page 0 only
    l->x = x;
    l->y = y;
    l->color = color;
}

static void make_setup_screen(void)
{
    const char *ap = wifi_setup_ap_name();
    char text[80];
    // the WiFi QR format phone cameras understand: join an open network
    snprintf(text, sizeof(text), "WIFI:T:nopass;S:%s;;", ap ? ap : "canon-ts207-Setup");
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    if (qrcodegen_encodeText(text, tmp, s_qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                             qrcodegen_Mask_AUTO, true))
        s_qr_size = qrcodegen_getSize(s_qr);
    else
        ESP_LOGE(TAG, "QR code: text too long");
    s_nlines = 0;
    add_line(176, 58, "CAI WIFI", C_GREEN);
    add_line(176, 88, "1.Quet ma", C_WHITE);
    add_line(176, 106, "QR nay", C_WHITE);
    add_line(176, 134, "2.Chon va", C_WHITE);
    add_line(176, 152, "nhap mat", C_WHITE);
    add_line(176, 170, "khau WiFi", C_WHITE);
    add_line((W - 14 * 12) / 2, 220, "Mo 192.168.4.1", C_GREEN);
}

static bool setup_pixel(int x, int y, uint16_t *c)
{
    int box = s_qr_size * QR_MOD + 2 * QR_QUIET;
    if (s_qr_size && x >= QR_X && y >= QR_Y && x < QR_X + box && y < QR_Y + box) {
        int mx = x - QR_X - QR_QUIET, my = y - QR_Y - QR_QUIET;
        bool dark = mx >= 0 && my >= 0 && mx < s_qr_size * QR_MOD && my < s_qr_size * QR_MOD &&
                    qrcodegen_getModule(s_qr, mx / QR_MOD, my / QR_MOD);
        *c = dark ? C_BLACK : C_WHITE;
        return true;
    }
    for (int i = 0; i < s_nlines; i++) {
        const text_line_t *l = &s_lines[i];
        if (y < l->y || y >= l->y + 16 || x < l->x || x >= l->x + 2 * l->width)
            continue;
        int col = (x - l->x) / 2, row = (y - l->y) / 2;
        if (col < OLED_WIDTH && (l->bits[col] >> row) & 1) {
            *c = l->color;
            return true;
        }
    }
    return false;
}

static text_line_t s_btn_text[2];

static void make_buttons(void)
{
    const char *labels[2] = {"NAP FW", "CHE DO IN"};
    int x0[2] = {BTN_L0, BTN_R0}, x1[2] = {BTN_L1, BTN_R1};
    for (int i = 0; i < 2; i++) {
        text_line_t *l = &s_btn_text[i];
        memset(l->bits, 0, sizeof(l->bits));
        l->width = oled_text(l->bits, 0, 0, labels[i], 1);
        l->x = (x0[i] + x1[i]) / 2 - l->width;     // drawn at 2x
        l->y = (BTN_Y0 + BTN_Y1) / 2 - 7;
    }
}

static bool button_pixel(int x, int y, uint16_t *c)
{
    for (int i = 0; i < 2; i++) {
        int x0 = i ? BTN_R0 : BTN_L0, x1 = i ? BTN_R1 : BTN_L1;
        if (x < x0 || x >= x1)
            continue;
        bool active = i == 0 ? usb_mode_flash() : !usb_mode_flash();
        uint16_t fill = active ? C_GREEN : C_BLACK, ink = active ? C_BLACK : C_WHITE;
        bool edge = y < BTN_Y0 + 2 || y >= BTN_Y1 - 2 || x < x0 + 2 || x >= x1 - 2;
        if (edge) {
            *c = active ? C_GREEN : C_GREY;
            return true;
        }
        const text_line_t *l = &s_btn_text[i];
        if (y >= l->y && y < l->y + 16 && x >= l->x && x < l->x + 2 * l->width) {
            int col = (x - l->x) / 2, row = (y - l->y) / 2;
            if ((l->bits[col] >> row) & 1) {
                *c = ink;
                return true;
            }
        }
        *c = fill;
        return true;
    }
    return false;
}

static void touch_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t b;
    if (i2c_new_master_bus(&bus, &b) != ESP_OK) {
        ESP_LOGW(TAG, "touch: no I2C bus");
        return;
    }
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TOUCH_ADDR,
                               .scl_speed_hz = 100000};
    if (i2c_master_bus_add_device(b, &dev, &s_touch) != ESP_OK) {
        s_touch = NULL;
        return;
    }
    // keep it awake (it stops answering I2C when it sleeps)
    uint8_t no_sleep[2] = {0xfe, 0x01};
    i2c_master_transmit(s_touch, no_sleep, 2, 50);
}

// true while a finger is down, with its position in this frame
static bool touch_read(int *lx, int *ly)
{
    if (!s_touch)
        return false;
    uint8_t reg = 0x02, d[5];
    if (i2c_master_transmit_receive(s_touch, &reg, 1, d, 5, 50) != ESP_OK || (d[0] & 0x0f) == 0)
        return false;
    int tx = ((d[1] & 0x0f) << 8) | d[2], ty = ((d[3] & 0x0f) << 8) | d[4];
    int x = TOUCH_SWAP_XY ? ty : tx, y = TOUCH_SWAP_XY ? tx : ty;
    if (TOUCH_MIRROR_X)
        x = W - 1 - x;
    if (TOUCH_MIRROR_Y)
        y = H - 1 - y;
    static int64_t last_log;
    int64_t now = esp_timer_get_time();
    if (now - last_log > 300000) {
        ESP_LOGI(TAG, "touch raw %d,%d -> %d,%d", tx, ty, x, y);
        last_log = now;
    }
    *lx = x;
    *ly = y;
    return true;
}

static uint16_t pixel(int x, int y)
{
    if (s_setup && y >= 46) {
        uint16_t c;
        return setup_pixel(x, y, &c) ? c : C_BLACK;
    }
    if (y < 42)
        return fb_bit(s_header, (x - X0) / 2, y / 2) ? C_WHITE : C_HEADER;
    if (y < 44)
        return C_HEADER;
    if (y < 46)
        return C_GREEN;
    if (y >= 52 && y < 52 + 2 * OLED_HEIGHT)
        return x >= X0 && fb_bit(s_status, (x - X0) / 2, (y - 52) / 2) ? C_WHITE : C_BLACK;
    if (y >= BTN_Y0 && y < BTN_Y1) {
        uint16_t c;
        if (button_pixel(x, y, &c))
            return c;
        return C_BLACK;
    }
    if (0) {
        // battery bar: 2 px outline, fill by percent
        bool edge = y < 198 || y >= 222 || x < X0 + 2 || x >= W - X0 - 2;
        if (edge)
            return C_WHITE;
        if (s_batt_pct < 0)
            return C_BLACK;
        int fill = X0 + 4 + (W - 2 * X0 - 8) * s_batt_pct / 100;
        return x < fill && y >= 200 && y < 220 ? (s_batt_pct <= 15 ? C_GREY : C_GREEN) : C_BLACK;
    }
    return C_BLACK;
}

static void flush(void)
{
    static const uint8_t caset[4] = {0, 0, (W - 1) >> 8, (W - 1) & 0xff};
    for (int y0 = 0; y0 < H; y0 += CHUNK_ROWS) {
        int rows = H - y0 < CHUNK_ROWS ? H - y0 : CHUNK_ROWS;
        cmd(0x2a, caset, 4);    // also waits for the previous chunk's DMA
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < W; x++)
                s_buf[y * W + x] = pixel(W - 1 - x, y0 + y);    // panel shows X mirrored
        uint8_t raset[4] = {y0 >> 8, y0 & 0xff, (y0 + rows - 1) >> 8, (y0 + rows - 1) & 0xff};
        cmd(0x2b, raset, 4);
        esp_lcd_panel_io_tx_color(s_io, 0x2c, s_buf, (size_t)rows * W * 2);
    }
}

// ---------------------------------------------------------------------------
// battery

static int adc_mv(int gpio)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(gpio, &unit, &ch) != ESP_OK)
        return -1;
    adc_oneshot_unit_handle_t adc = unit == ADC_UNIT_1 ? s_adc : s_adc2;
    if (!adc)
        return -1;
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(adc, ch, &cc);
    int sum = 0, raw;
    for (int i = 0; i < 16; i++) {
        if (adc_oneshot_read(adc, ch, &raw) != ESP_OK)
            return -1;
        sum += raw;
    }
    raw = sum / 16;
    int mv = raw * 3300 / 4095;
    if (s_cali && unit == ADC_UNIT_1)
        adc_cali_raw_to_voltage(s_cali, raw, &mv);
    return mv;
}

static void battery_init(void)
{
    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_CHARGING, .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        s_adc = NULL;
        return;
    }
    adc_oneshot_unit_init_cfg_t u2 = {.unit_id = ADC_UNIT_2};
    if (adc_oneshot_new_unit(&u2, &s_adc2) != ESP_OK)
        s_adc2 = NULL;
    adc_cali_curve_fitting_config_t cal = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                           .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK)
        s_cali = NULL;
}

// a reading through the divider that is a plausible Li-ion voltage
static bool plausible(int mv)
{
    int v = mv * CONFIG_LCD_BATTERY_DIVIDER_X100 / 100;
    return v >= 3000 && v <= 4400;
}

static void battery_update(int64_t now_us)
{
    s_charging = gpio_get_level(PIN_CHARGING) == CONFIG_LCD_CHARGING_LEVEL;
    if (!s_adc)
        return;
    static int64_t next_probe;
    if (s_batt_gpio < 0 && now_us >= next_probe) {
        // not documented for this board: take the first free analog pin
        // that reads like a battery, log them all
        next_probe = now_us + 10 * 1000000LL;
        char msg[96];
        int n = 0;
        for (int i = 0; i < NPROBE; i++) {
            int mv = adc_mv(PROBE_PINS[i]);
            n += snprintf(msg + n, sizeof(msg) - n, " GPIO%d=%d", PROBE_PINS[i], mv);
            if (s_batt_gpio < 0 && mv > 0 && plausible(mv))
                s_batt_gpio = PROBE_PINS[i];
        }
        ESP_LOGW(TAG, "battery probe (mV):%s, charging=%d -> %s GPIO%d", msg, s_charging,
                 s_batt_gpio >= 0 ? "using" : "none yet,", s_batt_gpio);
    }
    if (s_batt_gpio < 0)
        return;
    int mv = adc_mv(s_batt_gpio);
    if (mv > 0) {
        mv = mv * CONFIG_LCD_BATTERY_DIVIDER_X100 / 100;
        // smooth: the reading jumps with the WiFi radio's current draw
        s_batt_mv = s_batt_mv < 0 ? mv : (s_batt_mv * 7 + mv) / 8;
        int pct = (s_batt_mv - 3300) * 100 / (4150 - 3300);
        s_batt_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    }
}

// ---------------------------------------------------------------------------

static void render_header(void)
{
    // indented: the panel's rounded corners hide the first columns
    oled_clear(s_header);
    int x = oled_text(s_header, 4, 2, FW_TITLE, 1);
    oled_text(s_header, x + 6, 2, FW_VERSION, 1);
    char line[32];
    if (s_batt_pct >= 0)
        snprintf(line, sizeof(line), "Pin %d%%%s", s_batt_pct, s_charging ? "  Dang sac" : "");
    else
        snprintf(line, sizeof(line), "Pin --%%%s", s_charging ? "  Dang sac" : "");
    oled_text(s_header, 4, 12, line, 1);
}

static void lcd_task(void *arg)
{
    panel_init();
    make_buttons();
    touch_init();
    gpio_set_level(PIN_BL, 1);
    static uint8_t last_status[OLED_FB_SIZE], last_header[OLED_FB_SIZE];
    memset(last_status, 0xaa, sizeof(last_status));
    int last_pct = -2;
    bool last_setup = false;
    int64_t changed_at = esp_timer_get_time();
    bool lit = true;
    const int64_t off_us = (int64_t)CONFIG_LCD_BACKLIGHT_OFF_MIN * 60 * 1000000;
    bool down = false;
    int tap_x = 0, tap_y = 0;
    int64_t next_update = 0;
    while (1) {
        // touch every 30 ms; the screen once a second (or after a tap)
        vTaskDelay(pdMS_TO_TICKS(30));
        int64_t now = esp_timer_get_time();
        int tx, ty;
        bool d = touch_read(&tx, &ty);
        if (d) {
            tap_x = tx;
            tap_y = ty;
        }
        if (down && !d) {
            // a tap ended: the first one on a dark screen only wakes it
            bool was_dark = !lit;
            changed_at = now;
            next_update = 0;
            if (TOUCH_ACTIONS && !was_dark && !s_setup && tap_y >= BTN_Y0 - 10) {
                if (tap_x >= BTN_L0 && tap_x < BTN_L1)
                    usb_mode_set(true);
                else if (tap_x >= BTN_R0 && tap_x < BTN_R1)
                    usb_mode_set(false);
            }
        }
        down = d;
        if (now < next_update)
            continue;
        next_update = now + 1000000;
        battery_update(now);
        oled_gather(&s_state);
        oled_ui_render(&s_state, s_status);
        if (usb_mode_flash()) {
            // USB flash mode replaces the printer part of the status screen
            oled_clear(s_status);
            oled_text(s_status, 4, 2, "NAP USB", 2);
            oled_text(s_status, 4, 24, "Cam cap USB vao may", 1);
            oled_text(s_status, 4, 34, "tinh de nap firmware", 1);
            oled_text(s_status, 4, 46, "Bam CHE DO IN o duoi", 1);
            oled_text(s_status, 4, 56, "de quay lai in", 1);
        }
        render_header();
        s_setup = wifi_setup_ap_name() != NULL;
        if (s_setup && !s_nlines)
            make_setup_screen();
        bool status_changed = memcmp(s_status, last_status, OLED_FB_SIZE) != 0 || s_setup != last_setup;
        if (s_setup)
            changed_at = now;   // keep the backlight on while setting up
        if (status_changed || s_charging)
            changed_at = now;
        if (status_changed || memcmp(s_header, last_header, OLED_FB_SIZE) || s_batt_pct != last_pct) {
            flush();
            memcpy(last_status, s_status, OLED_FB_SIZE);
            memcpy(last_header, s_header, OLED_FB_SIZE);
            last_pct = s_batt_pct;
            last_setup = s_setup;
        }
        // the backlight is most of the board's current: off when nothing
        // happens for a while (never while charging / on USB power)
        bool want = off_us == 0 || now - changed_at < off_us;
        if (want != lit) {
            gpio_set_level(PIN_BL, want);
            lit = want;
        }
    }
}

void lcd_start(const char *hostname)
{
    oled_set_hostname(hostname);
    gpio_config_t out = {.pin_bit_mask = (1ULL << PIN_RST) | (1ULL << PIN_BL), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_set_level(PIN_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_CLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = W * CHUNK_ROWS * 2,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return;
    }
    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 10 * 1000 * 1000,     // as the working ESPHome config
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO: %s", esp_err_to_name(err));
        return;
    }
    s_buf = heap_caps_malloc(W * CHUNK_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf) {
        ESP_LOGE(TAG, "no DMA memory for the line buffer");
        return;
    }
    battery_init();
    if (xTaskCreate(lcd_task, "lcd", 6144, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "cannot start the screen task");
    else
        ESP_LOGI(TAG, "NV3023 %dx%d status screen", W, H);
}
