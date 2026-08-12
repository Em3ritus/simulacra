#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "probe_frame.h"     // probe_arch_t
#include "probe_agents.h"    // PROBE_AGENTS_MAX

// A cross-protocol persona: the shared lifecycle + device family that binds one BLE identity
// and one Wi-Fi probe identity into a single synthetic dual-radio "device". Named `phantom`
// (not `persona`) to avoid colliding with coexist_persona_t (the board coexistence profile).
typedef enum { PF_SAMSUNG, PF_GOOGLE, PF_APPLE, PF_GENERIC, PHANTOM_FAMILY_COUNT } phantom_family_t;

typedef struct {
    phantom_family_t family;
    uint32_t born_ms;
    uint32_t life_ms;
    uint32_t generation;     // bumped on each reincarnation; bound members re-sync on change
    bool     alive;
} phantom_t;

#define PHANTOM_MAX PROBE_AGENTS_MAX     // one persona per probe agent (bind from the Wi-Fi side)

void  phantom_init(int n, uint32_t now_ms);      // create n phantoms (clamped to PHANTOM_MAX)
int   phantom_lifecycle(uint32_t now_ms);        // retire+reincarnate expired; returns # reborn
int   phantom_count(void);
// Track the live Wi-Fi agent count. The persona count MUST equal the agent count: a persona
// without an agent is a BLE "phone" that never probes (a single-radio ghost -- the exact artefact
// personas exist to defeat), and an agent without a persona is a Wi-Fi device with no BLE twin and
// no lifecycle on the coexist path (it would never age out). Growing spawns fresh personas;
// shrinking releases the surplus BLE slots back to the unbound crowd via ble_device_unbind.
void  phantom_set_count(int n, uint32_t now_ms);
const phantom_t *phantom_at(int i);
probe_arch_t phantom_arch(phantom_family_t f);   // family -> Wi-Fi archetype
uint16_t     phantom_company(phantom_family_t f);// family -> BLE company id (0 = anonymous RPA)

// Align every bound probe agent to its phantom (agent i <-> phantom i). Reincarnates an agent
// whenever its phantom's generation has advanced, so the Wi-Fi identity co-appears/co-leaves
// with the persona and adopts the family's archetype. Requires probe_agents_init(n) first.
void phantom_sync_wifi(uint32_t now_ms);

// Align the bound BLE slots [0, phantom_count()) to their phantoms: slot i <-> phantom i.
// Reincarnates slot i whenever phantom i's generation advances, so the BLE identity co-appears
// with the Wi-Fi twin and carries the family's vendor (or a Law-3-safe anonymous RPA).
// Requires ble_devices_init(n) with n >= phantom_count().
void phantom_sync_ble(uint32_t now_ms);
