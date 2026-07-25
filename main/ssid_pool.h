#pragma once
#include <stdint.h>

// A fixed, compiled-in pool of GENERIC PUBLIC SSIDs (ubiquitous open/carrier/retail hotspot names
// that appear in a very large number of real devices' preferred-network lists everywhere). A decoy
// probing one is indistinguishable from the real background and reveals nothing about THIS user.
//
// SAFETY: this pool is the ONLY source of directed-probe SSIDs. It is NEVER populated from observed
// or learned traffic -- probing a locally-real SSID would announce the user's actual associations.
// This header/impl must not include any observe/learn/capture source (structurally tested).
#define SSID_POOL_MAX_LEN 32   // 802.11 SSID element maximum

// Per-entry suffix style: real router/gateway defaults are suffixed per-unit (spectrumsetup-4c,
// setup-c803, NETGEAR92), so a decoy naming the bare base repeatedly is a mild tell. A per-persona
// seed drives a stable suffix so each fake device looks like a distinct real router.
enum { SSID_SFX_NONE = 0, SSID_SFX_HEX2, SSID_SFX_HEX4, SSID_SFX_DIGIT };

int         ssid_pool_count(void);
const char *ssid_pool_at(int i, uint8_t *len_out);   // NUL-terminated BASE name + byte length; NULL if OOR
int         ssid_pool_pick_weighted(void);           // weighted random index (uses esp_random)
uint8_t     ssid_pool_suffix_style(int i);           // SSID_SFX_* for entry i (0 if OOR)
// Render entry i with a per-persona `seed` (base + suffix) into out (NUL-terminated, <= outmax).
// Returns the byte length written (0 if OOR). Never exceeds SSID_POOL_MAX_LEN.
uint8_t     ssid_pool_render(int i, uint16_t seed, char *out, uint8_t outmax);
