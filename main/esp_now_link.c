#include "esp_now_link.h"
#include <string.h>

void espnow_status_from_webui(radar_wire_status_t *out, const webui_status_t *in)
{
    memset(out, 0, sizeof(*out));
    out->uptime_s = in->uptime_s;
    out->flags = (uint8_t)((in->decoy_paused ? 0x1 : 0) | (in->wifi_config_mode ? 0x2 : 0)
                           | (in->tx_degraded ? 0x4 : 0)      // bit2: probe TX wedged (health)
                           | (in->battery_low ? 0x8 : 0)      // bit3: fuel-gauge SoC low
                           | (in->model_saturated ? 0x10 : 0));  // bit4: density under-reported
    out->active_devices = in->active_devices; out->roster_size = in->roster_size;
    out->probes_sent = in->probes_sent; out->epoch = in->epoch; out->pop_ewma = in->pop_ewma;
    out->total_obs = in->total_obs; out->active_target = in->active_target;
    uint8_t n = in->threat_count; if (n > RADAR_MAX_THREATS) n = RADAR_MAX_THREATS;
    out->threat_count = n;
    for (uint8_t i = 0; i < n; i++) {
        out->threats[i].hash = in->threats[i].hash;
        out->threats[i].vendor = in->threats[i].vendor;
        out->threats[i].epochs = in->threats[i].epochs;
        out->threats[i].best_rssi = in->threats[i].best_rssi;
        out->threats[i].first_epoch = in->threats[i].first_epoch;
        out->threats[i].last_epoch = in->threats[i].last_epoch;
        out->threats[i].kind = in->threats[i].kind;
        out->threats[i].class_id = in->threats[i].class_id;
        out->threats[i].category = in->threats[i].category;
        out->threats[i].confidence = in->threats[i].confidence;
        out->threats[i].sessions_seen = in->threats[i].sessions_seen;
        out->threats[i].places_seen = in->threats[i].places_seen;
    }
    out->form_restless = in->form_restless; out->form_wandering = in->form_wandering; out->form_bound = in->form_bound;
    out->battery_mv = in->battery_mv; out->battery_pct = in->battery_pct;
}

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radar_key.h"
#include "coexist.h"
#include "churn.h"
#include "probe_agents.h"
#include "learn.h"
#include "learn_wire.h"
#include "sig_wire.h"
#include "sig_store.h"
#include "fleet.h"
#include "config_wire.h"
#include "detect.h"
#include "sim_ctrl_key.h"
#include "settings.h"
#include "fleet_key.h"
#include "enroll_wire.h"
#include "nvs.h"

#ifndef SIMULACRA_ESPNOW_CHANNEL
#define SIMULACRA_ESPNOW_CHANNEL 1
#endif
static const char *ETAG = "espnow";
static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static uint8_t   s_salt[RADAR_SALT_LEN];
static uint64_t  s_counter;
static radar_replay_t s_req_replay;                 // reject replayed requests
static radar_replay_t s_sync_replay;                // reject replayed LEARN_SYNC from Vigil
static radar_replay_t s_sig_replay;                 // reject replayed SIG_SYNC from Vigil
#ifdef SIMULACRA_CONFIG_CTRL
// CONFIG replay floor: highest counter ever accepted from a *signed* command, persisted so a
// power-cycle doesn't reopen the window (the in-RAM radar_replay_t gates are telemetry-only).
// Reused NVS namespace: FLEET_NVS_NS, alongside k_epoch.
#define CFG_FLOOR_KEY "cfg_ctr"
static uint64_t s_cfg_floor;

