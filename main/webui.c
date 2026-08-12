#include "webui.h"
#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "churn.h"
#include "settings.h"
#include "roster.h"
#include "probe.h"
#include "vbat.h"
#include "coexist.h"
#include "observe.h"
#include "config_wire.h"   // CONFIG_CLEAR_THREATS sentinel
#include "rf_model.h"
#include "ble_devices.h"

int webui_build_status_json(char *buf, size_t len, const webui_status_t *st)
{
    int off = 0, n;
    #define PUT(...) do { \
        n = snprintf(buf + off, len - (size_t)off, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= len - (size_t)off) { \
            if (len) { buf[len - 1] = '\0'; } \
            return -1; \
        } \
        off += n; \
    } while (0)

    PUT("{\"uptime_s\":%u,\"decoy_paused\":%s,\"wifi_config_mode\":%s,"
        "\"active_devices\":%u,\"roster_size\":%u,\"probes_sent\":%u,"
        "\"epoch\":%u,\"pop_ewma\":%u,\"total_obs\":%u,\"active_target\":%u,"
        "\"threat_count\":%u,\"threats\":[",
        (unsigned)st->uptime_s, st->decoy_paused ? "true" : "false",
        st->wifi_config_mode ? "true" : "false",
        (unsigned)st->active_devices, (unsigned)st->roster_size,
        (unsigned)st->probes_sent, (unsigned)st->epoch,
        (unsigned)st->pop_ewma, (unsigned)st->total_obs,
        (unsigned)st->active_target, (unsigned)st->threat_count);

    for (uint8_t i = 0; i < st->threat_count && i < DETECT_MAX_THREATS; i++) {
        const detect_threat_t *t = &st->threats[i];
        PUT("%s{\"hash\":\"%08x\",\"vendor\":%u,\"rssi\":%d,\"epochs\":%u,"
            "\"first\":%u,\"last\":%u}",
            i ? "," : "", (unsigned)t->hash, (unsigned)t->vendor,
            (int)t->best_rssi, (unsigned)t->epochs,
            (unsigned)t->first_epoch, (unsigned)t->last_epoch);
    }
    PUT("]}");
    #undef PUT
    return off;
}

extern const char index_html_start[] asm("_binary_webui_index_html_start");
extern const char index_html_end[]   asm("_binary_webui_index_html_end");

static const char *WTAG = "webui";
static volatile bool s_window_done = false;
static volatile bool s_dns_run = false;

