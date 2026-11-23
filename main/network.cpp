#include "network.hpp"
#include "board.hpp"
#include "clock_time.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

namespace {
constexpr char tag[] = "clock_net";
constexpr int64_t sec = 1000000;
constexpr char kSetupPassword[] = "12345678";
struct Zone { int32_t base = 480; int32_t dst = 0; };
struct Wifi { char ssid[33]; char key[65]; };
Zone zone;
Wifi saved_wifi{};
bool saved_wifi_configured = false;
std::atomic<int32_t> offset{480};
std::atomic<bool> portal{false}, online{false};
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t last_sync;
NetworkStatus identity{};
char csrf[33];
esp_netif_t *ap_netif;
httpd_handle_t server;
struct ifreq ap_interface{};
QueueHandle_t changes;

bool ok(esp_err_t err)
{
    if (err != ESP_OK) ESP_LOGW(tag, "%s", esp_err_to_name(err));
    return err == ESP_OK;
}

void random_hex(char *text, size_t size)
{
    uint8_t bytes[16];
    constexpr char hex[] = "0123456789abcdef";
    esp_fill_random(bytes, size);
    for (size_t i = 0; i < size; ++i) {
        text[2*i] = hex[bytes[i] >> 4];
        text[2*i+1] = hex[bytes[i] & 15];
    }
    text[2*size] = 0;
}

void set_zone(Zone value)
{
    zone = value;
    offset.store(value.base + 60 * value.dst);
}

void read_zone()
{
    nvs_handle_t handle;
    if (nvs_open("clock_ui", NVS_READONLY, &handle) != ESP_OK) return;
    Zone value;
    size_t len = sizeof value;
    if (nvs_get_blob(handle, "zone", &value, &len) == ESP_OK && len == sizeof value &&
        value.base >= -720 && value.base <= 840 && (value.dst == 0 || value.dst == 1)) set_zone(value);
    nvs_close(handle);
}

bool save_zone(Zone value)
{
    nvs_handle_t handle;
    if (nvs_open("clock_ui", NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool saved = nvs_set_blob(handle, "zone", &value, sizeof value) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (saved) set_zone(value);
    return saved;
}

bool read_backlight()
{
    nvs_handle_t handle;
    if (nvs_open("clock_ui", NVS_READONLY, &handle) != ESP_OK) return false;
    uint32_t value = 100;
    size_t len = sizeof value;
    const bool read = nvs_get_blob(handle, "backlight", &value, &len) == ESP_OK &&
                      len == sizeof value && value <= 100;
    nvs_close(handle);
    if (read) board::set_backlight_percent(value);
    return read;
}

bool save_backlight(uint32_t value)
{
    nvs_handle_t handle;
    if (nvs_open("clock_ui", NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool saved = nvs_set_blob(handle, "backlight", &value, sizeof value) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (saved) board::set_backlight_percent(value);
    return saved;
}

bool read_saved_wifi()
{
    nvs_handle_t handle;
    if (nvs_open("clock_wifi", NVS_READONLY, &handle) != ESP_OK) return false;
    Wifi value{};
    size_t len = sizeof value;
    const bool read = nvs_get_blob(handle, "wifi", &value, &len) == ESP_OK &&
                      len == sizeof value && value.ssid[32] == 0 &&
                      std::strlen(value.ssid) > 0 &&
                      std::strlen(value.ssid) <= 32 &&
                      std::strlen(value.key) <= 64;
    nvs_close(handle);
    if (!read) return false;
    saved_wifi = value;
    saved_wifi_configured = true;
    return true;
}

bool save_wifi(const Wifi& value)
{
    nvs_handle_t handle;
    if (nvs_open("clock_wifi", NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool saved = nvs_set_blob(handle, "wifi", &value, sizeof value) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (saved) {
        saved_wifi = value;
        saved_wifi_configured = true;
    }
    return saved;
}

void link(bool up)
{
    portENTER_CRITICAL(&state_lock);
    const bool changed = online.exchange(up) != up;
    if (!up) {
        last_sync = 0;
        identity.sta_ip[0] = 0;
    }
    portEXIT_CRITICAL(&state_lock);
    if (changed) clock_time::notify_network(up);
}

void event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (data) {
            const auto* ip = static_cast<const ip_event_got_ip_t*>(data);
            esp_ip4addr_ntoa(&ip->ip_info.ip, identity.sta_ip, sizeof identity.sta_ip);
        }
        wifi_ap_record_t ap{};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            std::memcpy(identity.sta_ssid, ap.ssid, sizeof identity.sta_ssid);
            identity.sta_ssid[32] = 0;
        }
        link(true);
    } else if ((base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) ||
             (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)) link(false);
}

bool request_ok(httpd_req_t *req)
{
    if (portal.load()) return true;
    ESP_LOGW(tag, "Rejected %s because the setup portal is inactive", req->uri);
    return false;
}

esp_err_t reply(httpd_req_t *req, const char *body, const char *status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

esp_err_t reply_html(httpd_req_t *req, const char *body)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'none'; style-src 'unsafe-inline'; base-uri 'none'");
    return httpd_resp_sendstr(req, body);
}

esp_err_t captive_redirect(httpd_req_t *req)
{
    if (!request_ok(req)) return reply(req, "请通过时钟热点访问。", "403 Forbidden");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "");
}

esp_err_t form(httpd_req_t *req)
{
    if (!request_ok(req)) return reply(req, "请通过时钟热点访问。", "403 Forbidden");
    char body[2200];
    const int len = std::snprintf(body, sizeof body, R"html(<!doctype html><html lang="zh-CN">
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>时钟设置</title><h1>时钟设置</h1>
<form method="post" action="/configure"><input name="token" type="hidden" value="%s">
<p><label>Wi-Fi 名称（留空只改时区） <input name="ssid" maxlength="32"></label></p>
<p><label>Wi-Fi 密码（开放网络留空） <input name="key" type="password" maxlength="64" autocomplete="new-password"></label></p>
<p><label>UTC 偏移（分钟） <input name="zone" type="number" min="-720" max="840" value="%ld" required></label></p>
<p><label><input name="dst" type="checkbox" value="1" %s>夏令时（手动加 60 分钟）</label></p>
<p><label>背光亮度：<output id="brightness-value">%lu%%</output><br>
<input name="backlight" type="range" min="10" max="100" step="5" value="%lu"
oninput="document.getElementById('brightness-value').value=this.value+'%%'"></label></p>
<button>应用设置</button></form>
<form method="post" action="/time" onsubmit="this.elements.utc.value=Math.floor(Date.now()/1000)">
<input name="token" type="hidden" value="%s"><input name="utc" type="hidden">
<button>使用本机时间</button>（SNTP 成功前可用）</form>
<p>无线网络密码仅在本次开机使用；时区设置会保留。</p></html>)html",
        csrf, static_cast<long>(zone.base), zone.dst ? "checked" : "",
        static_cast<unsigned long>(board::backlight_percent()),
        static_cast<unsigned long>(board::backlight_percent()), csrf);
    if (len < 0 || static_cast<size_t>(len) >= sizeof body) return reply(req, "页面生成失败。", "500 Internal Server Error");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(req, body, len);
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template<size_t N>
bool field(const char *body, const char *name, char (&out)[N], bool optional = false)
{
    *out = 0;
    bool found = false;
    const size_t len = std::strlen(name);
    while (*body) {
        const char *end = std::strchr(body, '&');
        if (!end) end = body + std::strlen(body);
        if (static_cast<size_t>(end-body) > len && body[len] == '=' && !std::strncmp(body, name, len)) {
            if (found) return false;
            found = true;
            size_t n = 0;
            for (auto p = body + len + 1; p < end; ++p) {
                int c = static_cast<unsigned char>(*p);
                if (c == '%') {
                    if (end-p < 3 || hex_digit(p[1]) < 0 || hex_digit(p[2]) < 0) return false;
                    c = 16*hex_digit(p[1]) + hex_digit(p[2]); p += 2;
                } else if (c == '+') c = ' ';
                if (c < 32 || c == 127 || n+1 >= N) return false;
                out[n++] = static_cast<char>(c);
            }
            out[n] = 0;
        }
        body = *end ? end+1 : end;
    }
    return found || optional;
}

bool number(const char *text, long long lo, long long hi, long long &out)
{
    char *end;
    errno = 0;
    out = std::strtoll(text, &end, 10);
    return !errno && end != text && !*end && out >= lo && out <= hi;
}

esp_err_t submit(httpd_req_t *req)
{
    if (!request_ok(req)) return reply(req, "请通过时钟热点访问。", "403 Forbidden");
    char body[640], type[48], token[33];
    if (req->content_len == 0 || req->content_len >= sizeof body ||
        httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof type) != ESP_OK ||
        std::strcmp(type, "application/x-www-form-urlencoded")) return reply(req, "请求格式错误。", "400 Bad Request");
    for (size_t used = 0; used < req->content_len;) {
        const int got = httpd_req_recv(req, body+used, req->content_len-used);
        if (got <= 0) return ESP_FAIL;
        used += got;
    }
    body[req->content_len] = 0;
    if (std::memchr(body, 0, req->content_len) || !field(body, "token", token) || std::strcmp(token, csrf))
        return reply(req, "请重新打开设置页。", "403 Forbidden");
    if (!std::strcmp(req->uri, "/time")) {
        char value[16]; long long utc;
        if (!field(body, "utc", value) || !number(value, 1577836800LL, 4102444799LL, utc))
            return reply(req, "请检查手机或电脑的日期。", "400 Bad Request");
        if (!clock_time::submit_manual_utc(static_cast<std::time_t>(utc)))
            return reply(req, "现在不能使用浏览器校时，SNTP 优先。", "409 Conflict");
        return reply(req, "已提交时间，请查看屏幕。");
    }
    Wifi wifi{};
    char minutes[8], dst[2], backlight[5]; long long base, brightness;
    if (!field(body, "ssid", wifi.ssid) || !field(body, "key", wifi.key) ||
        !field(body, "zone", minutes) || !number(minutes, -720, 840, base) ||
        !field(body, "dst", dst, true) || (*dst && std::strcmp(dst, "1")) ||
        !field(body, "backlight", backlight) || !number(backlight, 10, 100, brightness))
        return reply(req, "请检查网络名称、密码和时区。", "400 Bad Request");
    const size_t len = std::strlen(wifi.key);
    if ((!*wifi.ssid && len) || (len && (len < 8 || (len == 64 && std::strspn(wifi.key, "0123456789abcdefABCDEF") != 64))))
        return reply(req, "请检查网络名称及密码长度。", "400 Bad Request");
    if (!save_zone({static_cast<int32_t>(base), *dst ? 1 : 0})) return reply(req, "保存时区失败。", "500 Internal Server Error");
    if (!save_backlight(static_cast<uint32_t>(brightness))) return reply(req, "保存背光亮度失败。", "500 Internal Server Error");
    if (*wifi.ssid) {
        if (!save_wifi(wifi)) return reply(req, "保存 Wi-Fi 信息失败。", "500 Internal Server Error");
        xQueueOverwrite(changes, &wifi);
        portENTER_CRITICAL(&state_lock);
        std::memcpy(identity.sta_ssid, wifi.ssid, sizeof identity.sta_ssid);
        identity.sta_ssid[32] = 0;
        identity.sta_ip[0] = 0;
        portEXIT_CRITICAL(&state_lock);
        return reply_html(req, R"html(<!doctype html><html lang="zh-CN">
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>设置已接收</title><body style="font-family:sans-serif;text-align:center;padding:2rem">
<h1>设置已接收</h1><p>ESP32 正在连接 Wi-Fi，随后会尝试校时并关闭配网页。</p>
<p>无需重启设备；校时成功后，可以关闭此页面。</p></body></html>)html");
    }
    return reply_html(req, R"html(<!doctype html><html lang="zh-CN">
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>时区已保存</title><body style="font-family:sans-serif;text-align:center;padding:2rem">
<h1>时区已保存</h1><p>页面设置已更新。</p></body></html>)html");
}

bool start_portal()
{
    if (!ok(esp_wifi_set_mode(WIFI_MODE_APSTA)) || !ok(esp_netif_get_netif_impl_name(ap_netif, ap_interface.ifr_name))) return false;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 3;
    cfg.max_uri_handlers = 16;
    cfg.recv_wait_timeout = cfg.send_wait_timeout = 3;
    if (!ok(httpd_start(&server, &cfg))) return false;
    httpd_uri_t uri{};
    uri.uri = "/"; uri.method = HTTP_GET; uri.handler = form;
    auto result = httpd_register_uri_handler(server, &uri);
    if (result == ESP_OK) {
        uri.uri = "/configure"; uri.method = HTTP_POST; uri.handler = submit;
        result = httpd_register_uri_handler(server, &uri);
    }
    if (result == ESP_OK) {
        uri.uri = "/time";
        uri.method = HTTP_POST;
        result = httpd_register_uri_handler(server, &uri);
    }
    if (result == ESP_OK) {
        constexpr const char* paths[] = {
            "/generate_204", "/gen_204", "/hotspot-detect.html",
            "/library/test/success.html", "/connecttest.txt",
            "/redirect", "/fwlink", "/ncsi.txt", "/canonical.html",
            "/success.txt",
        };
        uri.method = HTTP_GET;
        uri.handler = captive_redirect;
        for (const char* path : paths) {
            uri.uri = path;
            result = httpd_register_uri_handler(server, &uri);
            if (result != ESP_OK) break;
        }
    }
    if (!ok(result)) {
        httpd_stop(server); server = nullptr; return false;
    }
    portal.store(true);
    ESP_LOGI(tag, "Setup portal ready; see LCD for access");
    return true;
}

void dns_portal(void *)
{
    const int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGW(tag, "Captive DNS socket could not be created");
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = lwip_htons(53);
    address.sin_addr.s_addr = lwip_htonl(0);
    if (lwip_bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ESP_LOGW(tag, "Captive DNS port 53 could not be bound");
        lwip_close(fd);
        return;
    }

    uint8_t packet[512];
    for (;;) {
        sockaddr_in client{};
        socklen_t client_len = sizeof(client);
        const int length = lwip_recvfrom(fd, packet, sizeof(packet), 0,
                                         reinterpret_cast<sockaddr*>(&client), &client_len);
        if (length < 17 || (packet[2] & 0x80) != 0) continue;
        if (!portal.load()) continue;

        size_t position = 12;
        bool valid = true;
        while (position < static_cast<size_t>(length) && packet[position] != 0) {
            const uint8_t label = packet[position];
            if ((label & 0xc0) != 0 || position + label + 1 >= static_cast<size_t>(length)) {
                valid = false;
                break;
            }
            position += label + 1;
        }
        if (!valid || position >= static_cast<size_t>(length) ||
            position + 5 >= static_cast<size_t>(length)) continue;
        ++position;
        if (position + 4 > static_cast<size_t>(length)) continue;
        const size_t question_length = position - 12 + 4;
        if (question_length > 128) continue;

        uint8_t response[256];
        std::memcpy(response, packet, 12);
        response[2] = 0x81;
        response[3] = 0x80;
        response[6] = 0;
        response[7] = 1;
        response[8] = 0;
        response[9] = 0;
        response[10] = 0;
        response[11] = 0;
        std::memcpy(response + 12, packet + 12, question_length);

        size_t answer = 12 + question_length;
        response[answer++] = 0xc0;
        response[answer++] = 0x0c;
        response[answer++] = 0x00;
        response[answer++] = 0x01;
        response[answer++] = 0x00;
        response[answer++] = 0x01;
        response[answer++] = 0x00;
        response[answer++] = 0x00;
        response[answer++] = 0x00;
        response[answer++] = 0x3c;
        response[answer++] = 0x00;
        response[answer++] = 0x04;
        response[answer++] = 192;
        response[answer++] = 168;
        response[answer++] = 4;
        response[answer++] = 1;
        lwip_sendto(fd, response, answer, 0,
                    reinterpret_cast<sockaddr*>(&client), client_len);
    }
}

void manage(void *)
{
    int64_t retry = 0, retry_portal = 0, need_since = esp_timer_get_time();
    bool configured = saved_wifi_configured;
    for (;;) {
        const auto now = esp_timer_get_time();
        Wifi wifi;
        if (xQueueReceive(changes, &wifi, 0) == pdTRUE) {
            esp_wifi_disconnect();
            link(false);
            wifi_config_t cfg{};
            std::memcpy(cfg.sta.ssid, wifi.ssid, std::strlen(wifi.ssid));
            std::memcpy(cfg.sta.password, wifi.key, std::strlen(wifi.key));
            configured = ok(esp_wifi_set_config(WIFI_IF_STA, &cfg));
            retry = now + sec;
        }
        portENTER_CRITICAL(&state_lock);
        const auto synced = last_sync;
        const bool up = online.load();
        portEXIT_CRITICAL(&state_lock);
        if (configured && !up && now >= retry) {
            esp_wifi_connect();
            retry = now + 10 * sec;
        }
        const bool recent = up && synced && now - synced < 7200 * sec;
        if (recent) need_since = now;
        if (!recent && !portal.load() && now >= retry_portal && (!configured || now - need_since >= 35 * sec)) {
            if (!start_portal()) { esp_wifi_set_mode(WIFI_MODE_STA); retry_portal = now + 5 * sec; }
        } else if (recent && portal.load() && now - synced >= 15 * sec) {
            portal.store(false);
            httpd_stop(server); server = nullptr;
            ok(esp_wifi_set_mode(WIFI_MODE_STA));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
} // namespace

void network_init()
{
    identity = {};
    read_zone();
    read_backlight();
    if (!ok(esp_netif_init()) || !ok(esp_event_loop_create_default())) return;
    if (!esp_netif_create_default_wifi_sta() || !(ap_netif = esp_netif_create_default_wifi_ap())) return;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false; // Wi-Fi 凭据不写入 NVS；NVS 只保留界面偏好。
    if (!ok(esp_wifi_init(&init)) || !ok(esp_wifi_set_storage(WIFI_STORAGE_RAM)) ||
        !ok(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event, nullptr)) ||
        !ok(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event, nullptr)) ||
        !ok(esp_wifi_set_mode(WIFI_MODE_STA)) || !ok(esp_wifi_start())) return;
    uint8_t mac[6];
    if (!ok(esp_wifi_get_mac(WIFI_IF_AP, mac))) return;
    std::snprintf(identity.ap_ssid, sizeof identity.ap_ssid, "WaveshareClock-%02X%02X", mac[4], mac[5]);
    std::memcpy(identity.setup_key, kSetupPassword, sizeof kSetupPassword);
    random_hex(csrf, 16);
    wifi_config_t cfg{};
    std::memcpy(cfg.ap.ssid, identity.ap_ssid, std::strlen(identity.ap_ssid));
    std::memcpy(cfg.ap.password, identity.setup_key, std::strlen(identity.setup_key));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    read_saved_wifi();
    wifi_config_t sta_cfg{};
    if (saved_wifi_configured) {
        std::memcpy(sta_cfg.sta.ssid, saved_wifi.ssid, std::strlen(saved_wifi.ssid));
        std::memcpy(sta_cfg.sta.password, saved_wifi.key, std::strlen(saved_wifi.key));
    }
    // 停止 RF 后配置 AP，避免短暂开启默认开放热点。
    if (!ok(esp_wifi_stop()) || !ok(esp_wifi_set_mode(WIFI_MODE_APSTA)) || !ok(esp_wifi_set_config(WIFI_IF_AP, &cfg)) ||
        (saved_wifi_configured && !ok(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg))) ||
        !ok(esp_wifi_set_mode(WIFI_MODE_STA)) || !ok(esp_wifi_start())) return;
    if (saved_wifi_configured) {
        std::memcpy(identity.sta_ssid, saved_wifi.ssid, sizeof identity.sta_ssid);
        identity.sta_ssid[32] = 0;
        ok(esp_wifi_connect());
    }
    changes = xQueueCreate(1, sizeof(Wifi));
    if (!changes || xTaskCreate(manage, "clock_net", 4096, nullptr, 4, nullptr) != pdPASS)
        ESP_LOGE(tag, "Could not start network manager");
    if (xTaskCreate(dns_portal, "clock_dns", 4096, nullptr, 4, nullptr) != pdPASS)
        ESP_LOGE(tag, "Could not start captive DNS");
}

void network_sntp_result()
{
    const auto now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    if (online.load()) last_sync = now;
    portEXIT_CRITICAL(&state_lock);
}

NetworkStatus network_status()
{
    NetworkStatus result{};
    portENTER_CRITICAL(&state_lock);
    result.ap_active = portal.load();
    if (result.ap_active) {
        std::memcpy(result.ap_ssid, identity.ap_ssid, sizeof result.ap_ssid);
        std::memcpy(result.setup_key, identity.setup_key, sizeof result.setup_key);
    }
    result.sta_online = online.load();
    std::memcpy(result.sta_ssid, identity.sta_ssid, sizeof result.sta_ssid);
    std::memcpy(result.sta_ip, identity.sta_ip, sizeof result.sta_ip);
    portEXIT_CRITICAL(&state_lock);
    return result;
}

int32_t network_utc_offset_minutes() { return offset.load(); }