static void cfg_floor_load(void)
{
    nvs_handle_t h;
    if (nvs_open(FLEET_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // never set yet -> floor 0
    nvs_get_u64(h, CFG_FLOOR_KEY, &s_cfg_floor);
    nvs_close(h);
}

// Returns true only if the floor genuinely reached flash. The caller treats false as "don't act on
// this command" -- an NVS failure here means the "durable BEFORE acting" comment at the call site
// isn't actually true, and a reboot right after would let the exact same signed frame replay once
// more. Rare (needs a flash write failure) but this is the one NVS write this codebase explicitly
// frames as a security control, so it's worth refusing to fake success on.
static bool cfg_floor_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(FLEET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u64(h, CFG_FLOOR_KEY, s_cfg_floor);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);                     // one write per accepted control command; control-plane rates
    return e == ESP_OK;               // are minutes apart, so flash wear is not a concern
}
#endif
static bool s_answer;   // a REQUEST was seen this drain; answered once after it, so a burst of
                        // retransmits (the Vigil sends 3x) produces one reply, not three

// SIG_SYNC reassembly: accumulate one content_version's chunks, then adopt wholesale.
static threat_sig_t s_sig_rx[SIG_DB_CAP];
static uint16_t     s_sig_rx_ver;                   // version currently being assembled (0 = none)
static uint8_t      s_sig_rx_mask;                  // bit i set once chunk i received (<= 8 chunks)
static uint8_t      s_sig_rx_cnt;                   // expected chunk_count
static size_t       s_sig_rx_n;                     // records placed so far

// RX hand-off ring. The ESP-NOW receive callback runs in the Wi-Fi driver task, where IDF expects
// you to return promptly and not call back into esp_now_send. Everything the old callback did --
// AES-GCM open, record unpacking, learn-store and signature-DB mutation, and (during enrollment)
// a transmit -- now happens on espnow_task; the callback only copies bytes in here.
//
// Single producer (Wi-Fi task) and single consumer (espnow_task), so plain volatile indices are
// sufficient: each side advances only its own index, and a torn read of the other's cannot produce
// a false "has data" (worst case the consumer sees an item one drain late).
// 32 slots, not 8. Measured on the live fleet: a 3-node fleet plus the Vigil bursts well past 8
// frames between drains -- STATUS is sent 3x and REQUEST 4x by design (redundancy over a lossy
// broadcast), and learn/sig sync arrive in chunk trains. At 8 the ring overflowed within seconds
// (15 dropped in one burst on the Vigil). Dropping is safe for the redundant telemetry but would
// silently discard a CONFIG command, so size for the real burst instead.
#define RX_RING_N 32
typedef struct { uint8_t data[RADAR_FRAME_MAX]; uint16_t len; uint8_t src[6]; } rx_item_t;
static rx_item_t       s_rx_ring[RX_RING_N];
static volatile uint8_t s_rx_head, s_rx_tail;   // head = next write, tail = next read
static uint32_t        s_rx_dropped;            // ring full: telemetry bursts are re-sent 3x anyway

// Open a sealed frame with the current fleet key, then the previous (rotation grace).
int espnow_open_any(const uint8_t *frame, size_t flen, uint8_t *out_type, uint8_t *payload,
                    size_t payload_cap, size_t *payload_len, uint8_t out_salt[RADAR_SALT_LEN],
                    uint64_t *out_counter)
{
    const uint8_t *k = fleet_key_get();
    if (k && radar_wire_open(frame, flen, k, out_type, payload, payload_cap, payload_len,
                             out_salt, out_counter) == 0)
        return 0;
    const uint8_t *kp = fleet_key_prev();
    if (kp && radar_wire_open(frame, flen, kp, out_type, payload, payload_cap, payload_len,
                              out_salt, out_counter) == 0)
        return 0;
    return -1;
}

#ifdef SIMULACRA_FLEET_PROVISION
// Seek-enrollment state: set while we await a GRANT for a REQUEST we sent.
static uint8_t s_enr_nonce_d[24];   // our per-session nonce (bound into the GRANT)
static uint8_t s_enr_veph[32];      // Vigil ephemeral pubkey from the OFFER we answered
static bool    s_enr_pending;

// An un-enrolled decoy answers ANY signature-valid OFFER, unconditionally, forever -- that's the
// whole point while it's actively seeking enrollment (the Vigil re-broadcasts the SAME OFFER, same
// nonce_v, once a second for its whole 30s pairing window, and this decoy must be able to answer
// whichever resend actually gets through if an earlier REQUEST was lost). But with no bound at all,
// an attacker who captures one signed OFFER can replay it days or weeks later and the decoy will
// answer again with its permanent, never-rotating id_pk in plaintext -- turning an anti-tracking
// device into a stable tracking beacon for anyone who recorded one pairing window. Bound each
// nonce_v to a generous local answer window (comfortably longer than the Vigil's real 30s window,
// so every legitimate resend still gets answered) and refuse it once that elapses.
#define ENR_NONCE_HISTORY        8
#define ENR_NONCE_ANSWER_MS  60000u
typedef struct { uint8_t nv[24]; uint32_t first_seen_ms; bool used; } enr_nonce_rec_t;
static enr_nonce_rec_t s_enr_nv[ENR_NONCE_HISTORY];
static int s_enr_nv_head;

static bool enr_nonce_answerable(const uint8_t nv[24], uint32_t now_ms)
{
    for (int i = 0; i < ENR_NONCE_HISTORY; i++) {
        if (s_enr_nv[i].used && memcmp(s_enr_nv[i].nv, nv, 24) == 0)
            return (uint32_t)(now_ms - s_enr_nv[i].first_seen_ms) < ENR_NONCE_ANSWER_MS;
    }
    memcpy(s_enr_nv[s_enr_nv_head].nv, nv, 24);
    s_enr_nv[s_enr_nv_head].first_seen_ms = now_ms;
    s_enr_nv[s_enr_nv_head].used = true;
    s_enr_nv_head = (s_enr_nv_head + 1) % ENR_NONCE_HISTORY;
    return true;
}

// Handle a raw (unsealed) enrollment frame: [type(1) | enroll payload].
static void enroll_on_frame(const uint8_t *data, int len)
{
    if (data[0] == RADAR_TYPE_ENROLL_OFFER) {
        if (len != 1 + ENROLL_OFFER_LEN) return;
        uint8_t veph[32], nv[24]; uint32_t epoch;
        if (enroll_offer_open(data + 1, ENROLL_OFFER_LEN, SIMULACRA_CTRL_PK, veph, nv, &epoch) != 0)
            return;                                          // not a genuine Vigil offer
        // Answer when unenrolled, or when the offer carries a newer key (rotation). A same-or-
        // older epoch offer to an already-keyed decoy is ignored (replay / steady state).
        if (fleet_key_have() && epoch <= fleet_key_epoch()) return;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (!enr_nonce_answerable(nv, now_ms)) return;        // stale/replayed challenge -- refuse
        esp_fill_random(s_enr_nonce_d, 24);                  // fresh session
        memcpy(s_enr_veph, veph, 32);
        uint8_t idsk[32]; fleet_id_sk(idsk);
        uint8_t frame[1 + ENROLL_REQUEST_LEN];
        frame[0] = RADAR_TYPE_ENROLL_REQUEST;
        if (enroll_request_build(frame + 1, ENROLL_REQUEST_LEN, fleet_id_pk(), idsk,
                                 s_enr_nonce_d, veph, nv) != ENROLL_REQUEST_LEN) return;
        esp_now_send(BCAST, frame, sizeof frame);
        s_enr_pending = true;
        ESP_LOGW(ETAG, "enroll: answered OFFER (epoch %u) -> sent REQUEST", (unsigned)epoch);
        return;
    }
    if (data[0] == RADAR_TYPE_ENROLL_GRANT) {
        if (!s_enr_pending || len != 1 + ENROLL_GRANT_LEN) return;
        uint8_t idsk[32]; fleet_id_sk(idsk);
        uint8_t key[32], nd_echo[24]; uint32_t epoch;
        if (enroll_grant_open(data + 1, ENROLL_GRANT_LEN, s_enr_veph, idsk, key, &epoch, nd_echo) != 0)
            return;
        if (memcmp(nd_echo, s_enr_nonce_d, 24) != 0) return; // not our session (replay/stale)
        fleet_key_set(key, epoch);
        s_enr_pending = false;
        ESP_LOGW(ETAG, "enroll: GRANT accepted -> fleet key set (epoch %u)", (unsigned)epoch);
        return;
    }
}
#endif

// Full frame processing. Runs on espnow_task, NEVER in the RX callback: it opens AES-GCM,
// unpacks records, mutates the learn store and the signature-DB reassembly buffer, and may
// transmit. Doing that in the callback blocked the Wi-Fi driver task and mutated state the
// coexist task reads concurrently. `src` is copied from the recv info by the callback.
static void handle_frame(const uint8_t *data, int len, const uint8_t src[6])
{
#ifdef SIMULACRA_FLEET_PROVISION
    if (len >= 1 && (data[0] == RADAR_TYPE_ENROLL_OFFER || data[0] == RADAR_TYPE_ENROLL_GRANT)) {
        enroll_on_frame(data, len);                 // raw enrollment frame, not sealed
        return;
    }
#endif
    if (len < 0) return;                            // driver contract; keeps the cast below honest
    uint8_t type, pl[RADAR_FRAME_MAX], salt[RADAR_SALT_LEN]; size_t plen; uint64_t ctr;
    if (espnow_open_any(data, (size_t)len, &type, pl, sizeof pl, &plen, salt, &ctr) != 0)
        return;                                     // not ours / bad tag / oversized (ESP-NOW v2)
    if (type == RADAR_TYPE_REQUEST) {
        if (!radar_replay_ok(&s_req_replay, salt, ctr)) return;   // replayed request
        s_answer = true;                            // answered by the caller after this returns
        return;
    }
    if (type == RADAR_TYPE_LEARN_SYNC) {
        if (!radar_replay_ok(&s_sync_replay, salt, ctr)) return;
        learn_chunk_hdr_t h; learned_template_t rx[LEARN_WIRE_RECS_PER_CHUNK]; uint8_t nr;
        if (learn_wire_unpack(pl, plen, rx, &nr, &h) != 0) return;
        for (uint8_t i = 0; i < nr; i++) learn_ingest_wire(&rx[i]);   // regate inside
        return;
    }
    if (type == RADAR_TYPE_SIG_SYNC) {
        if (!radar_replay_ok(&s_sig_replay, salt, ctr)) return;
        sig_chunk_hdr_t h; threat_sig_t recs[SIG_WIRE_RECS_PER_CHUNK]; uint8_t nr;
        if (sig_wire_unpack(pl, plen, recs, &nr, &h) != 0) return;
        if (h.chunk_count == 0 || h.chunk_count > 8 || h.chunk_index >= h.chunk_count) return;
        if (h.content_version != s_sig_rx_ver) {          // new version -> restart assembly
            s_sig_rx_ver = h.content_version; s_sig_rx_mask = 0; s_sig_rx_cnt = h.chunk_count; s_sig_rx_n = 0;
        }
        size_t off = (size_t)h.chunk_index * SIG_WIRE_RECS_PER_CHUNK;
        for (uint8_t i = 0; i < nr && off + i < SIG_DB_CAP; i++) s_sig_rx[off + i] = recs[i];
        s_sig_rx_mask |= (uint8_t)(1u << h.chunk_index);
        if (off + nr > s_sig_rx_n) s_sig_rx_n = off + nr;
        if (s_sig_rx_n > SIG_DB_CAP) s_sig_rx_n = SIG_DB_CAP;   // the copy above clamps writes;
                                                                // clamp the count too, or adopt
                                                                // would read past s_sig_rx[]
        uint8_t full = (uint8_t)((1u << s_sig_rx_cnt) - 1);
        if ((s_sig_rx_mask & full) == full) {             // all chunks in -> re-gate + adopt
            if (sig_store_adopt(s_sig_rx, s_sig_rx_n, s_sig_rx_ver))
                ESP_LOGW(ETAG, "sig: adopted DB v%u (%u sigs)", (unsigned)s_sig_rx_ver,
                         (unsigned)sig_store_count());
            s_sig_rx_ver = 0; s_sig_rx_mask = 0;          // reset for the next announce
        }
        return;
    }
    if (type == RADAR_TYPE_FLEET_MACS) {                   // a peer decoy's active synthetic MACs
        uint8_t macs[FLEET_MAC_CAP][6];
        size_t nm = fleet_macs_unpack(pl, plen, macs, FLEET_MAC_CAP);
        if (nm) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            fleet_note_peer_macs(macs, nm, now);
            fleet_note_peer_node(src, now);              // real hardware sender identity -> live census
            ESP_LOGW(ETAG, "fleet: peer +%u macs (peers=%u)", (unsigned)nm, (unsigned)fleet_peer_count(now));
        }
        return;
    }
#ifdef SIMULACRA_CONFIG_CTRL
    if (type == RADAR_TYPE_CONFIG) {                       // Vigil -> decoy: signed settings preset
        uint8_t nonce12[12];                              // salt(8) || counter(4 BE), must match
        memcpy(nonce12, salt, RADAR_SALT_LEN);            // the Vigil's signing nonce exactly
        for (int i = 0; i < 4; i++) nonce12[RADAR_SALT_LEN + i] = (uint8_t)(ctr >> (24 - 8 * i));
        config_cmd_t cmd;
        if (config_wire_open_signed(pl, plen, nonce12, SIMULACRA_CTRL_PK, &cmd) != 0) return;  // bad sig
        if (cmd.version != CONFIG_WIRE_VER) return;
        // Signature verified FIRST, then the reboot-proof monotonic gate: the signature covers
        // salt||counter, i.e. exactly the material a replayer resends, so it proves authorship but
        // never freshness. Advancing the floor only on signed frames stops an unsigned flood from
        // jamming it to UINT64_MAX and deafening the control channel for good.
        if (!radar_replay_monotonic_ok(&s_cfg_floor, ctr)) return;      // stale / replayed command
        if (!cfg_floor_persist()) {          // durable BEFORE acting: a reboot between applying the
                                             // command and persisting the floor would re-open the
                                             // replay window for the command just executed. If the
                                             // write itself failed, don't act on the command at all
                                             // -- the RAM floor is already advanced (so a same-
                                             // session replay is still blocked), refusing here only
                                             // costs one legitimate command under a flash fault.
            ESP_LOGW(ETAG, "config: floor persist failed -- refusing to apply (flash write error)");
            return;
        }
        // Queued, not applied: presets resize the BLE population and clear_threats memsets the
        // detector table, and coexist_task is the single writer of both.
        coexist_request_preset(cmd.preset_id);
        ESP_LOGW(ETAG, "config: queued %s",
                 cmd.preset_id == CONFIG_CLEAR_THREATS ? "CLEAR THREATS" : "preset");
        return;
    }
#endif
}

