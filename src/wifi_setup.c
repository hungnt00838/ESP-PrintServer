// WiFi setup portal: an open access point "<HOSTNAME>-Setup", opened at run
// time next to the station whenever there is no network (wifi.c). A phone
// that joins it is sent to a page (captive portal: every DNS name answers
// 192.168.4.1) listing the networks around; the chosen SSID and password go
// to NVS and the station switches to them straight away.
//
// The page is served by the AirPrint web server on port 80 (its
// http_handler, see airprint_esp.c): while the portal is open, every HTTP
// request goes to wifi_setup_http().
#include "wifi_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "wifi_setup";

#define NVS_NS "wifi_cfg"
#define AP_IP "192.168.4.1"
#define PAGE_MAX 8192

static char s_ap_name[33];
static volatile bool s_open;
static bool s_netif_made, s_dns_started;

// ---------------------------------------------------------------------------
// saved credentials

bool wifi_setup_load(char *ssid, size_t ssid_n, char *pass, size_t pass_n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_n) == ESP_OK && ssid[0];
    if (ok && nvs_get_str(h, "pass", pass, &pass_n) != ESP_OK)
        pass[0] = 0;
    nvs_close(h);
    return ok;
}

static esp_err_t save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, "pass", pass);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// DNS: while the portal is open, every query gets an A record for it

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        if (sock >= 0)
            close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (!s_open || n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] == 0)
            continue;   // portal closed, too short, a response, or no question
        // end of the first question: QNAME labels, then QTYPE + QCLASS
        int p = 12;
        while (p < n && buf[p])
            p += buf[p] + 1;
        p += 5;
        if (p > n)
            continue;
        buf[2] = 0x81;  // response, recursion desired
        buf[3] = 0x80;  // recursion available, no error
        buf[5] = 1;     // one question
        buf[6] = 0, buf[7] = 1;     // one answer
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t ans[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(buf + p, ans, sizeof(ans));
        sendto(sock, buf, p + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
}

// ---------------------------------------------------------------------------
// open / close

esp_err_t wifi_setup_open(const char *hostname)
{
    snprintf(s_ap_name, sizeof(s_ap_name), "%.26s-Setup", hostname);
    if (!s_netif_made) {
        esp_netif_create_default_wifi_ap();
        s_netif_made = true;
    }
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ap_name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    // APSTA: the station keeps retrying, and scans for the list on the page
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK)
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "access point: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_dns_started) {
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
        s_dns_started = true;
    }
    s_open = true;
    ESP_LOGW(TAG, "setup portal: join WiFi \"%s\" and open http://" AP_IP "/", s_ap_name);
    return ESP_OK;
}

void wifi_setup_close(void)
{
    s_open = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup portal closed");
}

const char *wifi_setup_ap_name(void)
{
    return s_open ? s_ap_name : NULL;
}

// ---------------------------------------------------------------------------
// web page

typedef struct {
    char *buf;
    size_t len;
} page_t;

static void put(page_t *p, const char *s)
{
    size_t n = strlen(s);
    if (p->len + n < PAGE_MAX) {
        memcpy(p->buf + p->len, s, n);
        p->len += n;
        p->buf[p->len] = 0;
    }
}

static void html_escape(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        const char *r = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '"' ? "&quot;" : NULL;
        if (r) {
            strcpy(out + o, r);
            o += strlen(r);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Cài đặt Wi-Fi</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:420px;margin:0 auto;padding:16px;background:#f5f5f7;color:#111}"
    "h1{font-size:22px}label{display:block;margin:14px 0 6px;font-weight:600}"
    "select,input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;border:1px solid #bbb;border-radius:10px;background:#fff}"
    "button{margin-top:20px;width:100%;padding:14px;font-size:17px;border:0;border-radius:10px;background:#0a7cff;color:#fff}"
    "p{color:#555;line-height:1.4}.row{display:flex;gap:8px;align-items:center;margin-top:8px}"
    ".row input{width:auto}</style></head><body>";

static void send_page(airprint_http_t *h, page_t *p)
{
    put(p, "</body></html>");
    airprint_http_send_ex(h, "200 OK", "text/html; charset=utf-8", "Cache-Control: no-store\r\n", p->buf, p->len);
}

static void page_form(airprint_http_t *h, page_t *p)
{
    put(p, "<h1>Cài đặt Wi-Fi cho máy in</h1>"
           "<p>Chọn Wi-Fi nhà bạn (2.4 GHz) và nhập mật khẩu. Máy in sẽ chuyển sang mạng đó ngay.</p>"
           "<form method=post action=/save><label for=s>Mạng Wi-Fi</label><select id=s name=ssid>");
    wifi_scan_config_t sc = {.show_hidden = false};
    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_num(&count) == ESP_OK && count) {
        if (count > 25)
            count = 25;
        aps = calloc(count, sizeof(*aps));
        if (!aps || esp_wifi_scan_get_ap_records(&count, aps) != ESP_OK)
            count = 0;
    }
    char esc[200], line[520];
    for (int i = 0; i < count; i++) {
        if (!aps[i].ssid[0])
            continue;
        bool dup = false;   // one entry per name (the strongest comes first)
        for (int j = 0; j < i && !dup; j++)
            dup = !strcmp((char *)aps[j].ssid, (char *)aps[i].ssid);
        if (dup)
            continue;
        html_escape(esc, sizeof(esc), (char *)aps[i].ssid);
        snprintf(line, sizeof(line), "<option value=\"%s\">%s (%d dBm)%s</option>", esc, esc, aps[i].rssi,
                 aps[i].authmode == WIFI_AUTH_OPEN ? " - mở" : "");
        put(p, line);
    }
    free(aps);
    put(p, "<option value=''>Mạng khác (nhập tên bên dưới)...</option></select>"
           "<label for=o>Hoặc nhập tên Wi-Fi</label><input id=o name=other maxlength=32 autocapitalize=off>"
           "<label for=p>Mật khẩu</label><input id=p name=pass type=password maxlength=64>"
           "<div class=row><input type=checkbox id=v onclick=\"p.type=this.checked?'text':'password'\">"
           "<span>Hiện mật khẩu</span></div>"
           "<button type=submit>Lưu và kết nối</button></form>"
           "<p><a href=/>Quét lại danh sách Wi-Fi</a></p>");
    send_page(h, p);
}

static int hexval(char c)
{
    return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

// value of key in an application/x-www-form-urlencoded body, decoded
static void form_value(const char *body, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = body; p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=')
            continue;
        p += kl + 1;
        size_t o = 0;
        for (; *p && *p != '&' && o + 1 < n; p++) {
            if (*p == '+')
                out[o++] = ' ';
            else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
                p += 2;
            } else
                out[o++] = *p;
        }
        out[o] = 0;
        return;
    }
}

