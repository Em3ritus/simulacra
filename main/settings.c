#include "settings.h"
#include "churn.h"
#include "coexist.h"      // coexist_set_turbo(): the TURBO override lives at the coexist tick level
#include "probe.h"        // probe_desired_ble_floor(): this board's designed crowd size
#include "ble_devices.h"  // BLE_DEVICES_MAX
#include "fleet_pop.h"    // fleet_pop_share(): this node's share when K nodes split the crowd
#include "nvs.h"
#include <string.h>

#define SETTINGS_NVS_NS  "sim"
// Key bumped to "settings2" when the preset ceiling moved from CHURN_ACTIVE_SET (16) to the
// board's designed crowd size (32 on C5 / 24 on C6). active_target is stored as an absolute count,
// so a blob written under the old scale means something different under the new one -- a persisted
// 16 silently pinned the C5 to half its intended population. Reading the old key would restore a
// number that is no longer meaningful, so the old blob is abandoned and defaults are re-derived.
#define SETTINGS_NVS_KEY "settings2"

static sim_settings_t s_cur;   // current in-RAM settings (source of truth)

// Preset ceiling = this board's DESIGNED crowd size (personas + unbound companions), not the
// legacy CHURN_ACTIVE_SET.
//
// Those two are different scales and conflating them broke the crowd on hardware: CHURN_ACTIVE_SET
// is 16, while the C5's designed population is 32 (16 personas + 16 unbound). Once
// churn_set_active_target became live again, applying NORMAL at boot shrank the crowd from 32 to
// 16 -- exactly the persona count -- so every slot was persona-bound and the whole BLE crowd became
// company-0x0000 phone shapes with no beacons or tags at all. A pure-phone crowd is a monoculture,
// which is the failure the diversity log exists to catch.
uint8_t sim_settings_ceiling(void)
{
    // This node's SHARE of the fleet-wide designed crowd. Dividing here as well as in the floor
    // keeps both bounds on the same scale -- otherwise a 3-node fleet would floor at 1/K of the
    // personas but still be allowed to fill a whole standalone-sized crowd.
    int c = fleet_pop_share(probe_desired_ble_floor());
    if (c > BLE_DEVICES_MAX) c = BLE_DEVICES_MAX;
    if (c < SIM_TARGET_FLOOR) c = SIM_TARGET_FLOOR;
    return (uint8_t)c;
}

// Personas are capped at half the crowd (see coexist), so hosting N of them needs 2N devices.
uint8_t sim_settings_floor(void)
{
    int f = 2 * fleet_pop_share(probe_phone_target());
    if (f > BLE_DEVICES_MAX) f = BLE_DEVICES_MAX;
    if (f < SIM_TARGET_FLOOR) f = SIM_TARGET_FLOOR;
    return (uint8_t)f;
}

void sim_settings_clamp(sim_settings_t *s, uint8_t floor, uint8_t ceiling)
{
    if (floor < SIM_TARGET_FLOOR) floor = SIM_TARGET_FLOOR;
    if (ceiling < floor) ceiling = floor;
    if (s->active_target < floor) s->active_target = floor;
    if (s->active_target > ceiling) s->active_target = ceiling;
    if (s->accel < 1.0f) s->accel = 1.0f;
    if (s->accel > 4.0f) s->accel = 4.0f;
}

// Presets differ ONLY in knobs the engine actually reads: crowd size, turnover rate, and pause.
// The old dwell/cooldown windows described the roster promote/retire state machine that Milestone
// A replaced with per-device lifetimes; they were still stored and reported after they stopped
// driving anything, which is how the CYD came to display a preset the firmware was not running.
int sim_settings_resolve(sim_preset_t p, uint8_t floor, uint8_t ceiling, sim_settings_t *out)
{
    if (p >= SIM_PRESET_COUNT) return -1;
    uint8_t stealth = (uint8_t)((ceiling * 4) / 10);   // ~40% of ceiling (raised to floor below)
    sim_settings_t s = { .active_target = ceiling, .paused = false, .accel = 1.0f, .turbo = false };
    switch (p) {
    case SIM_PRESET_PAUSE:                                  // NORMAL values, rotation frozen
        s.paused = true; break;
    case SIM_PRESET_STEALTH:
        s.active_target = stealth; break;                   // smaller crowd, unhurried turnover
    case SIM_PRESET_NORMAL:
        break;                                              // firmware defaults
    case SIM_PRESET_DENSE:
        s.accel = 1.5f; break;                              // full crowd, 1.5x turnover
    case SIM_PRESET_MAX:
        s.accel = 2.5f; break;                              // full crowd, 2.5x turnover
    case SIM_PRESET_TURBO:
        // active_target/accel below are irrelevant once turbo=true: sim_settings_match_preset
        // (below) short-circuits on the turbo flag alone. The REAL population/churn rate is forced
        // directly by coexist_set_turbo (added in Task 5), bypassing the fleet-share floor/ceiling
        // entirely -- every board floods at its own hardware max, not a room-density estimate
        // divided across K nodes. That bypass is the whole point of the mode.
        s.turbo = true; break;
    default: return -1;
    }
    sim_settings_clamp(&s, floor, ceiling);
    *out = s;
    return 0;
}

