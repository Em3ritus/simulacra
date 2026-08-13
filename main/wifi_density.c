#include "wifi_density.h"
#include <string.h>
#include "freertos/FreeRTOS.h"

#define OBS_CAP 64
typedef struct { uint32_t hash; uint32_t last_ms; int used; } obs_e;

static obs_e    s_tbl[OBS_CAP];
static uint32_t s_salt;
static int      s_ewma_x16;   // EWMA of density, fixed-point (value << 4)

// s_tbl/s_ewma_x16 are written by wifi_obs_note() from rx_cb() (main/wifi_observe.c) -- the
// esp_wifi promiscuous-mode RX callback, invoked from IDF's own Wi-Fi task, not the caller's --
// and read/mutated by wifi_obs_density()/wifi_obs_target() from coexist_task. Same unsynchronized
// cross-task pattern already found and fixed in main/observe.c's BLE-side equivalent (s_obs_mux);
// missed here because it's a separate file from the callback site. All three functions below are
// pure in-memory struct/int mutation with no blocking call, so a tight spinlock is safe. s_salt is
// write-once before the callback is armed (wifi_obs_start() calls wifi_obs_reset() before
// esp_wifi_set_promiscuous(true)), so it doesn't need locking -- same reasoning as observe.c's
// s_salt.
static portMUX_TYPE s_wifi_obs_mux = portMUX_INITIALIZER_UNLOCKED;

void wifi_obs_reset(uint32_t salt)
{
    portENTER_CRITICAL(&s_wifi_obs_mux);
    memset(s_tbl, 0, sizeof s_tbl);
    s_ewma_x16 = 0;
    portEXIT_CRITICAL(&s_wifi_obs_mux);
    s_salt = salt;
}

static uint32_t hash_mac(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;             // FNV-1a offset basis, salted
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void wifi_obs_note(const uint8_t mac[6], uint32_t now_ms)
{
    uint32_t h = hash_mac(mac);                    // raw MAC consumed here; only the hash is kept
    portENTER_CRITICAL(&s_wifi_obs_mux);
    int slot = -1, oldest_i = 0, refresh = -1; uint32_t oldest = 0;
    for (int i = 0; i < OBS_CAP && refresh < 0; i++) {
        if (s_tbl[i].used && s_tbl[i].hash == h) { refresh = i; continue; }   // refresh, exit next check
        if (!s_tbl[i].used) { if (slot < 0) slot = i; continue; }
        uint32_t age = now_ms - s_tbl[i].last_ms;
        if (age >= WIFI_OBS_TTL_MS && slot < 0) slot = i;                    // reuse expired
        if (age > oldest) { oldest = age; oldest_i = i; }
    }
    if (refresh >= 0) {
        s_tbl[refresh].last_ms = now_ms;
    } else {
        if (slot < 0) slot = oldest_i;              // full of live entries: evict the oldest
        s_tbl[slot].used = 1; s_tbl[slot].hash = h; s_tbl[slot].last_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_wifi_obs_mux);
}

static int wifi_obs_density_locked(uint32_t now_ms)   // caller must hold s_wifi_obs_mux
{
    int n = 0;
    for (int i = 0; i < OBS_CAP; i++)
        if (s_tbl[i].used && (uint32_t)(now_ms - s_tbl[i].last_ms) < WIFI_OBS_TTL_MS) n++;
    return n;
}

int wifi_obs_density(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_wifi_obs_mux);
    int n = wifi_obs_density_locked(now_ms);
    portEXIT_CRITICAL(&s_wifi_obs_mux);
    return n;
}

int wifi_obs_target(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_wifi_obs_mux);
    int d = wifi_obs_density_locked(now_ms);
    // Floor the step magnitude to at least 1 (like rf_model.c's decayed() helper), not a plain
    // diff/4: that truncates toward zero, so a small nonzero diff (-3..-1 or 1..3) rounds to a 0
    // update and the accumulator sticks short of its true target forever -- the same dead-band
    // shape already found and fixed in rf_model_decay() (commit 95a79b9). Verified by hand-trace
    // (17,13,10,8,6,5,4,3,2,1,0,0,...) that this version reaches exactly 0, unlike a plain
    // truncating division (which sticks at 3) or a naive rounding division (which sticks at 1).
    // Currently masked downstream here by the floor-clamp/coarse final rounding below either way
    // (doesn't change the returned t for any case hand-checked), but fixing properly rather than
    // partially, since a future change to WIFI_OBS_FLOOR or the rounding constant could unmask it.
    int diff = (d << 4) - s_ewma_x16;
    if (diff != 0) {
        int step = diff / 4;
        if (step == 0) step = (diff > 0) ? 1 : -1;
        s_ewma_x16 += step;                        // EWMA alpha=1/4 (fixed-point)
    }
    int t = (s_ewma_x16 + 8) >> 4;                 // round to nearest
    portEXIT_CRITICAL(&s_wifi_obs_mux);
    if (t < WIFI_OBS_FLOOR) t = WIFI_OBS_FLOOR;
    if (t > WIFI_OBS_CAP)   t = WIFI_OBS_CAP;
    return t;
}