// The ONLY thing that runs in the Wi-Fi driver task: bounds-check and copy. No crypto, no state.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > RADAR_FRAME_MAX) return;      // v1 cap; an ESP-NOW v2 frame cannot be ours
    uint8_t head = s_rx_head, next = (uint8_t)((head + 1) % RX_RING_N);
    if (next == s_rx_tail) { s_rx_dropped++; return; }  // full: drop, don't block the driver
    rx_item_t *it = &s_rx_ring[head];
    memcpy(it->data, data, (size_t)len);
    it->len = (uint16_t)len;
    if (info) memcpy(it->src, info->src_addr, 6); else memset(it->src, 0, 6);
    s_rx_head = next;                                   // publish last: the item is complete first
}

static void espnow_drain_rx(void)
{
    while (s_rx_tail != s_rx_head) {
        rx_item_t *it = &s_rx_ring[s_rx_tail];
        handle_frame(it->data, (int)it->len, it->src);
        s_rx_tail = (uint8_t)((s_rx_tail + 1) % RX_RING_N);
    }
    if (s_rx_dropped) {
        ESP_LOGW(ETAG, "rx: dropped %u frame(s) (ring full)", (unsigned)s_rx_dropped);
        s_rx_dropped = 0;
    }
}

// Broadcast our current active synthetic MACs so fleet-mates can self-exclude us from their
// model / learn / detect. AES-GCM authenticated (only fleet PSK holders can read/emit it).
static void broadcast_fleet_macs(void)
{
    uint8_t macs[FLEET_BCAST_MACS_MAX][6]; size_t n = 0;
    for (size_t s = 0; s < churn_active_count() && n < FLEET_BCAST_MACS_MAX; s++) {
        const identity_t *id = churn_active_at(s);
        if (id) memcpy(macs[n++], id->addr, 6);
    }
    for (int i = 0; i < probe_agents_count() && n < FLEET_BCAST_MACS_MAX; i++) {
        const probe_agent_t *a = probe_agents_at(i);
        if (a) memcpy(macs[n++], a->mac, 6);        // Wi-Fi probe MACs -> peers exclude them from density
    }
    if (n == 0) return;
    const uint8_t *k = fleet_key_get(); if (!k) return;
    uint8_t pl[RADAR_FRAME_MAX]; size_t plen = fleet_macs_pack(pl, sizeof pl, macs, n);
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame, &flen, RADAR_TYPE_FLEET_MACS, pl, plen,
                        k, s_salt, ++s_counter) == 0) {
        esp_now_send(BCAST, frame, flen);
        ESP_LOGW(ETAG, "fleet: broadcast %u macs", (unsigned)n);
    }
}

