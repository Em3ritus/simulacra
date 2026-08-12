#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RADAR_MAGIC0 0x5A
#define RADAR_MAGIC1 0x4D
#define RADAR_WIRE_VER 3      // v3: nonce is salt(8)||counter(4); v2 was salt(4)||counter(8)
#define RADAR_TYPE_REQUEST 1
#define RADAR_TYPE_STATUS  2
#define RADAR_MAX_THREATS  8        // must match DETECT_MAX_THREATS
// Threat kind (shared so the renderer can label without depending on detect.h):
#define DETECT_KIND_FOLLOWER 0      // behavioral follower (persistence across epochs)
#define DETECT_KIND_KNOWN    1      // fingerprint match (known device class)
#define RADAR_KEY_LEN   32
#define RADAR_NONCE_LEN 12
#define RADAR_SALT_LEN   8          // nonce = salt(8) || counter(4 BE)
#define RADAR_TAG_LEN   16
#define RADAR_HDR_LEN    4          // magic(2)+ver+type
#define RADAR_FRAME_MAX 250

typedef struct __attribute__((packed)) {
    uint32_t uptime_s; uint8_t flags;            // bit0 paused, bit1 config_mode
    uint16_t active_devices, roster_size; uint32_t probes_sent;
    uint16_t epoch, pop_ewma; uint32_t total_obs;
    uint8_t active_target, threat_count;
    struct __attribute__((packed)) {
        uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi;
        uint16_t first_epoch, last_epoch;
        uint8_t kind, class_id, category, confidence;   // KNOWN-device fields (kind=DETECT_KIND_*)
        uint8_t sessions_seen, places_seen;             // recurrence counters (escalation)
    } threats[RADAR_MAX_THREATS];
    uint8_t form_restless, form_wandering, form_bound;  // BLE shade-form counts: RPA/NRPA/static
    uint16_t battery_mv;                                 // cell voltage, 0 = no battery / no gauge
    uint8_t  battery_pct;                                // state-of-charge %, 0xFF = unavailable (ADC backend)
    uint8_t  preset;                                     // running preset: 0-4 sim_preset_t, 5 CUSTOM, 0xFE MIXED, 0xFF none
} radar_wire_status_t;

typedef struct { uint8_t salt[RADAR_SALT_LEN]; uint64_t counter; bool seen; } radar_replay_t;

// Build [magic|ver|type|nonce|ct|tag] into frame. nonce = salt(4)|counter(8 BE). magic|ver|type
// authenticated as AAD. Returns 0 on success, <0 on error; *frame_len set to total bytes.
int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[RADAR_SALT_LEN], uint64_t counter);

// Verify + decrypt a frame. Returns 0 on success (type/payload/salt/counter filled), <0 if the
// header/magic/tag is bad OR the plaintext would not fit in payload_cap bytes. The capacity is
// checked BEFORE decryption (mbedtls writes plaintext before it compares the tag), so an
// oversized frame can never overflow `payload` — pass sizeof(your buffer) and nothing else.
int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t payload_cap, size_t *payload_len,
                    uint8_t out_salt[RADAR_SALT_LEN], uint64_t *out_counter);

// Replay gate for TELEMETRY: accept iff salt changed (peer reboot) or counter strictly newer.
// Updates st. Deliberately forgiving — a rebooted peer must be able to resume, and the worst a
// replayed STATUS/LEARN frame can do is restate stale data.
//
// NOT SUFFICIENT FOR CONTROL. The salt-change branch accepts any unfamiliar salt, so an attacker
// holding captures from two sessions can alternate them forever, and the signature (which covers
// salt||counter) re-verifies over exactly the replayed material. Commands that change behaviour
// use radar_replay_monotonic_ok against a floor persisted across reboots.
bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[RADAR_SALT_LEN], uint64_t counter);

// Replay gate for CONTROL: strictly monotonic, salt-independent — no "peer rebooted" escape
// hatch. *floor must be restored from non-volatile storage at boot and re-persisted whenever
// this returns true, or a power-cycle reopens the replay window.
//
// Callers MUST verify the command's signature BEFORE calling this. Advancing the floor on an
// unauthenticated frame would let anyone in range jam it to UINT64_MAX and permanently deafen
// the control channel.
bool radar_replay_monotonic_ok(uint64_t *floor, uint64_t counter);