typedef struct {
    char ssid[33], pass[65];
} creds_t;

// after the reply has gone out: the station's channel change drops the phone
static void switch_task(void *arg)
{
    creds_t *c = arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    wifi_use_network(c->ssid, c->pass);
    free(c);
    vTaskDelete(NULL);
}

static void page_save(airprint_http_t *h, page_t *p)
{
    char body[512];
    int got = 0, r;
    while (got < (int)sizeof(body) - 1 && (r = airprint_http_read_body(h, body + got, sizeof(body) - 1 - got)) > 0)
        got += r;
    body[got] = 0;
    creds_t *c = calloc(1, sizeof(*c));
    char other[33];
    if (!c) {
        airprint_http_send(h, "500 Internal Server Error", "text/plain", "", 0);
        return;
    }
    form_value(body, "ssid", c->ssid, sizeof(c->ssid));
    form_value(body, "other", other, sizeof(other));
    form_value(body, "pass", c->pass, sizeof(c->pass));
    if (other[0])
        strcpy(c->ssid, other);
    if (!c->ssid[0] || (c->pass[0] && strlen(c->pass) < 8)) {
        free(c);
        put(p, "<h1>Chưa lưu</h1><p>Hãy chọn một mạng Wi-Fi; mật khẩu Wi-Fi phải có ít nhất 8 ký tự.</p>"
               "<p><a href=/>Quay lại</a></p>");
        send_page(h, p);
        return;
    }
    esp_err_t err = save(c->ssid, c->pass);
    char esc[200], msg[700];
    html_escape(esc, sizeof(esc), c->ssid);
    if (err == ESP_OK)
        snprintf(msg, sizeof(msg),
                 "<h1>Đã lưu</h1><p>Máy in đang kết nối vào <b>%s</b>. Màn hình máy in sẽ hiện địa chỉ IP khi xong.</p>"
                 "<p>Hãy chuyển điện thoại về Wi-Fi nhà bạn. Nếu sai mật khẩu, Wi-Fi cài đặt <b>%s</b> sẽ mở lại.</p>",
                 esc, s_ap_name);
    else
        snprintf(msg, sizeof(msg), "<h1>Lỗi</h1><p>Không lưu được (%s).</p><p><a href=/>Thử lại</a></p>",
                 esp_err_to_name(err));
    put(p, msg);
    send_page(h, p);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved a network, switching to it");
        if (xTaskCreate(switch_task, "wifi_switch", 3072, c, 5, NULL) != pdPASS)
            free(c);
    } else {
        free(c);
    }
}

bool wifi_setup_http(airprint_http_t *h, const char *method, const char *path)
{
    if (!s_open)
        return false;
    bool post = !strcmp(method, "POST");
    size_t plen = strcspn(path, "?#");
    if (!post && plen != 1) {
        // anything else (phones' captive portal checks included) goes to the page
        airprint_http_send_ex(h, "302 Found", "text/plain", "Location: http://" AP_IP "/\r\n", "", 0);
        return true;
    }
    page_t p = {.buf = malloc(PAGE_MAX), .len = 0};
    if (!p.buf) {
        airprint_http_send(h, "500 Internal Server Error", "text/plain", "", 0);
        return true;
    }
    p.buf[0] = 0;
    put(&p, PAGE_HEAD);
    if (post && !strncmp(path, "/save", 5))
        page_save(h, &p);
    else
        page_form(h, &p);
    free(p.buf);
    return true;
}