static void respond_once(void)
{
    const uint8_t *k = fleet_key_get(); if (!k) return;
    webui_status_t w; webui_gather_status(&w);
    radar_wire_status_t r; espnow_status_from_webui(&r, &w);
    r.preset = (uint8_t)sim_settings_current_preset();
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame, &flen, RADAR_TYPE_STATUS, (uint8_t*)&r, sizeof r,
                        k, s_salt, ++s_counter) != 0) return;
    for (int i = 0; i < 3; i++) esp_now_send(BCAST, frame, flen);   // 3x back-to-back
    ESP_LOGW(ETAG, "answered request (%u B x3)", (unsigned)flen);
}

static void offer_library(void)
{
    static learned_template_t snap[LEARN_CAP];
    const uint8_t *k = fleet_key_get(); if (!k) return;
    size_t n = learn_snapshot(snap, LEARN_CAP);
    if (n == 0) return;
    uint8_t chunks = (uint8_t)((n + LEARN_WIRE_RECS_PER_CHUNK - 1) / LEARN_WIRE_RECS_PER_CHUNK);
    for (uint8_t ci = 0; ci < chunks; ci++) {
        size_t off = (size_t)ci * LEARN_WIRE_RECS_PER_CHUNK;
        uint8_t nrec = (uint8_t)((n - off < LEARN_WIRE_RECS_PER_CHUNK) ? (n - off)
                                                                       : LEARN_WIRE_RECS_PER_CHUNK);
        uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
        if (learn_wire_pack(pl, &plen, &snap[off], nrec, 1, ci, chunks) != 0) continue;
        uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
        if (radar_wire_seal(frame, &flen, RADAR_TYPE_LEARN_OFFER, pl, plen,
                            k, s_salt, ++s_counter) == 0)
            esp_now_send(BCAST, frame, flen);
        vTaskDelay(pdMS_TO_TICKS(20));   // space chunks so the peer's RX queue drains
    }
    ESP_LOGW(ETAG, "offered library (%u recs, %u chunks)", (unsigned)n, chunks);
}

