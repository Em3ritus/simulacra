#include "sdkconfig.h"
#include "coexist.h"

// Print a confirmed threat's full MAC to serial. OFF by default: the rest of the project hashes
// bystander MACs at ingest and never persists them, so this is an opt-in exception for when you
// intend to go find the device. -DSIMULACRA_LOG_THREAT_MAC=1 to enable.
#ifndef SIMULACRA_LOG_THREAT_MAC
#define SIMULACRA_LOG_THREAT_MAC 0
#endif

#if CONFIG_IDF_TARGET_ESP32C5
static const coexist_persona_t s_persona = {       // Ward: dense, mains, dual-band, stationary
    .wifi_period_ms      = 2000,                    // heavier Wi-Fi (~2 s)
    .reprofile_period_ms = 600000,                  // 10 min
    .use_5g              = true,
    .drift_threshold     = 2.0f,                    // unreachable -> anti-entourage off
};
#else
static const coexist_persona_t s_persona = {       // Shade: lean, battery, 2.4-only, portable
    .wifi_period_ms      = 7000,                    // thin Wi-Fi (~6-8 s)
    .reprofile_period_ms = 300000,                  // 5 min
    .use_5g              = false,
    .drift_threshold     = 0.45f,                   // active anti-entourage
};
#endif

const coexist_persona_t *coexist_persona(void) { return &s_persona; }

coexist_due_t coexist_due(const coexist_persona_t *p, uint32_t now_ms,
                          uint32_t *last_wifi_ms, uint32_t *last_reprofile_ms)
{
    coexist_due_t d = { false, false };
    if (now_ms - *last_wifi_ms >= p->wifi_period_ms)            { d.fire_wifi = true;      *last_wifi_ms = now_ms; }
    if (now_ms - *last_reprofile_ms >= p->reprofile_period_ms)  { d.fire_reprofile = true; *last_reprofile_ms = now_ms; }
    return d;
}

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "churn.h"
#include "probe.h"
#include "phantom.h"
#include "drift.h"
#include "observe.h"
#include "generate.h"
#include "roster.h"
#include "rf_model.h"
#include "esp_random.h"
#include "detect.h"
#include "sig_class_name.h"
#include "wifi_observe.h"
#include "wifi_density.h"
#include "probe_agents.h"
#include "ble_devices.h"
#include "fleet_pop.h"
#include "surveil_oui.h"
#include "settings.h"
#include "config_wire.h"   // CONFIG_CLEAR_THREATS sentinel
#include <string.h>

static const char *TAG = "coexist";
static bool s_wifi_obs_started = false;   // wifi_obs_start called once
static bool s_wifi_obs_ok = false;        // promiscuous observe enabled (else use WIFI_OBS_FALLBACK)
#define OBS_REPROFILE_MS   15000
#define COEX_TICK_MS       250
#define COEX_5G_EVERY      4               // do a 5 GHz excursion every Nth Wi-Fi burst (keep it sparse)
#define SHADE_DRIFT_ACCEL  3.0f
#define SHADE_ACCEL_DECAY_MS 120000u       // ~2 min linear decay back to 1.0

static bool     s_wifi_ok;
static bool     s_wifi_allowed = true;    // webui: false defers Wi-Fi (STA) so the config AP can own it
static uint32_t s_wifi_ctr;
static uint32_t s_accel_until_ms;         // 0 = not accelerating
static int      s_listen_ch = -1;         // espnow: >=0 -> park Wi-Fi on this channel between bursts to listen
static bool     s_turbo;                  // TURBO preset active: every radio floods at hardware max
#define COEX_TURBO_SLICE_MS 250u          // BLE presentation cadence while turbo (vs CHURN_SLICE_MS=1000)

