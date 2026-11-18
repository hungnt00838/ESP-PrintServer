// Firmware page on the main web server (port 80): http://<HOSTNAME>.local/firmware
//
// The printer takes the board's USB port over (USB host), so the port can no
// longer be used for flashing. From this page:
//   - switch the USB port to flash mode (usb_mode.c: a restart that keeps the
//     USB-Serial/JTAG; web flasher / esptool then work over the cable), and back
//   - in MODE=debug, upload a firmware.bin over WiFi (the debug server's
//     POST :8080/ota, which allows this page's origin)
#include "fw_page.h"

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_config.h"
#include "sdkconfig.h"
#include "usb_mode.h"
#include "usb_printer.h"

static const char *TAG = "fw_page";

#if CONFIG_LCD_NV3023
#define BOARD_NAME "Lily Touch (ESP32-S3, màn hình NV3023)"
#define BOARD_NOTE "Mạch chỉ có 1 cổng USB: máy in dùng nó. Muốn nạp qua cáp, chuyển sang chế độ nạp trước."
#else
#define BOARD_NAME "ESP32-S3 N16R8 (không màn hình)"
#define BOARD_NOTE "Cổng COM/TTL luôn nạp được. Chế độ nạp chỉ cần khi muốn nạp qua cổng USB/OTG."
#endif

static const char PAGE[] =
    "<!doctype html><html lang=vi><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Firmware</title><style>"
    ":root{--bg:#f4f5f7;--card:#fff;--fg:#1d2330;--mut:#667085;--line:#e2e5ea;--acc:#1a73e8;--warn:#b54708;--ok:#067647}"
    "@media(prefers-color-scheme:dark){:root{--bg:#121417;--card:#1c1f24;--fg:#e8eaed;--mut:#9aa0a6;--line:#30343a;--acc:#8ab4f8;--warn:#fdb022;--ok:#47cd89}}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}"
    "main{max-width:560px;margin:0 auto;padding:16px}h1{font-size:20px;margin:8px 0 16px}"
    "section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:14px}"
    "h2{font-size:16px;margin:0 0 10px}dl{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:0}"
    "dt{color:var(--mut)}dd{margin:0;word-break:break-word}p{margin:8px 0}.mut{color:var(--mut);font-size:14px}"
    "button{font:inherit;padding:9px 14px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer;margin:4px 6px 4px 0}"
    "button.pri{background:var(--acc);border-color:var(--acc);color:#fff}button:disabled{opacity:.5;cursor:default}"
    "input[type=file]{margin:6px 0;max-width:100%}progress{width:100%;height:10px}"
    "#msg{min-height:1.5em;font-weight:600}.warn{color:var(--warn)}.ok{color:var(--ok)}a{color:var(--acc)}"
    "</style></head><body><main><p><a href=/>&larr; Trang chính</a></p><h1>Firmware</h1>"
    "<section><h2>Thông tin</h2><dl id=info><dt>Đang tải…</dt><dd></dd></dl></section>"
    "<section><h2>Cổng USB</h2><p>Đang ở: <b id=mode>…</b></p><p class=mut id=note></p>"
    "<button class=pri id=toFlash>Chuyển sang chế độ nạp FW</button><button id=toPrint>Về chế độ in</button>"
    "<p class=mut>Mạch khởi động lại khoảng 5 giây. Ở chế độ nạp, máy in không hoạt động; cắm cáp vào máy tính rồi nạp "
    "(trang nạp web hoặc esptool). Nạp xong, hoặc rút nguồn cắm lại, mạch tự về chế độ in.</p></section>"
    "<section id=ota hidden><h2>Cập nhật qua Wi-Fi</h2><p class=mut>Chọn file <b>firmware.bin</b> (không phải file gộp "
    "merged.bin). Mạch kiểm tra file, ghi vào phân vùng còn lại, rồi khởi động lại. Nếu firmware mới không chạy được, "
    "mạch tự quay về bản cũ.</p><input type=file id=file accept=.bin><br><button class=pri id=up disabled>Tải lên và cài</button>"
    "<progress id=bar max=1 value=0 hidden></progress></section>"
    "<section id=noota hidden><h2>Cập nhật qua Wi-Fi</h2><p class=mut>Chỉ có khi firmware được build với MODE=debug.</p></section>"
    "<p id=msg></p></main><script>"
    "const $=id=>document.getElementById(id),msg=(t,c)=>{$('msg').textContent=t;$('msg').className=c||''};"
    "let st;"
    "async function load(){try{st=await(await fetch('/firmware/state',{cache:'no-store'})).json()}catch(e){return false}"
    "const rows=[['Mạch',st.board],['Phiên bản',st.version],['Build',st.built],['Phân vùng',st.partition],"
    "['Máy in',st.printer?'đã kết nối':'chưa kết nối']];"
    "const dl=$('info');dl.replaceChildren();for(const[k,v]of rows){const dt=document.createElement('dt'),dd=document.createElement('dd');"
    "dt.textContent=k;dd.textContent=v;dl.append(dt,dd)}"
    "$('mode').textContent=st.flash?'Chế độ nạp (USB-Serial/JTAG)':'Chế độ in (USB Host cho máy in)';"
    "$('note').textContent=st.note;$('toFlash').disabled=st.flash;$('toPrint').disabled=!st.flash;"
    "$('ota').hidden=!st.ota;$('noota').hidden=st.ota;return true}"
    "async function waitBack(t){msg(t,'warn');await new Promise(r=>setTimeout(r,4000));"
    "for(let i=0;i<60;i++){if(await load()){msg('Mạch đã chạy lại.','ok');return}await new Promise(r=>setTimeout(r,2000))}"
    "msg('Chưa thấy mạch trả lời. Tải lại trang sau ít phút.','warn')}"
    "async function setMode(m,q){if(!confirm(q))return;try{const r=await fetch('/firmware/usb?mode='+m,{method:'POST'});"
    "if(!r.ok)throw new Error(await r.text());waitBack('Đang khởi động lại…')}catch(e){msg('Lỗi: '+e.message,'warn')}}"
    "$('toFlash').onclick=()=>setMode('flash','Chuyển cổng USB sang chế độ nạp? Máy in sẽ ngừng hoạt động đến khi về chế độ in.');"
    "$('toPrint').onclick=()=>setMode('print','Về chế độ in?');"
    "$('file').onchange=()=>{$('up').disabled=!$('file').files.length};"
    "$('up').onclick=()=>{const f=$('file').files[0];if(!f||!confirm('Cài '+f.name+' ('+Math.round(f.size/1024)+' KB)?'))return;"
    "const x=new XMLHttpRequest();x.open('POST','http://'+location.hostname+':8080/ota');"
    "x.setRequestHeader('Content-Type','application/octet-stream');"
    "$('bar').hidden=false;$('up').disabled=true;msg('Đang tải lên…');"
    "x.upload.onprogress=e=>{if(e.lengthComputable)$('bar').value=e.loaded/e.total};"
    "x.onload=()=>{if(x.status==200)waitBack('Đã cài, đang khởi động lại…');else{msg('Lỗi: '+x.responseText,'warn');$('up').disabled=false}};"
    "x.onerror=()=>{msg('Không gửi được (máy chủ nạp ở cổng 8080 không trả lời).','warn');$('up').disabled=false};"
    "x.send(f)};"
    "load().then(ok=>{if(!ok)msg('Không đọc được trạng thái.','warn')});"
    "</script></body></html>";

