#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "probe_frame.h"
#include "ssid_pool.h"
#define AGENT_SSID_MAX 3        // a real phone's active saved-network set is small

// Independent fake-phone "agents". Each owns its identity (MAC + archetype), its own 802.11
// sequence counter, a jittered scan schedule, and a bounded lifetime. The population turns over
// (birth/death) so the set never stabilizes into a constellation fingerprint. Pure/host-testable:
// no ESP radio/timer calls; the clock arrives as now_ms, randomness via esp_random().
#define PROBE_AGENTS_MAX 16

typedef enum { DUTY_ACTIVE, DUTY_IDLE } probe_duty_t;

typedef struct {
    uint8_t      mac[6];
    probe_arch_t arch;          // bound to the MAC for the agent's whole life
    uint16_t     seq;           // 12-bit, monotonic per agent
    probe_duty_t duty;
    uint32_t     next_scan_ms;
    uint32_t     next_mac_rotate_ms;   // intra-life Wi-Fi MAC rotation (independent of the BLE RPA timer)
    uint32_t     born_ms;
    uint32_t     life_ms;       // bounded lifetime; on expiry the agent dies + reincarnates
    bool         alive;
    uint32_t     persona_gen;   // generation of the phantom this agent is bound to (0 = unbound)
    uint8_t      ssid_n;                    // # assigned named SSIDs; 0 = wildcard-only for this life
    uint8_t      ssid_idx[AGENT_SSID_MAX];  // indices into ssid_pool (assigned once per life)
    uint16_t     ssid_sfx[AGENT_SSID_MAX];  // per-persona suffix seed -> a stable per-router name
} probe_agent_t;

void     probe_agents_init(int n, uint32_t now_ms);          // (re)seed n agents (<= PROBE_AGENTS_MAX)
// Adjust the live agent set to n (clamped to [1, PROBE_AGENTS_MAX]): spawn to grow, drop to shrink.
// The Wi-Fi population-match knob (mirrors churn_set_active_target on the BLE side).
void     probe_agents_set_target(int n, uint32_t now_ms);
// Intra-life Wi-Fi MAC rotation ONLY (no birth/death): any agent past its rotation deadline gets a
// fresh MAC + sequence number, keeping arch/duty/born/life/persona_gen. Returns #rotated.
//
// Split out of probe_agents_lifecycle so the coexist build can rotate without also running the
// standalone reincarnation logic (persona-bound agents get their lifetime from phantom.c). The
// combined decoy calls THIS from coexist_task; only SIMULACRA_PROBE calls the lifecycle.
int      probe_agents_rotate_tick(uint32_t now_ms);
int      probe_agents_lifecycle(uint32_t now_ms);            // reincarnate expired + rotate; returns #reborn
int      probe_agents_due(uint32_t now_ms, probe_agent_t **out, int max);  // due subset; reschedules them
uint16_t probe_agent_next_seq(probe_agent_t *a);             // return current seq, then +1 (12-bit wrap)
int      probe_agents_count(void);
const probe_agent_t *probe_agents_at(int i);

// Bind agent i to a persona (see phantom.h): if the agent's recorded generation differs from
// `generation`, reincarnate it with a fresh unique MAC, the given archetype, and the persona's
// shared born/life. Returns 1 if reincarnated this call, else 0. Bound agents do NOT expire via
// probe_agents_lifecycle; the persona owns their lifetime.
int probe_agent_sync(int i, probe_arch_t arch, uint32_t born_ms, uint32_t life_ms, uint32_t generation);

// Choose this burst's SSID for agent a: a pool string (sets *len_out) to probe a NAMED network, or
// this burst's SSID for agent a: renders one of the agent's assigned pool names (with its stable
// per-persona suffix) into `out`, returning the byte length; returns 0 (wildcard burst) for a
// wildcard-only agent or a wildcard roll. Uses esp_random for the roll; does not mutate the agent.
uint8_t probe_agent_pick_ssid(const probe_agent_t *a, char *out, uint8_t outmax);

// Move `current` toward `target` by at most `step` (magnitude), never overshooting. Pure: the
// glide's step arithmetic, isolated from the jitter clock so it is directly unit-testable.
int probe_glide_next(int current, int target, int step);

// Record the desired applied population. The FIRST call after probe_agents_init applies immediately
// (boot-instant, no ramp); later calls only record it — probe_agents_glide_tick ramps toward it by
// GLIDE_STEP per jittered per-node interval. now_ms seeds/advances the glide clock.
void probe_agents_glide_set_target(int target, uint32_t now_ms);

// Advance the glide: if the per-node jittered interval has elapsed and the applied count differs
// from the desired target, step it one toward the target. Self-gating; call it every Wi-Fi burst.
void probe_agents_glide_tick(uint32_t now_ms);
