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
#include "ble_devices.h"   // ble_device_next_addr(): pre-drawn rotation targets for the broadcast
#include "radar_retx.h"
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
#define ESPNOW_ANSWER_MIN_MS 2000u   // floor between STATUS answers; Vigil polls every 3-5 s
static uint32_t s_last_answer_ms;
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
// The OFFER we answered and the REQUEST we answered it with. The Vigil re-sends the SAME signed
// OFFER once a second for its whole window, and each resend used to start a new session: a fresh
// nonce_d plus a full Ed25519 verify and crypto_box. That cost ~2 s per OFFER on a 160 MHz C6, so
// the RX ring backed up with resends, every GRANT sat behind a newer OFFER that had already re-rolled
// nonce_d, and the GRANT was then rejected as "not our session". The C6 could never enroll, and the
// backlog starved IDLE into the task watchdog. A byte-identical resend now replays the REQUEST
// already built: same session, no crypto, so whichever GRANT comes back matches.
static uint8_t s_enr_offer[ENROLL_OFFER_LEN];
static uint8_t s_enr_offer_nv[24];
static uint8_t s_enr_req[1 + ENROLL_REQUEST_LEN];

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
        if (memcmp(data + 1, s_enr_offer, ENROLL_OFFER_LEN) == 0) {
            // Resend of the OFFER already verified and answered. Once its GRANT has landed there is
            // nothing left to do; until then, replay our REQUEST under the same replay bound.
            if (!s_enr_pending) return;
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (!enr_nonce_answerable(s_enr_offer_nv, now_ms)) return;
            esp_now_send(BCAST, s_enr_req, sizeof s_enr_req);
            return;
        }
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
        s_enr_req[0] = RADAR_TYPE_ENROLL_REQUEST;
        if (enroll_request_build(s_enr_req + 1, ENROLL_REQUEST_LEN, fleet_id_pk(), idsk,
                                 s_enr_nonce_d, veph, nv) != ENROLL_REQUEST_LEN) return;
        esp_now_send(BCAST, s_enr_req, sizeof s_enr_req);
        memcpy(s_enr_offer, data + 1, ENROLL_OFFER_LEN);
        memcpy(s_enr_offer_nv, nv, 24);
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
        if (memcmp(nd_echo, s_enr_nonce_d, 24) != 0) {       // not our session (replay/stale)
            ESP_LOGW(ETAG, "enroll: GRANT for another session ignored");
            return;
        }
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
        if (cmd.version != CONFIG_WIRE_VER) {
            // Loud, not silent: a mismatch means a mixed-firmware fleet, and the preset ordinals
            // shifted in v2 -- applying it anyway would run the WRONG preset under a valid signature.
            ESP_LOGW(ETAG, "config: wire v%u rejected (need v%u) -- reflash the whole fleet",
                     (unsigned)cmd.version, (unsigned)CONFIG_WIRE_VER);
            return;
        }
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
        coexist_request_preset(cmd.preset_id, cmd.cap);
        ESP_LOGW(ETAG, "config: queued %s (cap %u)",
                 cmd.preset_id == CONFIG_CLEAR_THREATS ? "CLEAR THREATS" : "preset",
                 (unsigned)cmd.cap);
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

// STATUS retransmits, spread rather than blasted back-to-back. Fixed 2x (down from 3x): a decoy
// has no delivery feedback -- broadcast is unacknowledged and it never hears the Vigil's view -- so
// adapting the count here would be guesswork. The Vigil's own re-request covers a lost STATUS
// within one poll cycle.
#ifndef ESPNOW_IDROT_MIN_MS
#define ESPNOW_IDROT_MIN_MS  480000u   // 8 min
#endif
#ifndef ESPNOW_IDROT_SPAN_MS
#define ESPNOW_IDROT_SPAN_MS 420000u   // .. up to 15 min, at the ADDR_MAX_ONAIR_MS ceiling
#endif

static radar_retx_t s_status_retx;

