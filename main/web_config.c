/*
 * Web configuration page. See web_config.h.
 *
 * One embedded HTML page + a handful of JSON endpoints. The page is available
 * on the LAN (STA IP) and from the setup AP. Settings are persisted through
 * the settings module; volume/brightness apply immediately, Wi-Fi changes
 * reboot the device.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "backlight.h"
#include "clock_ui.h"
#include "esp_lvgl_port.h"
#include "ui_perf.h"
#include "main.h"
#include "radio.h"
#include "settings.h"
#include "wifi_app.h"
#include "web_config.h"

static const char *TAG = "webcfg";

static httpd_handle_t s_server;
static bool s_started;
static bool s_ap_mode;
static int64_t s_ap_deadline_us;
static esp_timer_handle_t s_ap_timer;

#define AP_MODE_TIMEOUT_S 300

/* ------------------------------- HTML ------------------------------ */

static const char s_html[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>桌面时钟 · 设置</title><style>"
"body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0b1b33;color:#dbe7f5;margin:0;padding:16px;max-width:520px;margin:auto}"
"h1{font-size:20px;margin:8px 0 16px;color:#fff}h2{font-size:15px;color:#00c2ff;margin:20px 0 8px;border-bottom:1px solid #1d3a5f;padding-bottom:6px}"
".row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:8px 0}"
".row label{color:#8fb0d0;font-size:14px}.val{color:#00c2ff;font-size:14px;min-width:48px;text-align:right}"
"input[type=range]{flex:1}.txt{width:100%;box-sizing:border-box;background:#13294a;border:1px solid #1d3a5f;color:#fff;border-radius:8px;padding:10px;font-size:14px;margin:6px 0}"
"button{background:#13395c;color:#dbe7f5;border:1px solid #1d4a75;border-radius:8px;padding:10px 14px;font-size:14px;margin:4px 6px 4px 0}"
"button:active{background:#1d4a75}button.warn{background:#5c1f22;border-color:#7a2b2e}"
"select{width:100%;background:#13294a;color:#fff;border:1px solid #1d3a5f;border-radius:8px;padding:10px;font-size:14px;margin:6px 0}"
"#st{font-size:13px;color:#8fb0d0;line-height:1.7}.ok{color:#24d17e}.bad{color:#ff5c5c}"
"</style></head><body>"
"<h1>桌面时钟 · 设置</h1>"
"<div id='st'>加载中…</div>"
"<h2>网络</h2>"
"<button onclick='scan()'>扫描 Wi-Fi</button>"
"<select id='list' size='1' onchange=\"if(this.value)document.getElementById('ssid').value=this.value\"><option value=''>— 点“扫描 Wi-Fi”选择热点 —</option></select>"
"<input class='txt' id='ssid' placeholder='Wi-Fi 名称 (SSID)'>"
"<input class='txt' id='pass' type='password' placeholder='Wi-Fi 密码'>"
"<button onclick='saveWifi()'>保存 Wi-Fi 并重启</button>"
"<h2>声音与显示</h2>"
"<div class='row'><label>音量</label><input type='range' id='vol' min='0' max='100' oninput='v(this.value)' onchange='saveSett()'><span class='val' id='volv'></span></div>"
"<div class='row'><label>亮度</label><input type='range' id='bri' min='5' max='100' oninput='b(this.value)' onchange='saveSett()'><span class='val' id='briv'></span></div>"
"<div class='row'><label>自动熄屏</label></div>"
"<select id='sleep' onchange='saveSett()'>"
"<option value='0'>从不</option><option value='1'>1 分钟</option><option value='5'>5 分钟</option>"
"<option value='10'>10 分钟</option><option value='30'>30 分钟</option></select>"
"<h2>电视频道表</h2>"
"<input class='txt' id='tvurl' placeholder='http://.../interface.m3u'>"
"<button onclick='saveSett()'>保存频道表地址</button>"
"<h2>系统</h2>"
"<button onclick='apmode()'>进入配网热点模式</button>"
"<button onclick='post(\"/api/restart\",{})'>重启设备</button>"
"<button class='warn' onclick='if(confirm(\"确定恢复出厂设置?\"))post(\"/api/factory\",{})'>恢复出厂设置</button>"
"<script>"
"const $=id=>document.getElementById(id);"
"async function get(u){const r=await fetch(u);return await r.json()}"
"async function post(u,d){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});return await r.json()}"
"function v(x){$('volv').textContent=x;api('/api/settings',{volume:+x})}"
"function b(x){$('briv').textContent=x+'%';api('/api/settings',{brightness:+x})}"
"let t=null;function api(u,d){clearTimeout(t);t=setTimeout(()=>post(u,d),150)}"
"function saveSett(){post('/api/settings',{volume:+$('vol').value,brightness:+$('bri').value,sleep:+$('sleep').value,tvurl:$('tvurl').value})}"
"async function scan(){$('list').innerHTML='<option>扫描中…</option>';const d=await get('/api/scan');"
"$('list').innerHTML=d.list.map(a=>`<option value='${a.ssid}'>${a.ssid} (${a.rssi}dBm)</option>`).join('')||'<option>未发现热点</option>'}"
"function saveWifi(){const s=$('ssid').value.trim();if(!s){alert('请输入 Wi-Fi 名称');return}"
"post('/api/wifi',{ssid:s,pass:$('pass').value}).then(()=>{document.body.innerHTML='<h1>已保存，设备重启中…</h1><p>请把手机连回原来的 Wi-Fi。</p>'})}"
"function apmode(){if(confirm('设备将开启热点 DesktopClock-Setup，5 分钟后自动重启。继续?'))"
"post('/api/apmode',{}).then(()=>{document.body.innerHTML='<h1>配网模式已开启</h1><p>用手机连接 Wi-Fi: DesktopClock-Setup，然后打开 http://192.168.4.1</p>'})}"
"async function tick(){const d=await get('/api/status');"
"$('st').innerHTML=`网络: <b class='${d.state==2?'ok':'bad'}'>${d.state==2?'已连接':'未连接'}</b> ${d.ssid} · IP ${d.ip||'-'} · 信号 ${d.rssi}dBm<br>内存: 内部 ${(d.heap/1024)|0}KB / PSRAM ${(d.psram/1024)|0}KB · 运行 ${(d.up/60)|0} 分钟`;"
"$('vol').value=d.vol;$('volv').textContent=d.vol;$('bri').value=d.bright;$('briv').textContent=d.bright+'%';"
"$('sleep').value=d.sleep;$('tvurl').value=d.tvurl}"
"tick();setInterval(tick,5000);"
"</script></body></html>";

/* ------------------------------ helpers ---------------------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static cJSON *recv_json(httpd_req_t *req)
{
    char buf[384];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return NULL;
    }
    buf[len] = '\0';
    return cJSON_Parse(buf);
}

static void restart_later(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void do_restart_soon(void)
{
    xTaskCreate(restart_later, "restart", 2048, NULL, 3, NULL);
}

/* ------------------------------ handlers --------------------------- */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req)
{
    const settings_t *c = settings_get();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "state", (int)wifi_app_state());
    cJSON_AddStringToObject(r, "ssid", c->wifi_ssid);
    cJSON_AddStringToObject(r, "ip", wifi_app_ip());
    cJSON_AddNumberToObject(r, "rssi", wifi_app_rssi());
    cJSON_AddNumberToObject(r, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "heapmin", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "heapbig", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r, "psrammin", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r, "up", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(r, "vol", c->volume);
    cJSON_AddNumberToObject(r, "bright", c->brightness);
    cJSON_AddNumberToObject(r, "sleep", c->sleep_min);
    cJSON_AddStringToObject(r, "tvurl", c->tv_url);
    cJSON_AddBoolToObject(r, "ap", s_ap_mode);
    ui_perf_snapshot_t pf;
    ui_perf_get(&pf);
    cJSON_AddNumberToObject(r, "uiframes", pf.frames);
    cJSON_AddNumberToObject(r, "uiframeus", pf.frame_us);
    cJSON_AddNumberToObject(r, "uiflushus", pf.flush_us);
    cJSON_AddNumberToObject(r, "uiareas", pf.areas);
    cJSON_AddNumberToObject(r, "uikpx", pf.px_per_s / 1000);
    return send_json(req, r);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    cJSON *r = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(r, "list");
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        if (num > 30) num = 30;
        wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(wifi_ap_record_t));
        if (recs != NULL) {
            uint16_t got = num;
            esp_wifi_scan_get_ap_records(&got, recs);
            for (int i = 0; i < got; i++) {
                if (recs[i].ssid[0] == '\0') {
                    continue;
                }
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "ssid", (const char *)recs[i].ssid);
                cJSON_AddNumberToObject(a, "rssi", recs[i].rssi);
                cJSON_AddItemToArray(list, a);
            }
            free(recs);
        }
    }
    return send_json(req, r);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    cJSON *j = recv_json(req);
    if (j == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    if (ssid == NULL || ssid[0] == '\0') {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid");
    }
    settings_set_wifi(ssid, pass ? pass : "");
    settings_save();
    ESP_LOGI(TAG, "wifi credentials saved (\"%s\"), restarting", ssid);
    cJSON_Delete(j);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    send_json(req, r);
    do_restart_soon();
    return ESP_OK;
}