static void espnow_task(void *arg)
{
    (void)arg;
    uint32_t last_offer = 0, last_fleet = 0;
    for (;;) {
        if (!fleet_key_have()) {         // seek-enrollment: can't seal; wait for a signed OFFER
            espnow_drain_rx();           // MUST still drain: the OFFER that keys us arrives here
            s_answer = false;            // ignore telemetry requests until keyed
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        espnow_drain_rx();               // all frame processing happens here, off the driver task
        if (s_answer) { s_answer = false; respond_once(); }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_offer > 30000) { last_offer = now; offer_library(); }   // every 30 s
        if (now - last_fleet > 25000) { last_fleet = now; broadcast_fleet_macs(); }   // every 25 s
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void esp_now_link_start(void)
{
#ifdef SIMULACRA_FLEET_PROVISION
    fleet_key_init();
    if (!fleet_key_have()) {
        char fp[24]; fleet_id_fingerprint(fp, sizeof fp);
        ESP_LOGW(ETAG, "fleet: unenrolled -- identity %s, seeking enrollment", fp);
    } else {
        ESP_LOGW(ETAG, "fleet: enrolled (epoch %u)", (unsigned)fleet_key_epoch());
    }
#endif
#ifdef SIMULACRA_CONFIG_CTRL
    cfg_floor_load();      // restore the control replay floor before any frame can arrive
    ESP_LOGW(ETAG, "config: replay floor %llu", (unsigned long long)s_cfg_floor);
#endif
    // Wi-Fi is already up (coexist STA). Randomize the STA source MAC once (locally-administered).
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_fill_random(mac, 6); mac[0] = (mac[0] & 0xFE) | 0x02;      // LAA, unicast
    esp_wifi_set_mac(WIFI_IF_STA, mac);                            // best-effort; ignore rc
    esp_fill_random(s_salt, sizeof s_salt);

    if (esp_now_init() != ESP_OK) { ESP_LOGE(ETAG, "esp_now_init failed"); return; }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST, 6); peer.channel = SIMULACRA_ESPNOW_CHANNEL; peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
    esp_now_register_recv_cb(on_recv);
    coexist_set_listen_channel(SIMULACRA_ESPNOW_CHANNEL);  // park the radio on ch1 between probe bursts
    xTaskCreate(espnow_task, "espnow", 4096, NULL, 3, NULL);
    ESP_LOGW(ETAG, "responder up (ch=%d, listen-only until requested)", SIMULACRA_ESPNOW_CHANNEL);
}
