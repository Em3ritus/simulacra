#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "identity.h"
#include "churn_adv.h"   // CHURN_HW_INSTANCES

// Active-set / cooldown / time-slice tunables. ACTIVE_SET is the synthetic crowd
// size (how many identities are "present" at once); it is deliberately a plain
// constant so the population density is a one-line tunable (see Task 6).
#define CHURN_ACTIVE_SET        16   // MAX active-set capacity (Ward ceiling); runtime target <= this
#define CHURN_TICK_MS           250
#define CHURN_SLICE_MS          1000

// apply(instance, id): place identity `id` on hardware `instance`. Return value
// (the adapter's rc) is ignored by the engine. Matches churn_adv_apply's int
// signature so the production adapter can be registered directly.
typedef int (*churn_apply_fn)(uint8_t instance, const identity_t *id);

void   churn_set_apply(churn_apply_fn fn);
// Population-match knob (M6): resize the live crowd to n (1..CHURN_ACTIVE_SET). Since Milestone A
// the population lives in ble_devices, so this forwards to ble_devices_set_count using the clock
// recorded by the last churn_tick. Safe to call at runtime, not just before churn_init.
void   churn_set_active_target(uint8_t n);
// Runtime read-back of the active target (population-match knob).
uint8_t churn_active_target(void);
// webui: pause/resume the churn rotation (BLE keeps its last advertised state).
void   churn_set_paused(bool paused);
bool   churn_paused(void);
// Monotonic counter bumped every time a device is (re)applied to a hardware advertising slot, i.e.
// whenever the set of live decoy addresses changes. Lets a consumer cache a view of that set and
// rebuild it only when it actually changed, instead of once per received advert.
uint32_t churn_apply_gen(void);
// M8: runtime turnover boost. mult >= 1.0 divides device lifetimes, so the crowd arrives and
// departs faster (mult=1.0 = the designed bands). Anti-entourage raises it on a drift spike and
// the coordinator decays it back to 1.0. Forwards to ble_devices_set_accel; idempotent.
void   churn_set_accel(float mult);
float  churn_accel(void);
void   churn_init(uint32_t now_ms);
void   churn_tick(uint32_t now_ms);
size_t churn_active_count(void);                 // non-NULL active slots
const identity_t *churn_active_at(size_t slot);  // may be NULL