// Rotate this node's ESP-NOW LINK IDENTITY: source MAC and wire salt, together.
//
// Both were drawn ONCE at startup and never again, which made each board carry a stable identifier
// for its entire powered life -- hours or days. Every other identifier in the project is capped at
// ADDR_MAX_ONAIR_MS (15 min); these two were orders of magnitude longer, and they sit on the one
// channel whose emission rate already isolates the fleet.
//
// THEY MUST ROTATE TOGETHER. The salt is the first 8 bytes of every frame, in cleartext (the nonce
// is salt||counter). Rotating only the MAC would achieve nothing at all -- an observer simply
// follows the salt instead, and vice versa. Two halves of one identity.
//
// Cost, accepted: a peer keys its replay window on the sender's salt, so each rotation opens a
// fresh one. RADAR_REPLAY_PEERS is raised alongside this so a peer's high-water history spans
// several rotations rather than being churned out by them.
//
// The counter deliberately does NOT reset. Nonce uniqueness needs (salt, counter) to be unique and
// a fresh salt guarantees that on its own; resetting would also break the Vigil's monotonic CONFIG
// floor, which is counter-only and salt-independent.
static void espnow_rotate_link_identity(void)
{
    uint8_t mac[6];
    esp_fill_random(mac, 6);
    mac[0] = (mac[0] & 0xFE) | 0x02;                 // locally administered, unicast
    esp_wifi_set_mac(WIFI_IF_STA, mac);              // best-effort; verified LAA on air by espnow_sniff
    esp_fill_random(s_salt, sizeof s_salt);
    ESP_LOGW(ETAG, "link identity rotated (mac %02x:%02x:.. salt refreshed)", mac[0], mac[1]);
}

// One sealed FLEET_MACS frame. Split out so the broadcast can chunk (see below).
static void fleet_macs_send_chunk(const uint8_t (*macs)[6], size_t n)
{
    if (n == 0) return;
    const uint8_t *k = fleet_key_get(); if (!k) return;
    uint8_t pl[RADAR_FRAME_MAX]; size_t plen = fleet_macs_pack(pl, sizeof pl, macs, n);
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame, &flen, RADAR_TYPE_FLEET_MACS, pl, plen,
                        k, s_salt, ++s_counter) == 0)
        esp_now_send(BCAST, frame, flen);
}

// Broadcast our current active synthetic MACs so fleet-mates can self-exclude us from their
// model / learn / detect. AES-GCM authenticated (only fleet PSK holders can read/emit it).
//
// CHUNKED, because one frame cannot carry a full board's identities. FLEET_BCAST_MACS_MAX (32) is
// a hard consequence of the 250-byte ESP-NOW frame, not a tunable. Since boards became additive
// (2026-08-24) a single decoy runs up to BLE_DEVICES_MAX + PROBE_AGENTS_MAX = 48 identities, so the
// old single-frame version silently TRUNCATED at 32 and never advertised the rest. Worse, BLE was
// packed first, so a full BLE crowd consumed all 32 slots and ZERO probe MACs were ever broadcast --
// the Wi-Fi density estimate had no fleet exclusion at all.
//
// The cost of truncation is a positive feedback loop: an unadvertised fleetmate MAC is counted as a
// real ambient device -> pop_ewma inflates -> AUTO grows the crowd -> more unadvertised MACs. On the
// bench this saturated every board at its ceiling within an hour, and because rf_model persists to
// NVS the inflated estimate survived reboots.
//
// No wire-format change is needed. Exclusion is additive with a TTL (fleet_note_peer_macs is
// refresh-or-insert), so each chunk is independently useful and the receiver needs no reassembly --
// unlike the sig-sync path, which must have every chunk before it can adopt.
//
// ---------------------------------------------------------------------------------------------
// DELTA + PACED (2026-08-26). Sending the whole table every 20-30 s made this the loudest thing
// the board does, and the 2026-08-25 Kismet capture showed why that matters more than its content:
//
//   fleet transmitters     :   5   action frames 19714   (99.4/min)
//   all other transmitters : 206   action frames    29   ( 0.1/min)
//   non-fleet devices emitting ANY action frame: 3 of 206
//
// Each board emitted ~25 vendor action frames/min where the loudest real device in a 206-device
// environment managed 0.07/min and the median managed none. The FLEET_MACS broadcast was 88% of
// that. Frame-length bucketing and retransmit jitter hide what the frames SAY; they cannot hide
// that they exist, and an adversary needs no key to count them. Two changes here:
//
//  1. Send only what CHANGED. Identities are stable between sweeps - rotation is 10-15 min while
//     sweeps are ~1 min - so a delta typically carries a handful of MACs instead of the whole
//     table. A periodic full resync repairs any drift and refreshes peer TTLs, which is why
//     FLEET_MAC_TTL_MS had to be raised to cover the resync period (see fleet.h).
//  2. PACE the chunks. The old loop emitted every chunk back-to-back with no delay, producing a
//     fixed ~20 ms cadence that is itself machine-timed and distinctive. Chunks are now queued and
//     released one at a time on a jittered interval by the task loop.
//
// This is a REDUCTION, not a fix. Any sustained vendor-action-frame rate is anomalous where 203 of
// 206 devices emit exactly zero. Physically wiring the boards is the only measure that removes the
// tell rather than shrinking it.
// ---------------------------------------------------------------------------------------------

