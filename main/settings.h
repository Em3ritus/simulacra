#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SIM_TARGET_FLOOR 4   // absolute minimum crowd, whatever the board

// This board's designed crowd size (personas + unbound companions) -- the ceiling every preset
// resolves against. NOT CHURN_ACTIVE_SET, which is a different, smaller scale.
uint8_t sim_settings_ceiling(void);

// Lower bound on the crowd: enough devices to host this node's DESIGNED persona count.
//
// A persona is a synthetic phone presenting on BLE and Wi-Fi together, and personas are capped at
// half the crowd so the population never becomes an all-phone monoculture. Hosting N personas
// therefore needs a crowd of 2N. Room-density matching flexes the UNBOUND half (beacons, tags,
// wearables); it must not squeeze out the personas themselves, which are a design constant of the
// node, not a property of the room.
uint8_t sim_settings_floor(void);

typedef enum {
    SIM_PRESET_PAUSE = 0, SIM_PRESET_STEALTH, SIM_PRESET_NORMAL,
    SIM_PRESET_DENSE, SIM_PRESET_MAX, SIM_PRESET_COUNT
} sim_preset_t;

// Every field here drives live behaviour — nothing is stored for display only. The CYD infers the
// running preset from these values (sim_settings_current_preset) and shows it to the operator, so
// a field the engine ignores would make the display lie about what the firmware is doing.
typedef struct {
    uint8_t  active_target;                       // concurrent phantom crowd size
    bool     paused;                              // freeze rotation (phantoms stay on-air)
    float    accel;                               // lifetime divisor: >1.0 = faster arrivals/departures
} sim_settings_t;

// Pure: resolve preset p to concrete settings between `floor` and `ceiling`, already clamped.
// Returns 0 on success, -1 for an unknown preset. No side effects.
int  sim_settings_resolve(sim_preset_t p, uint8_t floor, uint8_t ceiling, sim_settings_t *out);

// Clamp settings into [floor, ceiling] in place (idempotent). Used on every apply so a forged or
// malformed command can never cross safe bounds. Both bounds are explicit so the pure core stays
// testable while the board-derived values come from sim_settings_floor()/_ceiling().
void sim_settings_clamp(sim_settings_t *s, uint8_t floor, uint8_t ceiling);

// Apply settings to the churn engine now (no persistence).
void sim_settings_apply(const sim_settings_t *s);
// Resolve preset against sim_settings_ceiling(), clamp, apply, and persist. 0 ok, -1 unknown.
int  sim_settings_apply_preset(sim_preset_t p);
// Load persisted settings from NVS (or firmware defaults) and apply. Call once at boot.
void sim_settings_init(void);
// Re-clamp live settings to the current board bounds (which follow the live node census) and
// apply if the crowd target moved. Call when the census changes. Does not persist.
void sim_settings_recalc_bounds(void);

// Snapshot the current in-RAM settings.
void sim_settings_get(sim_settings_t *out);
// Pure: which preset (against floor/ceiling) resolves to exactly *cur? SIM_PRESET_COUNT = CUSTOM.
sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t floor, uint8_t ceiling);
// The preset the engine is currently running (inferred from live settings). SIM_PRESET_COUNT = CUSTOM.
sim_preset_t sim_settings_current_preset(void);
// Web-UI granular path: clamp, apply, and persist an explicit settings struct.
void sim_settings_set(const sim_settings_t *s);
// Current pause state (convenience for the web UI toggle; avoids exposing the struct).
bool sim_settings_get_paused(void);