// --- M9 detection wiring ---
#define DETECT_EPOCH_DRIFT 0.45f           // detection-owned; separate from anti-entourage thresh
#define SURVEIL_CONF       85              // vendor-owned-OUI Wi-Fi surveillance hit confidence
#define COEX_SELF_MAX      16              // max decoy active-set MACs to self-exclude (Ward ceiling)
static uint16_t s_epoch;                   // location-epoch counter
static uint32_t s_detect_salt;             // per-install salt (stable across sweeps/reboots)
static struct { uint32_t hash; int8_t last_rssi; uint32_t last_ms; bool used; }
       s_locate[DETECT_MAX_THREATS];       // per-confirmed-threat locate-throttle state

uint16_t coexist_current_epoch(void) { return s_epoch; }

// Control-command inbox (see coexist.h). 0x100 = empty; the payload is a uint8 preset id, so the
// sentinel sits outside the byte range and a single int carries both state and value.
#define COEX_NO_REQ 0x100
static volatile int s_preset_req = COEX_NO_REQ;

void coexist_request_preset(uint8_t preset_id) { s_preset_req = (int)preset_id; }

static void coexist_drain_requests(void)
{
    int req = s_preset_req;
    if (req == COEX_NO_REQ) return;
    s_preset_req = COEX_NO_REQ;
    if (req == CONFIG_CLEAR_THREATS) {
        detect_clear_threats();
        ESP_LOGW(TAG, "control: threats cleared");
    } else if (sim_settings_apply_preset((sim_preset_t)req) == 0) {
        ESP_LOGW(TAG, "control: applied preset %d", req);
    }
}

void coexist_set_turbo(bool on)
{
    if (on == s_turbo) return;                        // idempotent
    s_turbo = on;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    ble_devices_set_turbo(on, now);
    probe_agents_set_turbo(on, now);
    churn_set_slice_ms(on ? COEX_TURBO_SLICE_MS : CHURN_SLICE_MS);
    if (on) {
        // Bypasses the fleet-share ceiling directly: every board floods at its OWN hardware max,
        // independent of room density or how many peers are heard. No persona coupling either --
        // personas exist to defeat single-radio-ghost filtering (an indistinguishability
        // mechanism), which is not the goal here, so release any currently bound.
        ble_devices_set_count(BLE_DEVICES_MAX, now);
        probe_agents_set_target(PROBE_AGENTS_MAX, now);
        phantom_set_count(0, now);
        ESP_LOGW(TAG, "TURBO: flooding at max (ble=%d wifi=%d)", BLE_DEVICES_MAX, PROBE_AGENTS_MAX);
    } else {
        // BLE already snaps back to the fleet-shared ceiling synchronously via
        // sim_settings_apply's churn_set_active_target -> ble_devices_set_count (called before this,
        // so ble_devices_count() below already reflects the new ceiling). Wi-Fi has no equivalent
        // path: probe_agents_count() would otherwise stay at PROBE_AGENTS_MAX until the next
        // reprofile (up to ~10 min), radiating a maxed, non-population-matched crowd after the
        // operator told the node to stand down. Re-arm the glide's target now, using the same
        // crowd/2 fleet-share cap the non-turbo Wi-Fi block computes every burst -- only the
        // *target* is re-armed immediately; the glide keeps stepping down gradually.
        int crowd = ble_devices_count();
        int cap   = crowd / 2; if (cap < 1) cap = 1;
        probe_agents_glide_set_target(cap, now);
        ESP_LOGW(TAG, "TURBO: off, resuming normal population-match (wifi glide -> %d)", cap);
    }
}

