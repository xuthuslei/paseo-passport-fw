// Paseo Passport phase-1 application.
//
// State machine: BOOT -> PROVISIONING (config AP + extended form) when no
// Paseo config is stored, else BOOT -> CONNECTING (station) -> paseo_client
// (relay WS -> E2EE -> daemon hello -> ONLINE). The xiaozhi Application,
// protocol layer, OTA, MCP server and wake word are compiled but never
// initialized in this firmware (see README_PHASE1.md).
#include "paseo_app.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

#include "board.h"
#include "display.h"
#include "settings.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "paseo_client.h"
#include "paseo_ui.h"
#include "pbase64.h"

#define TAG "paseo_app"

#define EV_WIFI_CONNECTED BIT0
#define EV_WIFI_DISCONNECTED BIT1
#define EV_CONFIG_EXIT BIT2

static EventGroupHandle_t s_events;
static paseo_offer_t s_offer;
static bool s_offer_valid = false;

/* ---------------- board key hook + input queue ---------------- */

typedef struct {
    int key;
    int kind;
} paseo_key_msg_t;

static QueueHandle_t s_key_queue = NULL;
static paseo_key_hook_t s_key_hook = NULL;

void paseo_set_key_hook(paseo_key_hook_t hook) { s_key_hook = hook; }
paseo_key_hook_t paseo_get_key_hook(void) { return s_key_hook; }

/* Runs on the iot_button task (board callback context): only enqueue. */
static void BoardKeyHook(int key, int kind)
{
    if (!s_key_queue) return;
    paseo_key_msg_t m = {key, kind};
    xQueueSend(s_key_queue, &m, 0);
}

static PaseoUi s_ui;

/* ---------------- persistent config (NVS namespace "paseo") ---------------- */

static const char *kAsrEndpointDefault =
    "https://api.siliconflow.cn/v1/audio/transcriptions";
static const char *kAsrModelDefault = "XingChenAGI/XingChenASR-V3.2-Ultra";

static bool LoadOffer(paseo_offer_t *out)
{
    Settings s("paseo");
    std::string sid = s.GetString("srv_id");
    std::string ep = s.GetString("relay_ep");
    std::string pub = s.GetString("da_pub_b64");
    if (sid.empty() || ep.empty() || pub.empty()) return false;
    memset(out, 0, sizeof(*out));
    strncpy(out->server_id, sid.c_str(), sizeof(out->server_id) - 1);
    strncpy(out->relay_endpoint, ep.c_str(), sizeof(out->relay_endpoint) - 1);
    out->relay_use_tls = s.GetInt("relay_tls", 0) != 0;
    if (pb64_decode(pub.c_str(), out->daemon_pubkey, 32) != 32) return false;
    return true;
}

static void SaveOffer(const paseo_offer_t &offer)
{
    Settings s("paseo", true);
    s.SetString("srv_id", offer.server_id);
    s.SetString("relay_ep", offer.relay_endpoint);
    s.SetInt("relay_tls", offer.relay_use_tls ? 1 : 0);
    char pub[64];
    pb64_encode(offer.daemon_pubkey, 32, pub, sizeof(pub));
    s.SetString("da_pub_b64", pub);
}

/* ------------- client callbacks (run on paseo client task) -------------
 * They only copy state into the UI object and mark it dirty; all LVGL
 * work happens on this (app) task via PaseoUi::Tick/HandleKey. */

static void OnPaseoState(paseo_state_t st, void *ctx)
{
    (void)ctx;
    s_ui.OnState(st);
    ESP_LOGI(TAG, "paseo state: %d", (int)st);
}

static void OnPaseoAgents(const paseo_agent_t *agents, int count, void *ctx)
{
    (void)ctx;
    s_ui.OnAgents(agents, count);
}

static void OnPaseoPerms(const paseo_perm_t *perms, int count, void *ctx)
{
    (void)ctx;
    s_ui.OnPerms(perms, count);
}

static void OnPaseoTimeline(const char *agent_id, const char *text,
                            const char *error, void *ctx)
{
    (void)ctx;
    s_ui.OnTimeline(agent_id, text, error);
}

static void OnPaseoAction(paseo_action_t action, bool ok,
                          const char *agent_id, const char *error, void *ctx)
{
    (void)ctx;
    s_ui.OnAction(action, ok, agent_id, error);
}

/* ---------------- provisioning form (config AP extra handlers) ------------ */

