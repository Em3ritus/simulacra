#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SIM_TARGET_FLOOR 4   // absolute minimum crowd, whatever the board

// Minimum personas the crowd must be able to host, used to derive the AUTO floor.
//
// This is the MINIMUM VIABLE persona count, deliberately NOT the board's designed count
// (probe_phone_target(), 16 on C5 / 8 on C6). Flooring on the designed count made floor == ceiling
// on the C5 -- 16 personas at the crowd/2 cap need 32 devices, which is the entire budget -- so
// AUTO had no range at all and a bench fleet radiated 88 decoys into a room holding 4-9 real
// devices, exactly the density tell population-match exists to close.
//
// A room with almost nothing in it should not contain sixteen fake phones. The crowd shrinks with
// the room, personas included; the existing crowd/2 cap in coexist scales them back up as ambient
// density rises. This floor only guarantees the cross-protocol (BLE+Wi-Fi bound) layer never
// disappears entirely.
#define SIM_PERSONA_MIN 4

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

// AUTO scales the crowd with measured ambient density; the MANUAL levels name a fixed fraction of
// this board's ceiling and ignore the room. TURBO is HIGH's device count without the realism --
// personas released, max churn (coexist_set_turbo owns that, bypassing floor/ceiling entirely).
//
// ORDINALS ARE A WIRE CONTRACT (config_wire.h, CONFIG_WIRE_VER 2). Changing the order silently
// remaps every preset a Vigil sends. Reordering requires a wire version bump and a whole-fleet
// reflash, not just an edit here.
typedef enum {
    SIM_PRESET_PAUSE = 0, SIM_PRESET_AUTO, SIM_PRESET_LOW,
    SIM_PRESET_MED, SIM_PRESET_HIGH, SIM_PRESET_TURBO, SIM_PRESET_COUNT
} sim_preset_t;

// Every field here drives live behaviour - nothing is stored for display only. The CYD infers the
// running preset from these values (sim_settings_current_preset) and shows it to the operator, so
// a field the engine ignores would make the display lie about what the firmware is doing.
typedef struct {
    uint8_t  active_target;                       // concurrent phantom crowd size
    bool     paused;                              // all decoy BLE + Wi-Fi TX off (see churn.h)
    float    accel;                               // lifetime divisor: >1.0 = faster arrivals/departures
    bool     turbo;                                // TURBO active: coexist_set_turbo owns the REAL
                                                   // population/churn rate, bypassing floor/ceiling
    bool     auto_scale;                          // AUTO: the re-profile tick drives active_target
                                                  // from measured ambient density. When false a
                                                  // manual level sticks -- without this flag the
                                                  // re-profile would clobber it within 10 min.
    uint8_t  auto_cap;                            // AUTO upper bound (this board's share of the
                                                  // operator's fleet-wide cap). 0 = uncapped.
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
// Equivalent to sim_settings_apply_preset_capped(p, 0) -- i.e. AUTO uncapped.
int  sim_settings_apply_preset(sim_preset_t p);
// As above, but also stores `cap` as the AUTO upper bound (0 = uncapped). The cap bounds AUTO only;
// MANUAL levels name their own target. 0 ok, -1 unknown preset.
int  sim_settings_apply_preset_capped(sim_preset_t p, uint8_t cap);
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

// AUTO mode active? The re-profile tick must not overwrite active_target when this is false.
bool    sim_settings_auto_scale(void);
// Current AUTO upper bound (0 = uncapped). Applied by the re-profile after the ambient estimate.
uint8_t sim_settings_auto_cap(void);