#if CONFIG_IDF_TARGET_ESP32C5
// C5 hard exclusion: tuning to 5 GHz means BLE (2.4 GHz) cannot TX. Inject a small ROTATING
// subset of the 5 GHz set per excursion (injecting all 8 back-to-back floods the Wi-Fi TX
// buffer -> ESP_ERR_NO_MEM/257 on the later channels), draining between channels, then retune
// to 2.4 GHz so BLE adv resumes. Full 5 GHz coverage rolls over several excursions (still sparse).
#define COEX_5G_PER_EXCURSION 2
static void coexist_5g_excursion(void)
{
    const uint8_t *ch5; size_t n5 = probe_channels_5g(&ch5);
    static size_t idx;
    for (int k = 0; k < COEX_5G_PER_EXCURSION && n5; k++, idx++) {
        if (k) vTaskDelay(pdMS_TO_TICKS(3));   // let the TX buffer drain between channels
        probe_inject_burst(ch5[idx % n5]);
    }
    const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
    if (n24) esp_wifi_set_channel(ch24[0], WIFI_SECOND_CHAN_NONE);   // back to 2.4 GHz
}
#else
static void coexist_5g_excursion(void) {}
#endif

static void coexist_handle_drift(const coexist_persona_t *p, float score)
{
#if !CONFIG_IDF_TARGET_ESP32C5                  // Shade (C6) only; Ward is stationary
    if (drift_exceeds(score, p->drift_threshold)) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        s_accel_until_ms = now + SHADE_ACCEL_DECAY_MS;
        churn_set_accel(SHADE_DRIFT_ACCEL);
        ESP_LOGW(TAG, "anti-entourage: drift=%.3f > %.2f -> accel=%.1f for %ums",
                 score, p->drift_threshold, SHADE_DRIFT_ACCEL, (unsigned)SHADE_ACCEL_DECAY_MS);
    }
#else
    (void)p; (void)score;
#endif
}

// --- M9 detector wiring adapter (impure glue; keeps detect.c pure) ---

// Build the self-exclusion set from the churn active identities (our own live decoy MACs).
static size_t coexist_self_macs(uint8_t out[][6], size_t max)
{
    size_t n = 0;
    for (size_t s = 0; s < churn_active_count() && n < max; s++) {
        const identity_t *id = churn_active_at(s);
        if (id) memcpy(out[n++], id->addr, 6);
    }
    return n;
}

