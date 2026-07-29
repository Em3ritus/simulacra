#include "surveil_oui.h"
#include "threat_sig.h"
#include <stddef.h>
#include <string.h>

typedef struct { uint8_t oui[3]; uint8_t class_id; uint8_t category; } surveil_entry_t;

// VERIFIED vendor-owned IEEE blocks only (see the design doc). Add entries only after confirming the
// OUI is registered to a surveillance vendor outright -- never a shared component-module vendor.
static const surveil_entry_t WATCH[] = {
    { { 0xB4, 0x1E, 0x52 }, SIG_CLASS_FLOCK, SIG_CAT_CAMERA },   // Flock Safety (ALPR cameras)
    { { 0x00, 0x25, 0xDF }, SIG_CLASS_AXON,  SIG_CAT_BODYCAM },  // Axon / Taser (bodycam + evidence)
};
#define WATCH_N (sizeof WATCH / sizeof WATCH[0])

bool surveil_oui_match(const uint8_t mac[6], uint8_t *class_id, uint8_t *category)
{
    for (size_t i = 0; i < WATCH_N; i++) {
        if (mac[0] == WATCH[i].oui[0] && mac[1] == WATCH[i].oui[1] && mac[2] == WATCH[i].oui[2]) {
            if (class_id) *class_id = WATCH[i].class_id;
            if (category) *category = WATCH[i].category;
            return true;
        }
    }
    return false;
}

typedef struct { const char *ssid; uint8_t len; uint8_t class_id; uint8_t category; } surveil_ssid_t;

// Surveillance network names probed for by known gear. test_flck: Flock Falcon/Sparrow saved dev
// network (CVE-2025-59409) -- units probe for it when Wi-Fi is up. Exact, case-sensitive.
static const surveil_ssid_t SSID_WATCH[] = {
    { "test_flck", 9, SIG_CLASS_FLOCK, SIG_CAT_CAMERA },
};
#define SSID_WATCH_N (sizeof SSID_WATCH / sizeof SSID_WATCH[0])

bool surveil_ssid_match(const uint8_t *ssid, uint8_t len, uint8_t *class_id, uint8_t *category)
{
    for (size_t i = 0; i < SSID_WATCH_N; i++) {
        if (len == SSID_WATCH[i].len && memcmp(ssid, SSID_WATCH[i].ssid, len) == 0) {
            if (class_id) *class_id = SSID_WATCH[i].class_id;
            if (category) *category = SSID_WATCH[i].category;
            return true;
        }
    }
    return false;
}

#define SURVEIL_RING 8
static uint32_t s_salt;
static struct { uint32_t hash; int8_t rssi; uint8_t class_id; uint8_t category; } s_ring[SURVEIL_RING];
static volatile uint32_t s_head, s_tail;

void surveil_init(uint32_t salt) { s_salt = salt; s_head = 0; s_tail = 0; }

uint32_t surveil_hash(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void surveil_note(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category)
{
    uint32_t n = (s_head + 1u) % SURVEIL_RING;
    if (n == s_tail) return;                        // ring full -> drop (hits are rare)
    s_ring[s_head].hash = hash; s_ring[s_head].rssi = rssi;
    s_ring[s_head].class_id = class_id; s_ring[s_head].category = category;
    s_head = n;
}

bool surveil_next(uint32_t *hash, int8_t *rssi, uint8_t *class_id, uint8_t *category)
{
    if (s_tail == s_head) return false;
    *hash = s_ring[s_tail].hash; *rssi = s_ring[s_tail].rssi;
    *class_id = s_ring[s_tail].class_id; *category = s_ring[s_tail].category;
    s_tail = (s_tail + 1u) % SURVEIL_RING;
    return true;
}