static std::string UrlDecode(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)(hi * 16 + lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

static std::string FormField(const std::string &body, const char *name)
{
    std::string key = std::string(name) + "=";
    size_t pos = 0;
    while (pos < body.size()) {
        size_t start = (pos == 0) ? 0 : pos;
        if (body.compare(start, key.size(), key) == 0 &&
            (start == 0 || body[start - 1] == '&')) {
            size_t vstart = start + key.size();
            size_t vend = body.find('&', vstart);
            if (vend == std::string::npos) vend = body.size();
            return UrlDecode(body.substr(vstart, vend - vstart));
        }
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

static const char *kFormHead =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Paseo Passport 配置</title><style>"
    "body{font-family:sans-serif;margin:16px;max-width:520px}"
    "textarea,input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box}"
    "button{padding:10px 20px;font-size:16px}"
    "label{font-weight:bold}</style></head><body>";

/* /exit only accepts POST (esp-wifi-connect), so a plain <a href="/exit">
 * link gets a 404 and never exits provisioning (hotfix 0.3.1). All Paseo
 * pages must use this button + fetch POST instead. */
static const char *kExitButton =
    "<p><button type=\"button\" onclick=\"exitProv()\">"
    "完成并退出配网模式</button> <span id=\"exitmsg\"></span></p>"
    "<script>function exitProv(){fetch('/exit',{method:'POST'})"
    ".then(function(){document.getElementById('exitmsg').textContent="
    "'已退出，设备正在连接 WiFi 并上线，可关闭本页。';})"
    ".catch(function(){document.getElementById('exitmsg').textContent="
    "'已发送退出请求。';});}</script>";

static esp_err_t PaseoFormHandler(httpd_req_t *req)
{
    Settings s("paseo");
    std::string asr_ep = s.GetString("asr_ep", kAsrEndpointDefault);
    std::string asr_model = s.GetString("asr_model", kAsrModelDefault);
    std::string provider = s.GetString("def_prov", "codex");
    std::string cwd = s.GetString("def_cwd", "");
    bool has_key = !s.GetString("asr_key").empty();
    bool has_offer = !s.GetString("srv_id").empty();

    std::string html = kFormHead;
    html += "<h2>Paseo 配置</h2>";
    html += "<p>1) 先在<a href=\"/\">WiFi 设置页</a>填好 WiFi；"
            "2) 在本页粘贴 Paseo 配对 offer；3) 最后点页尾的按钮退出配网。</p>";
    html += "<form method=\"POST\" action=\"/paseo/submit\">";
    html += "<label>Paseo 配对 offer 链接";
    if (has_offer) html += "（已保存，留空则不修改）";
    html += "</label><textarea name=\"offer\" rows=\"4\" "
            "placeholder=\"https://app.paseo.sh/#offer=...\"></textarea>";
    html += "<label>ASR API Key（硅基流动）";
    if (has_key) html += "（已保存，留空则不修改）";
    html += "</label><input type=\"password\" name=\"asr_key\">";
    html += "<label>ASR 端点</label><input name=\"asr_ep\" value=\"" + asr_ep + "\">";
    html += "<label>ASR 模型</label><input name=\"asr_model\" value=\"" + asr_model + "\">";
    html += "<label>新建 agent 默认 provider</label><input name=\"def_prov\" value=\"" + provider + "\">";
    html += "<label>新建 agent 默认 cwd</label><input name=\"def_cwd\" value=\"" + cwd + "\">";
    html += "<button type=\"submit\">保存 Paseo 配置</button></form>";
    html += kExitButton;
    html += "</body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html.c_str(), html.size());
}

static esp_err_t PaseoSubmitHandler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    std::string body;
    body.resize(req->content_len);
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, &body[got], req->content_len - got);
        if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        got += r;
    }
    /* NOTE: body contains the offer + API key. Never log it. */
    std::string offer_text = FormField(body, "offer");
    std::string asr_key = FormField(body, "asr_key");
    std::string asr_ep = FormField(body, "asr_ep");
    std::string asr_model = FormField(body, "asr_model");
    std::string def_prov = FormField(body, "def_prov");
    std::string def_cwd = FormField(body, "def_cwd");

    paseo_offer_t parsed;
    bool have_parsed = false;
    if (!offer_text.empty()) {
        char err[96] = "";
        if (!paseo_parse_offer(offer_text.c_str(), &parsed, err, sizeof(err))) {
            std::string html = kFormHead;
            html += "<h2>offer 解析失败</h2><p>";
            html += err;
            html += "</p><p>常见原因：offer 粘贴不完整（配对链接很长，"
                    "务必整段复制），或该配对链接已失效。请从 Paseo App "
                    "重新复制完整的配对链接再试。</p>"
                    "<p><a href=\"/paseo\">返回重填</a></p></body></html>";
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            httpd_resp_set_hdr(req, "Connection", "close");
            return httpd_resp_send(req, html.c_str(), html.size());
        }
        SaveOffer(parsed);
        have_parsed = true;
        /* Only identifiers safe to print: serverId + endpoint (design §7). */
        ESP_LOGI(TAG, "paseo config saved: serverId=%s relay=%s tls=%d",
                 parsed.server_id, parsed.relay_endpoint,
                 parsed.relay_use_tls ? 1 : 0);
    }
    {
        Settings s("paseo", true);
        if (!asr_key.empty()) s.SetString("asr_key", asr_key);
        if (!asr_ep.empty()) s.SetString("asr_ep", asr_ep);
        if (!asr_model.empty()) s.SetString("asr_model", asr_model);
        if (!def_prov.empty()) s.SetString("def_prov", def_prov);
        if (!def_cwd.empty()) s.SetString("def_cwd", def_cwd);
    }

    std::string html = kFormHead;
    html += "<h2>已保存</h2>";
    if (have_parsed) {
        html += "<p>serverId：";
        html += parsed.server_id;
        html += "<br>relay：";
        html += parsed.relay_endpoint;
        html += parsed.relay_use_tls ? "（TLS）" : "（明文）";
        html += "</p>";
    } else {
        paseo_offer_t cur;
        if (LoadOffer(&cur)) {
            html += "<p>当前 serverId：";
            html += cur.server_id;
            html += "<br>relay：";
            html += cur.relay_endpoint;
            html += "</p>";
        } else {
            html += "<p style=\"color:#b91c1c;font-weight:bold\">注意：尚未保存"
                    "有效的 offer！只填其它设置就退出，设备会再次回到配网模式。"
                    "请返回表单粘贴 Paseo 配对 offer。</p>";
        }
    }
    html += "<p>如 WiFi 也已设置完成，点下面按钮退出配网，设备将连接 "
            "WiFi 并上线。</p>";
    html += kExitButton;
    html += "<p><a href=\"/paseo\">继续修改</a> | <a href=\"/\">WiFi 设置页</a></p>"
            "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html.c_str(), html.size());
}