// M9 per-install-salted FNV-1a over the MAC (stable across sweeps/reboots -- deliberate).
static uint32_t coexist_detect_hash(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_detect_salt;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

static void coexist_locate_emit(uint32_t hash, uint32_t id_prefix, int8_t rssi, uint32_t now_ms)
{
    int slot = -1;
    for (size_t i = 0; i < DETECT_MAX_THREATS; i++) {
        if (s_locate[i].used && s_locate[i].hash == hash) { slot = (int)i; break; }
        if (slot < 0 && !s_locate[i].used) slot = (int)i;
    }
    if (slot < 0) return;
    if (!s_locate[slot].used) {                       // first sighting since confirm -> emit + seed
        s_locate[slot].used = true; s_locate[slot].hash = hash;
        s_locate[slot].last_rssi = rssi; s_locate[slot].last_ms = now_ms;
        ESP_LOGW(TAG, "THREAT locate id=%04x rssi=%d seen=+0s", (unsigned)id_prefix, rssi);
        return;
    }
    if (detect_locate_due(rssi, s_locate[slot].last_rssi, now_ms, s_locate[slot].last_ms)) {
        ESP_LOGW(TAG, "THREAT locate id=%04x rssi=%d seen=+%us", (unsigned)id_prefix, rssi,
                 (unsigned)((now_ms - s_locate[slot].last_ms) / 1000));
        s_locate[slot].last_rssi = rssi; s_locate[slot].last_ms = now_ms;
    }
}

// Registered on observe: fires for every raw report (NimBLE host-task context).
static void coexist_on_report(const uint8_t mac[6], int8_t rssi, uint16_t company,
                              const sig_hit_t *hit)
{
    if (!detect_enabled()) return;
    // Self-exclusion set, cached. This runs on the NimBLE host task for EVERY advert -- in a dense
    // room, hundreds a second -- and used to rebuild the whole 16-entry MAC array each time even
    // though the on-air set only changes when churn re-applies a slot. Both the cache and its
    // generation stamp are touched only from this callback, so there is no cross-task write here;
    // churn_apply_gen() is a plain monotonic read.
    static uint8_t  s_self[COEX_SELF_MAX][6];
    static size_t   s_nself;
    static uint32_t s_self_gen = 0xFFFFFFFFu;               // force a build on the first advert
    uint32_t gen = churn_apply_gen();
    if (gen != s_self_gen) { s_nself = coexist_self_macs(s_self, COEX_SELF_MAX); s_self_gen = gen; }
    if (detect_mac_in_set(mac, s_self, s_nself)) return;    // never flag our own decoys

    uint32_t hash = coexist_detect_hash(mac);
    uint16_t epoch = s_epoch;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (hit) {                                              // M10 fingerprint: known device class
        if (detect_note_known(hash, rssi, hit->class_id, hit->category, hit->confidence, epoch)
                == DETECT_CONFIRM)
            ESP_LOGW(TAG, "KNOWN %s id=%04x conf=%u rssi=%d", sig_class_name(hit->class_id),
                     (unsigned)(hash & 0xFFFF), (unsigned)hit->confidence, rssi);
    }
    detect_result_t r = detect_observe(hash, rssi, company, epoch);
    if (r == DETECT_CONFIRM) {
        // Everywhere else a bystander's MAC is hashed on ingest and never stored. This one line
        // would print it in full, which is defensible for a detector (you often want the MAC to go
        // find the device) but is a deliberate exception to the project's own model — so it is a
        // build-time choice, not a default. Enable with -DSIMULACRA_LOG_THREAT_MAC=1.
#if SIMULACRA_LOG_THREAT_MAC
        ESP_LOGW(TAG, "THREAT confirmed id=%04x vendor=0x%04x epochs=%u rssi=%d "
                      "mac=%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)(hash & 0xFFFF), company, DETECT_EPOCH_STRIKES, rssi,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#else
        ESP_LOGW(TAG, "THREAT confirmed id=%04x vendor=0x%04x epochs=%u rssi=%d",
                 (unsigned)(hash & 0xFFFF), company, DETECT_EPOCH_STRIKES, rssi);
#endif
    } else if (r == DETECT_KNOWN) {
        coexist_locate_emit(hash, hash & 0xFFFF, rssi, now);
    }
}

// Optional status LED: slow-blink while >=1 confirmed threat is active. Board-gated -- compiled
// out unless SIMULACRA_DETECT_LED_GPIO is defined (LED wiring differs across boards).
#ifdef SIMULACRA_DETECT_LED_GPIO
#include "driver/gpio.h"
static void coexist_detect_led_init(void)
{
    gpio_reset_pin((gpio_num_t)SIMULACRA_DETECT_LED_GPIO);
    gpio_set_direction((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, GPIO_MODE_OUTPUT);
}
static void coexist_detect_led_tick(uint32_t now_ms)
{
    static uint32_t last; static int on;
    if (detect_threat_count() == 0) { gpio_set_level((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, 0); return; }
    if (now_ms - last >= 500) { on = !on; last = now_ms;                 // slow blink = threat active
        gpio_set_level((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, on); }
}
#else
static inline void coexist_detect_led_init(void) {}
static inline void coexist_detect_led_tick(uint32_t now_ms) { (void)now_ms; }
#endif

// Re-profile is split across ticks: _start opens the observation window and snapshots the model,
// _finish runs when the window closes. In between, coexist_task keeps ticking churn, draining the
// detector and injecting probes — none of which used to happen during the 15 s scan.
static rf_model_t s_repro_prev;        // pre-window snapshot (static: ~1 KB, too big for the stack)
static bool       s_repro_active;

static void coexist_reprofile_start(void)
{
    if (s_repro_active) return;                             // a window is already open
    s_repro_prev   = *observe_model();                      // snapshot pre-update
    s_repro_active = true;
    observe_window_begin(OBS_REPROFILE_MS);                 // ~15 s scan while advertising
}

static void coexist_reprofile_finish(const coexist_persona_t *p)
{
    const rf_model_t prev = s_repro_prev;
    const rf_model_t *cur = observe_model();
    if (cur->total_obs < GEN_MIN_OBS) {                     // too sparse -> keep current population
        ESP_LOGW(TAG, "reprofile: total_obs=%u < %d -> skip reshape",
                 (unsigned)cur->total_obs, GEN_MIN_OBS);
        return;
    }
    float score = drift_score(&prev, cur);
    if (prev.sweeps > 0 && score > DETECT_EPOCH_DRIFT) {    // materially new room -> new location-epoch
        s_epoch++;
        detect_on_epoch_change(s_epoch);
        ESP_LOGW(TAG, "epoch -> %u (drift=%.3f)", (unsigned)s_epoch, score);
    }
    roster_reseed(cur);                                     // fresh room-matched behaviour library
    // Room density flexes the crowd, but never below what this node's designed persona count needs
    // (personas are capped at half the crowd, so N personas require 2N devices). GEN_CEILING caps
    // the density estimate at 8 on Shade / 16 on Ward, which on its own would shrink the crowd to
    // the point where personas get squeezed out -- the phones we present are a design constant of
    // the node, not a property of the room; it is the unbound beacons/tags that should flex.
    // generate_active_target estimates the density of the WHOLE room; this node presents its 1/K
    // share of it. The boot path already did this and the re-profile did not, so the crowd snapped
    // back to a full standalone-sized population at the first re-profile.
    uint8_t at = (uint8_t)fleet_pop_share(generate_active_target(cur));
    uint8_t floor_n = sim_settings_floor();
    if (at < floor_n) at = floor_n;
    churn_set_active_target(at);                            // resize to the new population
    ESP_LOGW(TAG, "reprofile: drift=%.3f active_target=%u (floor %u, fleet_k %d)",
             score, (unsigned)at, (unsigned)floor_n, fleet_pop_size());
    if (prev.sweeps > 0) coexist_handle_drift(p, score);   // skip day-one false trigger (empty prev model)
}

static void coexist_decay_accel(void)
{
    if (s_accel_until_ms == 0) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now >= s_accel_until_ms) {
        churn_set_accel(1.0f);
        s_accel_until_ms = 0;
        ESP_LOGW(TAG, "anti-entourage: accel decayed to 1.0");
        return;
    }
    float frac = (float)(s_accel_until_ms - now) / (float)SHADE_ACCEL_DECAY_MS;   // 1.0 -> 0
    churn_set_accel(1.0f + (SHADE_DRIFT_ACCEL - 1.0f) * frac);                    // linear decay
}

static void coexist_task(void *arg)
{
    (void)arg;
    const coexist_persona_t *p = coexist_persona();
    uint32_t now0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_wifi = now0, last_repro = now0;      // don't fire at the instant of boot
    uint32_t hop24 = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        fleet_pop_refresh(now);                         // live node census -> everyone's 1/K share
        {   // A node joining or leaving changes everyone's share; resize now rather than waiting
            // for the next re-profile (up to 10 min on Ward).
            static int s_last_k = -1;
            int k = fleet_pop_size();
            if (k != s_last_k) {
                if (s_last_k >= 0) ESP_LOGW(TAG, "fleet census %d -> %d nodes; resizing crowd", s_last_k, k);
                s_last_k = k;
                if (!s_turbo) sim_settings_recalc_bounds();   // turbo's bounds aren't K-relative
            }
        }
        coexist_drain_requests();                       // control commands land here, not on the
        churn_tick(now);                                // caller's task (single-writer discipline)
        if (s_turbo) {                                  // re-assert: a radio re-init (probe_pool_init
            // on boot-restore or a WEBUI wifi re-enable) can silently reset either count while
            // TURBO stays flagged on. Both calls are idempotent no-ops once at the ceiling.
            if (ble_devices_count()  < BLE_DEVICES_MAX)  ble_devices_set_count(BLE_DEVICES_MAX, now);
            if (probe_agents_count() < PROBE_AGENTS_MAX) probe_agents_set_target(PROBE_AGENTS_MAX, now);
        }
        coexist_decay_accel();
        detect_threat_t nt;
        if (detect_drain_pending(&nt)) {                // persist a new confirmation off the BLE callback
            int rc = detect_save_nvs();
            ESP_LOGW(TAG, "THREAT persisted id=%04x rc=%d", (unsigned)(nt.hash & 0xFFFF), rc);
        }
        {                                               // Wi-Fi surveillance-OUI hits (RX thread) -> detector
            uint32_t sh; int8_t sr; uint8_t sc, sk;
            while (surveil_next(&sh, &sr, &sc, &sk)) {
                if (detect_note_known(sh, sr, sk, sc, SURVEIL_CONF, s_epoch) == DETECT_CONFIRM)
                    ESP_LOGW(TAG, "SURVEILLANCE %s id=%04x rssi=%d", sig_class_name(sk),
                             (unsigned)(sh & 0xFFFF), sr);
            }
        }
        coexist_detect_led_tick(now);
        coexist_due_t d = coexist_due(p, now, &last_wifi, &last_repro);
        // s_wifi_allowed as well as s_wifi_ok: the setter's false path used to write a flag the
        // tick never read, so coexist_set_wifi_enabled(false) after start silently kept injecting.
        //
        // Also held off while the re-profile scan is open. The scan used to block this whole task,
        // so it was implicitly Wi-Fi-silent; now that the tick keeps running, injecting during the
        // window would steal antenna time from the BLE scan (on Ward a 5 GHz excursion stops BLE TX
        // outright) and the density we measure would come out low. Keep the measurement clean —
        // churn, the detector drain and threat persistence still run, which is the point.
        // s_turbo ORs into the gate: while turbo is active, fire on EVERY tick (COEX_TICK_MS = 250
        // ms) rather than waiting for the persona's normal wifi_period_ms (2-7s). This is the real
        // Wi-Fi throughput lever, the same way churn_set_slice_ms is the real BLE lever.
        if ((d.fire_wifi || s_turbo) && s_wifi_ok && s_wifi_allowed && !observe_window_active()) {
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            if (!s_turbo) {
                probe_agents_glide_tick(now);                         // ramp applied pop toward target
                // The glide moves the Wi-Fi agent count to match room density; the persona registry
                // must follow it. Personas beyond the agent count would advertise a phone on BLE
                // that never probes on Wi-Fi, and agents beyond the persona count would have no BLE
                // twin and no lifecycle here (probe_agents_lifecycle is SIMULACRA_PROBE-only), so
                // they would never age out. Keeping the counts equal preserves the
                // one-device-two-radios invariant. Personas may never fill the whole BLE crowd: a
                // crowd that is 100% phone-shaped personas is a monoculture (every device company
                // 0x0000, no beacons, no tags), which is a stronger tell than any single device.
                // Cap them at half the population and pull the Wi-Fi agent set down to match.
                // TURBO skips all of this -- it doesn't use personas at all (see coexist_set_turbo).
                int crowd = ble_devices_count();
                int cap   = crowd / 2;  if (cap < 1) cap = 1;
                if (probe_agents_count() > cap) probe_agents_set_target(cap, now);
                phantom_set_count(probe_agents_count(), now);
                phantom_sync_wifi(now);                               // agents track persona lives
            }
            probe_agents_rotate_tick(now);        // intra-life MAC rotation: without this a persona
                                                  // holds ONE Wi-Fi MAC for its whole life while its
                                                  // BLE RPA rotates — the mismatch is the tell.
                                                  // probe_agents_lifecycle is standalone-only.
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
        if (!s_wifi_ok || !s_wifi_allowed) {
            // No Wi-Fi means no probe requests. A persona is a phone presenting on both radios, so
            // with one radio gone every persona would be a BLE "phone" that has never probed for a
            // network -- more conspicuous than not presenting phones at all. Release them; the
            // slots rejoin the unbound crowd as ordinary beacons/tags, which is still plausible.
            phantom_set_count(0, now);
        }
        if (s_wifi_ok && s_wifi_allowed && !s_wifi_obs_started) {
            s_wifi_obs_ok = wifi_obs_start();       // enable promiscuous once the STA/injection side is up
            s_wifi_obs_started = true;
        }
        if (d.fire_reprofile && !s_turbo) coexist_reprofile_start();   // turbo owns its own population
        if (s_repro_active && observe_window_poll(now)) {           // window closed -> reshape
            s_repro_active = false;
            coexist_reprofile_finish(p);                            // BLE population-match (may early-return)
            int wt      = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
            int k       = fleet_pop_live_size(now);                 // live fleet size (peers heard + self)
            int agents  = fleet_pop_share_k(wt, k);                 // this node's share of the crowd target
            probe_agents_glide_set_target(agents, now);             // glide toward it (boot-instant first time)
            // Log the APPLIED target + the divisor so the live census is observable (wt is pre-division).
            ESP_LOGW(TAG, "wifi popmatch: density=%d wt=%d /nodes=%d -> agents=%d%s",
                     s_wifi_obs_ok ? wifi_obs_density(now) : -1, wt, k, agents,
                     s_wifi_obs_ok ? "" : " (fallback)");
        }
        if (s_listen_ch >= 0 && s_wifi_ok && s_wifi_allowed && !observe_window_active())
            esp_wifi_set_channel((uint8_t)s_listen_ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
    }
}

void coexist_set_listen_channel(int ch)
{
    s_listen_ch = ch;      // -1 disables; >=0 makes the coexist tick return the radio here between bursts
}

void coexist_set_wifi_enabled(bool en)
{
    if (en && !s_wifi_ok) {                       // bring Wi-Fi (STA) up now that the AP is down
        int rc = probe_wifi_init();
        if (rc == 0) {
            probe_pool_init();       // init agents BEFORE publishing wifi-ready to coexist_task,
            s_wifi_ok = true;        // so the tick can't fire phantom_sync_wifi mid-agents-init
        }
        ESP_LOGW(TAG, "coexist: wifi enabled post-config rc=%d", rc);
    }
    s_wifi_allowed = en;
}

void coexist_start(void)
{
    if (s_wifi_allowed) {
        int rc = probe_wifi_init();
        if (rc == 0) { probe_pool_init(); s_wifi_ok = true;    // (pre-spawn here, but keep the
                       ESP_LOGW(TAG, "coexist: wifi up -> BLE + Wi-Fi combined decoy"); }
        else         { s_wifi_ok = false;                      //  publish-after-init order uniform)
                       ESP_LOGE(TAG, "coexist: wifi init rc=%d -> BLE-only fallback", rc); }
    } else {
        s_wifi_ok = false;
        ESP_LOGW(TAG, "coexist: wifi deferred (config window) -> BLE-only for now");
    }
    observe_reprofile_init(esp_random());
    s_detect_salt = detect_load_salt();          // M9: stable per-install salt
    surveil_init(esp_random());                   // per-session salt for the Wi-Fi surveillance hits
    detect_begin_session();                       // escalation: bump + load the persistent boot-session id
    detect_load_nvs();                            // restore previously-confirmed threats (best-effort)
    observe_set_report_cb(coexist_on_report);     // subscribe the detector to raw reports
    coexist_detect_led_init();
    BaseType_t ok = xTaskCreate(coexist_task, "coexist", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "coexist_task create failed -> BLE-only emergency loop");
        for (;;) {                    // never brick: keep the BLE decoy advertising
            churn_tick((uint32_t)(esp_timer_get_time() / 1000));
            vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
        }
    }
}