void webui_gather_status(webui_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->uptime_s        = (uint32_t)(esp_timer_get_time() / 1000000);
    out->decoy_paused    = churn_paused();
    out->wifi_config_mode= true;
    out->active_devices  = (uint16_t)churn_active_count();
    ble_devices_form_counts(&out->form_restless, &out->form_wandering, &out->form_bound);
    out->roster_size     = CHURN_ROSTER_SIZE;
    out->probes_sent     = probe_total_sent();
    out->tx_degraded     = !probe_tx_healthy();
    out->battery_low     = vbat_low();
    out->model_saturated = observe_saturated();   // density is a floor, not a count
    // Gate on vbat_present(), not on mv > 0. The ADC backend returns whatever the divider reads
    // even with no cell fitted (a floating node, or the charger rail), so `mv > 0` would report a
    // bogus voltage as a battery; vbat_present() applies the VBAT_PRESENT_MV threshold that decides
    // whether a cell is actually there. battery_mv == 0 is the wire's "no battery" signal.
    bool bpresent = vbat_present();
    int bmv = bpresent ? vbat_mv() : -1;
    out->battery_mv  = (bmv > 0) ? (uint16_t)bmv : 0;
    int bpc = bpresent ? vbat_soc_pct() : -1;
    out->battery_pct = (bpc >= 0) ? (uint8_t)bpc : 0xFF;
    out->epoch           = coexist_current_epoch();
    out->active_target   = churn_active_target();
    rf_model_t m;
    if (rf_model_load_nvs(&m) == 0) {
        out->pop_ewma  = (uint16_t)(m.pop_ewma + 0.5f);
        out->total_obs = m.total_obs;
    }
    size_t nt = detect_threat_count();
    if (nt > DETECT_MAX_THREATS) nt = DETECT_MAX_THREATS;
    for (size_t i = 0; i < nt; i++) detect_threat_at(i, &out->threats[i]);
    out->threat_count = (uint8_t)nt;
}

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_redirect(httpd_req_t *r)   // captive-portal probes -> the page
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t h_status(httpd_req_t *r)
{
    webui_status_t st; webui_gather_status(&st);
    char buf[1536];
    int n = webui_build_status_json(buf, sizeof(buf), &st);
    if (n < 0) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

// Extract the "action" value from a tiny {"action":"..."} body into out. Returns false if the key
// is absent or the value doesn't terminate. Replaces substring matching on the raw body, where
// "preset_normal_oops" matched preset_normal and the else-if order silently decided ties.
static bool action_of(const char *body, char *out, size_t cap)
{
    const char *k = strstr(body, "\"action\"");
    if (!k) return false;
    const char *q = strchr(k + 8, '"');            // opening quote of the value
    if (!q) return false;
    const char *e = strchr(++q, '"');              // closing quote
    if (!e || (size_t)(e - q) >= cap) return false;
    memcpy(out, q, (size_t)(e - q));
    out[e - q] = '\0';
    return true;
}

static esp_err_t h_control(httpd_req_t *r)
{
    char body[128];
    int len = r->content_len < sizeof(body)-1 ? (int)r->content_len : (int)sizeof(body)-1;
    int off = 0;
    while (off < len) {                            // httpd_req_recv may return a short read
        int got = httpd_req_recv(r, body + off, len - off);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return httpd_resp_send_500(r);
        off += got;
    }
    body[off] = '\0';

    char act[32];
    if (!action_of(body, act, sizeof act)) return httpd_resp_send_500(r);

    static const struct { const char *name; sim_preset_t preset; } PRESETS[] = {
        { "preset_stealth", SIM_PRESET_STEALTH }, { "preset_normal", SIM_PRESET_NORMAL },
        { "preset_dense",   SIM_PRESET_DENSE   }, { "preset_max",    SIM_PRESET_MAX    },
        { "preset_pause",   SIM_PRESET_PAUSE   },
    };
    // Requests are queued for the coexist tick, never applied on the HTTP task: presets resize the
    // BLE population and clear_threats memsets the detector table, both of which coexist_task is
    // concurrently reading.
    for (size_t i = 0; i < sizeof PRESETS / sizeof PRESETS[0]; i++)
        if (strcmp(act, PRESETS[i].name) == 0) {
            coexist_request_preset((uint8_t)PRESETS[i].preset);
            return httpd_resp_sendstr(r, "{\"ok\":1}");
        }

    if      (strcmp(act, "detect_toggle") == 0) detect_set_enabled(!detect_enabled());
    else if (strcmp(act, "churn_toggle")  == 0) coexist_request_preset((uint8_t)(
                                                    sim_settings_get_paused() ? SIM_PRESET_NORMAL : SIM_PRESET_PAUSE));
    else if (strcmp(act, "clear_threats") == 0) coexist_request_preset(CONFIG_CLEAR_THREATS);
    else if (strcmp(act, "done")          == 0) s_window_done = true;
    else if (strcmp(act, "reboot")        == 0) { httpd_resp_sendstr(r, "{\"ok\":1}"); esp_restart(); }
    else return httpd_resp_send_404(r);            // unknown action: say so, don't silently succeed
    return httpd_resp_sendstr(r, "{\"ok\":1}");
}

static void dns_task(void *arg)   // answer every A-query with 192.168.4.1 (captive portal)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) { if (s>=0) close(s); vTaskDelete(NULL); return; }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t pkt[512];
    while (s_dns_run) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int n = recvfrom(s, pkt, sizeof(pkt), 0, (struct sockaddr*)&cli, &cl);
        if (n < 12) continue;
        pkt[2] |= 0x80; pkt[3] = 0x80;              // response, no error
        pkt[6]=0; pkt[7]=1; pkt[8]=0; pkt[9]=0; pkt[10]=0; pkt[11]=0;  // 1 question, 1 answer
        if (n + 16 > (int)sizeof(pkt)) continue;
        uint8_t *p = pkt + n;
        *p++=0xc0; *p++=0x0c;                       // name ptr to question
        *p++=0; *p++=1; *p++=0; *p++=1;             // type A, class IN
        *p++=0; *p++=0; *p++=0; *p++=60;            // TTL 60
        *p++=0; *p++=4; *p++=192; *p++=168; *p++=4; *p++=1;   // RDLENGTH + 192.168.4.1
        sendto(s, pkt, p - pkt, 0, (struct sockaddr*)&cli, cl);
    }
    close(s); vTaskDelete(NULL);
}