// Every identity this board currently advertises: live BLE addrs, their pre-drawn next addrs, and
// Wi-Fi probe-agent MACs.
#define FLEET_IDENT_MAX (2 * BLE_DEVICES_MAX + PROBE_AGENTS_MAX)

static uint8_t  s_sent[FLEET_IDENT_MAX][6];   // what peers have already been told about
static size_t   s_sent_n;
static uint8_t  s_pend[FLEET_IDENT_MAX][6];   // queued for release, drained a chunk at a time
static size_t   s_pend_n, s_pend_i;
static uint32_t s_next_chunk_ms;

// Gather the current identity set. Returns the count written to `out`.
static size_t collect_identities(uint8_t (*out)[6], size_t max)
{
    size_t n = 0;
    for (size_t s = 0; s < churn_active_count() && n < max; s++) {
        const identity_t *id = churn_active_at(s);
        if (id) memcpy(out[n++], id->addr, 6);
    }
    // The PRE-DRAWN NEXT address, so peers hold it before it goes on air. Without this a freshly
    // rotated fleetmate address is unexcluded for up to one broadcast interval, and in that window
    // peers both count it as a real ambient device (the population feedback measured on
    // 2026-08-25) and match it against the tracker signature DB.
    for (int i = 0; i < ble_devices_count() && n < max; i++) {
        const uint8_t *na = ble_device_next_addr(i);
        if (na) memcpy(out[n++], na, 6);        // NULL for static: never rotates
    }
    for (int i = 0; i < probe_agents_count() && n < max; i++) {
        const probe_agent_t *a = probe_agents_at(i);
        if (a) memcpy(out[n++], a->mac, 6);     // peers exclude these from Wi-Fi density
    }
    return n;
}

static bool mac_in(const uint8_t (*set)[6], size_t n, const uint8_t mac[6])
{
    for (size_t i = 0; i < n; i++) if (memcmp(set[i], mac, 6) == 0) return true;
    return false;
}

// Queue this sweep's MACs for paced release. `full` sends everything (resync); otherwise only
// identities peers have not already been told about.
static void broadcast_fleet_macs(bool full)
{
    uint8_t cur[FLEET_IDENT_MAX][6];
    size_t n = collect_identities(cur, FLEET_IDENT_MAX);

    // Anything still queued from the previous sweep is superseded: a resync covers it, and a delta
    // recomputes against s_sent, which the unsent entries were never added to.
    s_pend_n = s_pend_i = 0;
    for (size_t i = 0; i < n; i++) {
        if (full || !mac_in(s_sent, s_sent_n, cur[i]))
            memcpy(s_pend[s_pend_n++], cur[i], 6);
    }
    // The sent-set becomes exactly the live set, so retired MACs stop being tracked and a MAC that
    // reappears is re-sent. Bounded by construction: it is a copy of the live identity set.
    memcpy(s_sent, cur, n * 6);
    s_sent_n = n;

    if (s_pend_n)
        ESP_LOGW(ETAG, "fleet: queued %u macs (%s) of %u live",
                 (unsigned)s_pend_n, full ? "resync" : "delta", (unsigned)n);
}