void sim_settings_apply(const sim_settings_t *s)
{
    churn_set_active_target(s->active_target);
    churn_set_paused(s->paused);
    churn_set_accel(s->accel);
    coexist_set_turbo(s->turbo);
    s_cur = *s;
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;   // best-effort
    nvs_set_blob(h, SETTINGS_NVS_KEY, &s_cur, sizeof s_cur);
    nvs_commit(h); nvs_close(h);
}

void sim_settings_set(const sim_settings_t *s)
{
    sim_settings_t c = *s; sim_settings_clamp(&c, sim_settings_floor(), sim_settings_ceiling());
    sim_settings_apply(&c); settings_save();
}

int sim_settings_apply_preset(sim_preset_t p)
{
    sim_settings_t s;
    if (sim_settings_resolve(p, sim_settings_floor(), sim_settings_ceiling(), &s) != 0) return -1;
    sim_settings_apply(&s); settings_save();
    return 0;
}

// Re-clamp the live settings to the CURRENT board bounds and apply if the target moved.
//
// The bounds depend on the live node census (each node runs 1/K of the fleet crowd), and K changes
// as peers are heard or go quiet. Without this the crowd would only resize at the next re-profile
// -- up to 10 minutes on Ward -- so a fleet powering on together would radiate ~K times the
// intended density for that whole window. Deliberately does NOT persist: the census is a runtime
// observation, not an operator choice, and writing it would overwrite the chosen preset in NVS.
void sim_settings_recalc_bounds(void)
{
    sim_settings_t s = s_cur;
    sim_settings_clamp(&s, sim_settings_floor(), sim_settings_ceiling());
    if (s.active_target != s_cur.active_target) sim_settings_apply(&s);
}

void sim_settings_get(sim_settings_t *out) { *out = s_cur; }

sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t floor, uint8_t ceiling)
{
    for (sim_preset_t p = SIM_PRESET_PAUSE; p < SIM_PRESET_COUNT; p++) {
        sim_settings_t r;
        if (sim_settings_resolve(p, floor, ceiling, &r) != 0) continue;
        // Turbo is identified by the flag alone. While turbo is running, coexist_set_turbo forces
        // the real population directly (bypassing floor/ceiling), so cur->active_target is NOT the
        // fleet-shared value resolve() computed above -- requiring it to also match would always
        // report CUSTOM instead of TURBO while the mode is genuinely active.
        if (r.turbo != cur->turbo) continue;
        if (cur->turbo) return p;
        if (r.active_target == cur->active_target && r.paused == cur->paused &&
            r.accel == cur->accel)
            return p;
    }
    return SIM_PRESET_COUNT;   // CUSTOM
}

sim_preset_t sim_settings_current_preset(void)
{
    return sim_settings_match_preset(&s_cur, sim_settings_floor(), sim_settings_ceiling());
}

bool sim_settings_get_paused(void) { return s_cur.paused; }

void sim_settings_init(void)
{
    sim_settings_t s;
    nvs_handle_t h; size_t len = sizeof s;
    bool loaded = (nvs_open(SETTINGS_NVS_NS, NVS_READONLY, &h) == ESP_OK) &&
                  (nvs_get_blob(h, SETTINGS_NVS_KEY, &s, &len) == ESP_OK) && len == sizeof s;
    if (loaded) nvs_close(h);
    if (!loaded) sim_settings_resolve(SIM_PRESET_NORMAL, sim_settings_floor(), sim_settings_ceiling(), &s);
    sim_settings_clamp(&s, sim_settings_floor(), sim_settings_ceiling());   // guard a stale blob
    sim_settings_apply(&s);
}