static esp_err_t h_settings(httpd_req_t *req)
{
    cJSON *j = recv_json(req);
    if (j == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    cJSON *vol = cJSON_GetObjectItem(j, "volume");
    cJSON *bri = cJSON_GetObjectItem(j, "brightness");
    cJSON *slp = cJSON_GetObjectItem(j, "sleep");
    cJSON *tv = cJSON_GetObjectItem(j, "tvurl");

    if (cJSON_IsNumber(vol)) {
        settings_set_volume((int)vol->valuedouble);
        audio_set_vol(settings_get()->volume);
    }
    if (cJSON_IsNumber(bri)) {
        settings_set_brightness((int)bri->valuedouble);
        backlight_set(settings_get()->brightness);
        if (backlight_is_on()) {
            backlight_on();      /* re-apply duty */
        }
    }
    if (cJSON_IsNumber(slp)) {
        settings_set_sleep_min((int)slp->valuedouble);
    }
    if (cJSON_IsString(tv) && tv->valuestring[0] != '\0') {
        settings_set_tv_url(tv->valuestring);
    }
    settings_save();
    cJSON_Delete(j);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, r);
}

static esp_err_t h_restart(httpd_req_t *req)
{
    (void)req;
    settings_save();
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_factory(httpd_req_t *req)
{
    (void)req;
    settings_factory_reset();
    esp_restart();
    return ESP_OK;
}

/* Force a full-screen repaint and time it: render + rotate/swap + SPI DMA for
 * 480x320 pixels. This is the number to watch when tuning the display path. */
static esp_err_t h_uibench(httpd_req_t *req)
{
    const uint32_t full_px = 480u * 320u;   /* logical screen */

    /* ?rot=0 / ?rot=90 temporarily switches the software rotation off to see
     * how much of a frame the per-flush rotation actually costs. */
    int rot = -1;
    char qs[32];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(qs, "rot", val, sizeof(val)) == ESP_OK) {
            rot = atoi(val);
        }
    }
    lv_display_t *disp = lv_display_get_default();
    if (rot >= 0 && lvgl_port_lock(1000)) {
        lv_display_set_rotation(disp, rot == 0 ? LV_DISPLAY_ROTATION_0 : LV_DISPLAY_ROTATION_90);
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ?plain=1 hides the tileview, leaving just the screen background, to find
     * the floor of a full repaint (bus + LVGL fill rate). */
    bool plain = false;
    {
        char qs2[32];
        char val[8];
        if (httpd_req_get_url_query_str(req, qs2, sizeof(qs2)) == ESP_OK &&
            httpd_query_key_value(qs2, "plain", val, sizeof(val)) == ESP_OK) {
            plain = (atoi(val) != 0);
        }
    }
    lv_obj_t *tv = clock_ui_tileview();
    if (plain && tv && lvgl_port_lock(1000)) {
        lv_obj_add_flag(tv, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    uint32_t px0 = ui_perf_total_px();
    uint32_t ar0 = 0, fu0 = 0;
    ui_perf_raw(&ar0, &fu0, NULL);
    int64_t t0 = esp_timer_get_time();

    if (lvgl_port_lock(1000)) {
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_unlock();
    }

    int64_t deadline = t0 + 2000000;
    uint32_t got = 0;
    while (esp_timer_get_time() < deadline) {
        got = ui_perf_total_px() - px0;
        if (got >= full_px - (full_px / 100)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    int64_t dt_us = esp_timer_get_time() - t0;

    ui_perf_snapshot_t pf;
    ui_perf_get(&pf);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "ms", (double)dt_us / 1000.0);
    cJSON_AddNumberToObject(r, "px", got);
    uint32_t ar1 = 0, fu1 = 0;
    ui_perf_raw(&ar1, &fu1, NULL);
    cJSON_AddNumberToObject(r, "areas", (double)(ar1 - ar0));
    cJSON_AddNumberToObject(r, "flushms", (double)(fu1 - fu0) / 1000.0);
    if (plain && tv && lvgl_port_lock(1000)) {
        lv_obj_remove_flag(tv, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    if (rot >= 0 && lvgl_port_lock(1000)) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
        lvgl_port_unlock();
    }
    return send_json(req, r);
}

static esp_err_t h_apmode(httpd_req_t *req)
{
    (void)req;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    send_json(req, r);
    vTaskDelay(pdMS_TO_TICKS(300));
    web_config_enter_ap_mode();
    return ESP_OK;
}

/* ------------------------------ lifecycle -------------------------- */

static bool start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    httpd_uri_t u;
    memset(&u, 0, sizeof(u));
    u.uri = "/";            u.method = HTTP_GET;  u.handler = h_root;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/status";  u.method = HTTP_GET;  u.handler = h_status;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/scan";    u.method = HTTP_GET;  u.handler = h_scan;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/wifi";    u.method = HTTP_POST; u.handler = h_wifi;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/settings";u.method = HTTP_POST; u.handler = h_settings;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/restart"; u.method = HTTP_POST; u.handler = h_restart;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/factory"; u.method = HTTP_POST; u.handler = h_factory;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/uibench"; u.method = HTTP_POST; u.handler = h_uibench;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/uibench"; u.method = HTTP_GET;  u.handler = h_uibench;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/apmode";  u.method = HTTP_POST; u.handler = h_apmode;
    httpd_register_uri_handler(s_server, &u);
    ESP_LOGI(TAG, "http server started (%s)", s_ap_mode ? "AP mode" : "LAN");
    return true;
}

static void web_monitor_task(void *arg)
{
    (void)arg;
    while (!s_server) {
        if (wifi_app_ap_active() || wifi_app_state() == WIFI_APP_CONNECTED) {
            if (start_httpd()) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* Stay alive (the setup AP is entered long after boot, from the settings
     * page or the web page) and keep an eye on the deadline as a safety net;
     * the actual timeout is an esp_timer, which cannot be starved. */
    int tick = 0;
    while (1) {
        if (s_ap_mode) {
            int64_t left = (s_ap_deadline_us - esp_timer_get_time()) / 1000000;
            if (left <= 0) {
                ESP_LOGW(TAG, "setup mode timeout, restarting");
                esp_restart();
            }
            if (++tick >= 15) {     /* ~30 s */
                tick = 0;
                ESP_LOGI(TAG, "setup mode: %lld s left", (long long)left);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void web_config_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    xTaskCreate(web_monitor_task, "web_mon", 3072, NULL, 3, NULL);
}

bool web_config_ap_mode(void)
{
    return s_ap_mode;
}

static void ap_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "setup mode timed out after %d min, restarting", AP_MODE_TIMEOUT_S / 60);
    esp_restart();
}

void web_config_enter_ap_mode(void)
{
    if (s_ap_mode) {
        return;
    }
    ESP_LOGI(TAG, "entering setup AP mode");
    radio_stop();
    s_ap_mode = true;
    s_ap_deadline_us = esp_timer_get_time() + (int64_t)AP_MODE_TIMEOUT_S * 1000000;

    if (s_ap_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = ap_timeout_cb,
            .name = "ap_timeout",
        };
        if (esp_timer_create(&args, &s_ap_timer) != ESP_OK) {
            s_ap_timer = NULL;
        }
    }
    if (s_ap_timer != NULL) {
        esp_timer_start_once(s_ap_timer, (uint64_t)AP_MODE_TIMEOUT_S * 1000000);
    }
    wifi_app_ap_start();
}