// Release at most one chunk per call, on a jittered interval. Called from the task loop.
static void fleet_macs_pump(uint32_t now_ms)
{
    if (s_pend_i >= s_pend_n) return;
    if ((int32_t)(now_ms - s_next_chunk_ms) < 0) return;
    size_t n = s_pend_n - s_pend_i;
    if (n > FLEET_BCAST_MACS_MAX) n = FLEET_BCAST_MACS_MAX;
    fleet_macs_send_chunk(&s_pend[s_pend_i], n);
    s_pend_i += n;
    // Wide, jittered spacing. The old back-to-back loop produced a fixed ~20 ms cadence; this
    // spreads a multi-chunk resync across several seconds and never repeats an interval.
    s_next_chunk_ms = now_ms + 250u + (esp_random() % 1000u);
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
    radar_retx_arm(&s_status_retx, frame, flen, 2,
                   (uint32_t)(esp_timer_get_time() / 1000));
    ESP_LOGW(ETAG, "answered request (%u B x2, spread)", (unsigned)flen);
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
    // Jittered periods, re-drawn on every fire. Fixed 25 s / 30 s timers made this node's ESP-NOW
    // housekeeping a metronome: a listener who cannot read a single sealed byte could still lock
    // onto the cadence and count the fleet.
    //
    // FLEET_MACS now runs at two rates. The DELTA sweep only needs to beat rotation, which is
    // 10-15 min away and pre-drawn besides, so 45-75 s is ample and costs ~1 frame. The RESYNC
    // sweep repairs drift and refreshes peer TTLs, so it must stay well inside FLEET_MAC_TTL_MS
    // (12 min) -- 3-4 min keeps the same 3x margin the old 20-30 s had against 90 s.
    uint32_t last_offer = 0, last_fleet = 0, last_resync = 0, last_idrot = 0;
    uint32_t offer_period  = 25000 + esp_random() % 10001;   // 25-35 s
    uint32_t fleet_period  = 45000 + esp_random() % 30001;   // 45-75 s  (delta)
    uint32_t resync_period = 180000 + esp_random() % 60001;  // 3-4 min  (full, TTL 12 min)
    // Link identity (MAC + salt) on the same ceiling as every other identifier the project emits.
    // Overridable so the rotation can actually be OBSERVED on air in a short bench run instead of
    // waiting 15 minutes per sample; the shipped value is the real one.
    uint32_t idrot_period  = ESPNOW_IDROT_MIN_MS + esp_random() % (ESPNOW_IDROT_SPAN_MS + 1);
    for (;;) {
        if (!fleet_key_have()) {         // seek-enrollment: can't seal; wait for a signed OFFER
            espnow_drain_rx();           // MUST still drain: the OFFER that keys us arrives here
            s_answer = false;            // ignore telemetry requests until keyed
            // Nothing we queued could be sealed, and peers keyed to a previous epoch cannot have
            // read anything either. Drop the delta baseline so the first sweep after enrolment
            // sends the FULL set rather than a delta against a state no peer shares.
            s_sent_n = s_pend_n = s_pend_i = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        espnow_drain_rx();               // all frame processing happens here, off the driver task
        {   // Spread STATUS repeats. The 50 ms task tick is comfortably finer than the 40-120 ms
            // gaps, so no extra timer is needed.
            uint32_t rnow = (uint32_t)(esp_timer_get_time() / 1000);
            if (radar_retx_due(&s_status_retx, rnow, esp_random()))
                esp_now_send(BCAST, s_status_retx.frame, s_status_retx.len);
        }
        // Minimum spacing between STATUS answers, independent of how many REQUESTs arrive.
        // Defence in depth behind the per-salt replay table: even if an attacker cycled enough
        // distinct captured salts to evict an entry, a replayed REQUEST can extract at most one
        // answer per window rather than an unbounded stream. The Vigil polls every 3-5 s, so a 2 s
        // floor never delays a legitimate request.
        if (s_answer) {
            s_answer = false;
            uint32_t rn = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(rn - s_last_answer_ms) >= ESPNOW_ANSWER_MIN_MS || s_last_answer_ms == 0) {
                s_last_answer_ms = rn;
                respond_once();
            }
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_offer > offer_period) {
            last_offer = now; offer_period = 25000 + esp_random() % 10001;
            offer_library();
        }
        // Resync takes precedence: it supersedes a delta due in the same tick, so the two never
        // both queue and the delta's work is not wasted.
        if (now - last_resync > resync_period) {
            last_resync = now; last_fleet = now;
            resync_period = 180000 + esp_random() % 60001;
            fleet_period  = 45000 + esp_random() % 30001;
            broadcast_fleet_macs(true);
        } else if (now - last_fleet > fleet_period) {
            last_fleet = now; fleet_period = 45000 + esp_random() % 30001;
            broadcast_fleet_macs(false);
        }
        fleet_macs_pump(now);        // release at most one queued chunk, on a jittered interval
        // Rotate the link identity LAST in the tick, so nothing queued this pass is half-sent under
        // the old MAC and half under the new one.
        if (now - last_idrot > idrot_period) {
            last_idrot = now;
            idrot_period = ESPNOW_IDROT_MIN_MS + esp_random() % (ESPNOW_IDROT_SPAN_MS + 1);
            espnow_rotate_link_identity();
        }
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
    // 4096 was too tight: a real hardware pairing session under SIMULACRA_FLEET_PROVISION crashed
    // with "Stack protection fault" in this task on real hardware, reliably, every boot -- the
    // enrollment handshake's Ed25519 verify (OFFER) and X25519/crypto_box operations (REQUEST/GRANT)
    // are meaningfully stack-heavier than the steady-state AES-GCM open/seal this task otherwise
    // does, and this exact path had never actually been hardware-tested with real enrollment traffic
    // flowing until this session. coexist_task budgets 8192 for a comparable (arguably lighter)
    // crypto load; match it here rather than guess a smaller number.
    xTaskCreate(espnow_task, "espnow", 8192, NULL, 3, NULL);
    ESP_LOGW(ETAG, "responder up (ch=%d, listen-only until requested)", SIMULACRA_ESPNOW_CHANNEL);
}
