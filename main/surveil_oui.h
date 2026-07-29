#pragma once
#include <stdint.h>
#include <stdbool.h>

// Curated Wi-Fi surveillance-vendor OUI detection. The watchlist holds ONLY verified vendor-owned
// IEEE OUI blocks (Flock, Axon), so a match is a trustworthy surveillance signal with no noise.

// Match mac[0..2] against the watchlist. Returns true (and fills class_id/category, sig_class_t /
// sig_category_t values) on a hit; false otherwise. Pure.
bool surveil_oui_match(const uint8_t mac[6], uint8_t *class_id, uint8_t *category);

// Match an SSID (exact, case-sensitive) against the surveillance-SSID watchlist. Returns true (and
// fills class_id/category) on a hit; false otherwise. Pure. `ssid` is NOT NUL-terminated; `len` is the
// SSID element length.
bool surveil_ssid_match(const uint8_t *ssid, uint8_t len, uint8_t *class_id, uint8_t *category);

// Seed the per-session hash salt (call once, e.g. surveil_init(esp_random())).
void surveil_init(uint32_t salt);
// Salted FNV-1a over the 6-byte MAC (Law 1: hash in the RX path, then drop the MAC).
uint32_t surveil_hash(const uint8_t mac[6]);

// Single-producer (Wi-Fi RX) / single-consumer (coexist) pending ring.
void surveil_note(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category);
bool surveil_next(uint32_t *hash, int8_t *rssi, uint8_t *class_id, uint8_t *category);
