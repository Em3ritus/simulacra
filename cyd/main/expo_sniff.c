#include "expo_sniff.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "expo";
static exposure_t *s_e;
static const uint8_t CH[] = { 1, 6, 11 };
static int s_hop;
static uint32_t s_last_hop;

// FNV-1a over the probe's IE STRUCTURE: (id,len) pairs, excluding SSID (id 0) and DS param (id 3),
// and never the addresses -- so a device's probes group across MAC + SSID rotation.
static uint32_t fingerprint(const uint8_t *ies, int len)
{
    uint32_t h = 2166136261u;
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], ln = ies[i + 1];
        if (id != 0x00 && id != 0x03) { h ^= id; h *= 16777619u; h ^= ln; h *= 16777619u; }
        i += 2 + ln;
    }
    return h;
}

static const uint8_t *ssid_of(const uint8_t *ies, int len, uint8_t *out_len)
{
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], ln = ies[i + 1];
        // ln is attacker-controlled (this scans every promiscuously-captured over-the-air probe
        // request) and was never checked against the bytes actually remaining in the captured
        // frame -- only the 2-byte (id,len) header was bounds-checked above, not the IE body it
        // declares. A truncated/malformed frame could claim ln up to 255 while only a few real
        // bytes of IE buffer remain, and the caller's memcpy(tmp, ss, sl) trusts sl as a read
        // length with no way to know it ran past the packet into the Wi-Fi driver's RX buffer
        // pool -- a real out-of-bounds heap read of a nearby attacker's choosing. rx_cb runs on
        // every over-the-air probe request within range, so this is directly remotely reachable.
        if (id == 0x00) {
            if (i + 2 + ln > len) { *out_len = 0; return 0; }   // declared IE body doesn't fit: reject
            *out_len = ln; return ln ? &ies[i + 2] : 0;
        }
        i += 2 + ln;
    }
    *out_len = 0; return 0;
}

static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !s_e) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = p->payload;
    int flen = p->rx_ctrl.sig_len;
    if (flen < 24) return;                    // shorter than a MAC header: nothing to read safely
    if (f[0] != 0x40) return;                 // probe request
    int ielen = flen - 24 - 4;                // after the 24-byte MAC header, minus FCS
    if (ielen < 2) return;
    const uint8_t *ies = f + 24;
    uint32_t fp = fingerprint(ies, ielen);
    uint8_t sl; const uint8_t *ss = ssid_of(ies, ielen, &sl);
    char tmp[33];
    if (ss && sl && sl <= 32) { memcpy(tmp, ss, sl); tmp[sl] = 0; }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    expo_probe(s_e, fp, (ss && sl) ? tmp : 0, sl, now);
}

void expo_sniff_start(exposure_t *e, uint32_t now_ms)
{
    s_e = e; s_hop = 0; s_last_hop = now_ms;
    expo_start(e, now_ms);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(rx_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(CH[0], WIFI_SECOND_CHAN_NONE);
    ESP_LOGW(TAG, "exposure sniff up");
}

void expo_sniff_tick(uint32_t now_ms)
{
    if (now_ms - s_last_hop >= 250) {         // hop ~4x/s to cover 1/6/11
        s_last_hop = now_ms;
        s_hop = (s_hop + 1) % (int)(sizeof CH);
        esp_wifi_set_channel(CH[s_hop], WIFI_SECOND_CHAN_NONE);
    }
}

void expo_sniff_stop(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   // back to the ESP-NOW channel
    s_e = 0;
    ESP_LOGW(TAG, "exposure sniff down");
}
