#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Rich HT/VHT/HE/ext-cap/vendor IE sets exceed 64 bytes; give ample headroom.
#define PROBE_FRAME_MAX 256

// Phone archetypes the injector models. Order must match ARCHS[] in probe_frame.c
// and the arch index used by tools/probe_audit/probe_dump.c.
typedef enum { ARCH_IPHONE, ARCH_GALAXY, ARCH_PIXEL, ARCH_ANDROID, PROBE_ARCH_COUNT } probe_arch_t;

typedef struct {
    const char    *name;
    const uint8_t *tail24; uint16_t tail24_len;   // 2.4 GHz IE body; NULL/0 if band absent
    const uint8_t *tail5;  uint16_t tail5_len;    // 5 GHz IE body;   NULL/0 if band absent
    uint8_t        weight;                        // fixed pool-draw spread
} probe_archetype_t;

// Fill a randomized locally-administered, unicast MAC (Wi-Fi analog of BLE random-static).
void   probe_random_mac(uint8_t out[6]);

const  probe_archetype_t *probe_archetype(probe_arch_t a);   // NULL if out of range
size_t probe_archetype_count(void);
probe_arch_t probe_pick_archetype(void);                     // weighted draw (uses esp_random)

// Build a probe request for source `mac` on `ch`, using archetype `arch`'s per-band IE set. band5
// selects the 5 GHz tail. `ssid`/`ssid_len` give an optional directed SSID: pass NULL/0 for the
// wildcard broadcast probe (output then byte-identical to the historical wildcard-only builder).
// Writes the 802.11 frame to out (<= PROBE_FRAME_MAX) and its length. Returns 0 on success; 1 bad
// arch, 2 arch lacks that band, 3 frame overflow, 4 ssid_len exceeds the 802.11 SSID max (32).
int    probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                           const char *ssid, uint8_t ssid_len, uint8_t *out, size_t *out_len);
