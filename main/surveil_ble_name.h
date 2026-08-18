#pragma once
#include <stdint.h>
#include <stdbool.h>

// BLE advertised local-name surveillance-vendor detection. Complements the byte-pattern signature
// DB (threat_sig.h / sig_seed.c) for gear that's identifiable by its advertised name rather than a
// structured mfg/service-data field -- e.g. Flock Safety's Raven units, per public research (GainSec
// / BirdShot, DEF CON 34, confirmed against real hardware: the advertised local name contains
// "flock"). A name-substring match is weaker evidence than an exact byte-pattern match, so callers
// should treat it as a lower-confidence signal (see the confidence value returned by the caller).

// Match a BLE advertised local name (Complete or Shortened -- NimBLE's parser already unifies both
// into one field) against known surveillance-device name substrings, case-insensitive. Returns true
// (and fills class_id/category, sig_class_t / sig_category_t values) on a hit; false otherwise. Pure.
// `name` is NOT NUL-terminated; `len` is its byte length.
bool surveil_name_match(const uint8_t *name, uint8_t len, uint8_t *class_id, uint8_t *category);