static void switch_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500)); // let the response go out first
    usb_mode_set(arg != NULL);
    vTaskDelete(NULL);
}

bool fw_page_http(airprint_http_t *h, const char *method, const char *path)
{
    if (strncmp(path, "/firmware", 9) != 0)
        return false;
    const char *rest = path + 9;
    if ((!*rest || !strcmp(rest, "/")) && !strcmp(method, "GET")) {
        airprint_http_send_ex(h, "200 OK", "text/html; charset=utf-8", "Cache-Control: no-store\r\n", PAGE,
                              sizeof(PAGE) - 1);
        return true;
    }
    if (!strcmp(rest, "/state") && !strcmp(method, "GET")) {
        const esp_app_desc_t *d = esp_app_get_description();
        const esp_partition_t *run = esp_ota_get_running_partition();
        char json[512];
        int n = snprintf(json, sizeof(json),
                         "{\"board\":\"%s\",\"note\":\"%s\",\"version\":\"%s\",\"built\":\"%s %s\","
                         "\"partition\":\"%s\",\"flash\":%s,\"printer\":%s,\"ota\":%s}",
                         BOARD_NAME, BOARD_NOTE, d->version, d->date, d->time, run ? run->label : "?",
                         usb_mode_flash() ? "true" : "false", usb_printer_connected() ? "true" : "false",
                         strcmp(MODE, "debug") == 0 ? "true" : "false");
        airprint_http_send_ex(h, "200 OK", "application/json", "Cache-Control: no-store\r\n", json, (size_t)n);
        return true;
    }
    if (!strncmp(rest, "/usb", 4) && !strcmp(method, "POST")) {
        bool flash;
        if (!strcmp(rest, "/usb?mode=flash"))
            flash = true;
        else if (!strcmp(rest, "/usb?mode=print"))
            flash = false;
        else {
            airprint_http_send(h, "400 Bad Request", "text/plain", "mode=flash or mode=print\n", 25);
            return true;
        }
        if (flash && usb_printer_job_active()) {
            static const char busy[] = "đang in, thử lại sau\n";
            airprint_http_send(h, "409 Conflict", "text/plain; charset=utf-8", busy, sizeof(busy) - 1);
            return true;
        }
        if (flash == usb_mode_flash()) {
            airprint_http_send(h, "200 OK", "text/plain", "already\n", 8);
            return true;
        }
        ESP_LOGW(TAG, "web: switching the USB port to %s mode", flash ? "flash" : "print");
        airprint_http_send(h, "200 OK", "text/plain", "restarting\n", 11);
        xTaskCreate(switch_task, "usb_switch", 2560, flash ? (void *)1 : NULL, 5, NULL);
        return true;
    }
    return false;
}