void webui_run_config_window(uint32_t timeout_ms)
{
    // --- Wi-Fi/netif/event init: coexist deferred its STA bring-up, so we own the stack
    //     here and fully deinit on teardown, leaving it clean for probe_wifi_init (STA). ---
    esp_netif_init();
    esp_event_loop_create_default();            // ESP_ERR_INVALID_STATE if already up: fine
    esp_netif_t *ap_if = esp_netif_create_default_wifi_ap();
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     // don't persist AP config to NVS

    // --- open SoftAP, randomized SSID suffix so multiple units don't collide ---
    wifi_config_t ap = {0};
    int sl = snprintf((char*)ap.ap.ssid, sizeof(ap.ap.ssid),
                      "simulacra-%04x", (unsigned)(esp_random() & 0xFFFF));
    ap.ap.ssid_len = sl; ap.ap.channel = 1; ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(WTAG, "config AP up: SSID=%s http://192.168.4.1/ (%u s idle timeout, stays up while connected)",
             ap.ap.ssid, (unsigned)(timeout_ms/1000));

    s_dns_run = true;
    xTaskCreate(dns_task, "webdns", 3072, NULL, 5, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) == ESP_OK) {
        httpd_register_uri_handler(srv, &(httpd_uri_t){ .uri="/", .method=HTTP_GET, .handler=h_root });
        httpd_register_uri_handler(srv, &(httpd_uri_t){ .uri="/api/status", .method=HTTP_GET, .handler=h_status });
        httpd_register_uri_handler(srv, &(httpd_uri_t){ .uri="/api/control", .method=HTTP_POST, .handler=h_control });
        httpd_register_uri_handler(srv, &(httpd_uri_t){ .uri="/*", .method=HTTP_GET, .handler=h_redirect });
    }

    // Stay up while a client is connected; only apply timeout_ms as an *idle* timeout, so a
    // connected session is never torn down mid-use (re-arm the countdown on each connect).
    //
    // The idle timer alone is unbounded from an attacker's perspective: sta.num > 0 keeps
    // re-arming it forever, and this call blocks simulacra_task before coexist_set_wifi_enabled /
    // esp_now_link_start run -- so an adversary in RF range at power-on who simply associates and
    // stays associated holds the decoy deaf (no Wi-Fi probes, no ESP-NOW fleet link) indefinitely,
    // or can silently pause it via one unauthenticated /api/control POST, forever. The open-AP /
    // no-auth-API tradeoff is deliberate and documented above (this feature defaults OFF); an
    // unbounded hold time was not. Cap the whole window at a flat multiple of timeout_ms -- long
    // enough for a real user to actually browse and configure the device, short enough that a
    // hostile client can't wedge it past a normal boot session.
    uint32_t window_open_at = (uint32_t)(esp_timer_get_time()/1000);
    uint32_t hard_cap_ms = timeout_ms * 5u;
    uint32_t start = window_open_at;
    s_window_done = false;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time()/1000);
        wifi_sta_list_t sta = {0};
        esp_wifi_ap_get_sta_list(&sta);
        if (sta.num > 0) start = now;              // a client is connected -> keep the window open
        if (s_window_done) break;
        if (now - start >= timeout_ms) break;      // idle for timeout_ms -> hand Wi-Fi to the decoy
        if (now - window_open_at >= hard_cap_ms) {  // absolute ceiling regardless of connection state
            ESP_LOGW(WTAG, "config window hit its %u s hard cap -- closing despite an active client",
                     (unsigned)(hard_cap_ms/1000));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // --- teardown: HTTP, DNS, then fully deinit Wi-Fi so probe_wifi_init can re-init to STA ---
    if (srv) httpd_stop(srv);
    s_dns_run = false;
    vTaskDelay(pdMS_TO_TICKS(1200));            // let dns_task hit its recv timeout and exit
    esp_wifi_stop();
    esp_wifi_deinit();                          // leave nothing inited (probe_wifi_init re-inits fresh)
    esp_netif_destroy_default_wifi(ap_if);
    ESP_LOGW(WTAG, "config window closed -> handing Wi-Fi to the decoy");
}