/* ---------------- application state machine ---------------- */

void paseo_app_run(void)
{
    /* The key queue + board hook must exist before the board is
     * constructed: its ADC buttons start sampling in the constructor. */
    s_key_queue = xQueueCreate(16, sizeof(paseo_key_msg_t));
    paseo_set_key_hook(BoardKeyHook);

    auto &board = Board::GetInstance();
    auto *display = board.GetDisplay();
    display->SetupUI();
    display->SetStatus("Paseo Passport");
    display->SetChatMessage("system", "Paseo Passport 0.3.4 (phase 3d)");

    /* BOOT-stage provisioning entry (kept from phase 1 semantics): if OK
     * is held from power-on, the button framework reports the long press
     * 2000 ms after press-down (ADC long-press convention), so watch the
     * key queue for a ~2.6 s window before deciding. Only needed when a
     * config exists; without one we provision anyway and skip the wait.
     * Runtime long-presses are separate (handled per screen) and cannot
     * retrigger this: the window closes before station mode starts. */
    bool boot_force_prov = false;
    if (LoadOffer(&s_offer)) {
        display->SetStatus("Paseo Passport (hold OK = provisioning)");
        int64_t until_ms = esp_timer_get_time() / 1000 + 2600;
        while (esp_timer_get_time() / 1000 < until_ms) {
            paseo_key_msg_t m;
            if (xQueueReceive(s_key_queue, &m, pdMS_TO_TICKS(100)) ==
                pdTRUE) {
                if (m.key == PASEO_KEY_OK && m.kind == PASEO_KEY_LONG) {
                    ESP_LOGI(TAG, "OK long-press at boot -> provisioning");
                    boot_force_prov = true;
                    break;
                }
            }
        }
    }

    {
        Settings s("paseo");
        s_ui.Init(display, s.GetString("def_prov", "codex"),
                  s.GetString("def_cwd", ""),
                  s.GetString("asr_ep", kAsrEndpointDefault),
                  s.GetString("asr_model", kAsrModelDefault),
                  s.GetString("asr_key", ""));
    }

    s_events = xEventGroupCreate();
    auto &wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([](WifiEvent e, const std::string &) {
        switch (e) {
        case WifiEvent::Connected:
            xEventGroupSetBits(s_events, EV_WIFI_CONNECTED);
            break;
        case WifiEvent::Disconnected:
            xEventGroupSetBits(s_events, EV_WIFI_DISCONNECTED);
            break;
        case WifiEvent::ConfigModeExit:
            xEventGroupSetBits(s_events, EV_CONFIG_EXIT);
            break;
        default:
            break;
        }
    });
    WifiManagerConfig wcfg;
    wcfg.ssid_prefix = "PaseoPassport";
    wcfg.language = "zh-CN";
    if (!wifi.Initialize(wcfg)) {
        ESP_LOGE(TAG, "wifi init failed");
        display->SetStatus("WiFi init failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    static const httpd_uri_t form_uri = {
        .uri = "/paseo", .method = HTTP_GET,
        .handler = PaseoFormHandler, .user_ctx = NULL};
    static const httpd_uri_t submit_uri = {
        .uri = "/paseo/submit", .method = HTTP_POST,
        .handler = PaseoSubmitHandler, .user_ctx = NULL};
    wifi.AddConfigApUriHandler(form_uri);
    wifi.AddConfigApUriHandler(submit_uri);

    bool force_provisioning = boot_force_prov;
    while (true) {
        s_offer_valid = LoadOffer(&s_offer);
        if (!s_offer_valid || force_provisioning) {
            /* -------- PROVISIONING -------- */
            ESP_LOGI(TAG, "entering provisioning (config AP)");
            display->SetStatus("Provisioning: config AP");
            if (s_offer_valid) {
                display->SetChatMessage(
                    "system",
                    "配网模式：可在配网页修改 WiFi 与 Paseo 配置，完成后在"
                    "网页点“完成并退出配网模式”按钮。");
            } else if (!SsidManager::GetInstance().GetSsidList().empty()) {
                display->SetChatMessage(
                    "system",
                    "还差第 2 步：WiFi 已保存，但 Paseo offer 未配置。连接"
                    "热点打开配网页，点顶部橙色按钮粘贴 Paseo offer 并保存。");
            } else {
                display->SetChatMessage(
                    "system",
                    "配网两步（缺一不可）：①连接热点 PaseoPassport-xxxx，"
                    "打开配网页选 WiFi 点 Connect 保存；②点配网页顶部橙色"
                    "按钮，粘贴 Paseo offer 并保存。两步完成后退出配网。");
            }
            xEventGroupClearBits(s_events, EV_CONFIG_EXIT);
            wifi.StartConfigAp();
            xEventGroupWaitBits(s_events, EV_CONFIG_EXIT, pdTRUE, pdFALSE,
                                portMAX_DELAY);
            ESP_LOGI(TAG, "provisioning exited");
            force_provisioning = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* -------- CONNECTING (station) -------- */
        ESP_LOGI(TAG, "connecting wifi (station), serverId=%s",
                 s_offer.server_id);
        display->SetStatus("WiFi connecting...");
        xEventGroupClearBits(s_events, EV_WIFI_CONNECTED | EV_WIFI_DISCONNECTED);
        wifi.StartStation();
        EventBits_t bits = xEventGroupWaitBits(
            s_events, EV_WIFI_CONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(60000));
        if (!(bits & EV_WIFI_CONNECTED)) {
            ESP_LOGW(TAG, "station connect timeout; back to provisioning");
            display->SetStatus("WiFi failed -> provisioning");
            force_provisioning = true;
            continue;
        }
        ESP_LOGI(TAG, "wifi connected: %s", wifi.GetIpAddress().c_str());
        display->SetStatus("WiFi connected");

        /* -------- ONLINE via paseo_client -------- */
        paseo_client_config_t ccfg = {};
        ccfg.offer = s_offer;
        ccfg.on_state = OnPaseoState;
        ccfg.on_agents = OnPaseoAgents;
        ccfg.on_perms = OnPaseoPerms;
        ccfg.on_timeline = OnPaseoTimeline;
        ccfg.on_action = OnPaseoAction;
        ccfg.ctx = NULL;
        paseo_client_start(&ccfg);
        /* Drop keys pressed while connecting; show the phase-2 screens. */
        {
            paseo_key_msg_t m;
            while (xQueueReceive(s_key_queue, &m, 0) == pdTRUE) {
            }
        }
        s_ui.Show();

        /* Stay until WiFi is lost for good: tolerate transient drops while
         * WifiStation retries with backoff (grace 25 s). */
        int64_t down_since_ms = -1;
        while (true) {
            EventBits_t b = xEventGroupWaitBits(
                s_events, EV_WIFI_CONNECTED | EV_WIFI_DISCONNECTED, pdTRUE,
                pdFALSE, pdMS_TO_TICKS(200));
            paseo_key_msg_t m;
            while (xQueueReceive(s_key_queue, &m, 0) == pdTRUE)
                s_ui.HandleKey(m.key, m.kind);
            s_ui.Tick();
            if (b & EV_WIFI_CONNECTED) down_since_ms = -1;
            if (b & EV_WIFI_DISCONNECTED && down_since_ms < 0)
                down_since_ms = esp_timer_get_time() / 1000;
            if (down_since_ms >= 0 &&
                esp_timer_get_time() / 1000 - down_since_ms > 25000)
                break;
        }
        ESP_LOGW(TAG, "wifi lost; stopping paseo client");
        s_ui.Hide();
        paseo_client_stop();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
