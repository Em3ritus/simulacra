#include "probe_agents.h"
#include "esp_random.h"
#include "ssid_pool.h"
#include <string.h>

#define LIFE_MIN_MS    60000u    // 1 min
#define LIFE_MAX_MS    600000u   // 10 min
#define ACTIVE_MIN_MS  4000u
#define ACTIVE_MAX_MS  16000u
#define IDLE_MIN_MS    30000u
#define IDLE_MAX_MS    180000u
#define PERSONA_MAC_ROT_MIN_MS 480000u   // 8 min  (Wi-Fi MAC intra-life rotation, fast-realistic)
#define PERSONA_MAC_ROT_MAX_MS 900000u   // 15 min
#define SSID_ASSIGN_PCT      62   // % of personas that get a named-SSID set (rest wildcard for life)
#define SSID_BURST_NAMED_PCT 60   // for an assigned persona, % of bursts that name a network (on-air realism)
#define GLIDE_STEP    1        // move the applied population one agent at a time (device-faithful)
#define GLIDE_MIN_MS  30000u   // per-node jittered step interval: lower bound (~30 s)
#define GLIDE_MAX_MS  60000u   // upper bound (~60 s); each step re-draws independently via esp_random

static probe_agent_t s_agents[PROBE_AGENTS_MAX];
static int           s_n;
static int      s_glide_target;       // desired applied count the glide is ramping toward
static bool     s_glide_armed;        // false until the first glide_set_target (boot-instant gate)
static uint32_t s_next_glide_ms;      // earliest time the next +/-1 step may apply

static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + (esp_random() % (hi - lo + 1u)); }
static uint32_t persona_mac_rotate_base(void) { return rnd_range(PERSONA_MAC_ROT_MIN_MS, PERSONA_MAC_ROT_MAX_MS); }

// Draw this persona's saved-network set ONCE (called only from birth sites). ~SSID_ASSIGN_PCT of
// personas get 1..AGENT_SSID_MAX distinct pool entries; the rest stay wildcard-only for their life.
static void assign_ssids(probe_agent_t *a)
{
    a->ssid_n = 0;
    if ((esp_random() % 100u) >= SSID_ASSIGN_PCT) return;         // wildcard-only persona
    int want = 1 + (int)(esp_random() % (uint32_t)AGENT_SSID_MAX);
    for (int tries = 0; tries < 16 && a->ssid_n < want; tries++) {
        int idx = ssid_pool_pick_weighted();
        int dup = 0;
        for (int j = 0; j < a->ssid_n; j++) if (a->ssid_idx[j] == (uint8_t)idx) dup = 1;
        if (!dup) {
            a->ssid_sfx[a->ssid_n] = (uint16_t)esp_random();     // stable per-router suffix seed
            a->ssid_idx[a->ssid_n++] = (uint8_t)idx;
        }
    }
}

static void agent_spawn(probe_agent_t *a, uint32_t now_ms)
{
    probe_random_mac(a->mac);
    a->arch    = probe_pick_archetype();
    a->seq     = (uint16_t)(esp_random() & 0x0FFFu);             // fresh random 12-bit base
    a->duty    = (esp_random() % 3u == 0u) ? DUTY_ACTIVE : DUTY_IDLE;  // ~33% active
    a->born_ms = now_ms;
    a->life_ms = rnd_range(LIFE_MIN_MS, LIFE_MAX_MS);
    a->alive   = true;
    uint32_t base = (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                             : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
    a->next_scan_ms = now_ms + (esp_random() % base);            // random phase-in (not all due at once)
    a->next_mac_rotate_ms = now_ms + persona_mac_rotate_base();
    assign_ssids(a);
}

void probe_agents_init(int n, uint32_t now_ms)
{
    if (n > PROBE_AGENTS_MAX) n = PROBE_AGENTS_MAX;
    if (n < 1) n = 1;
    s_n = n;
    s_glide_armed = false;                         // next glide_set_target is treated as boot (instant)
    for (int i = 0; i < s_n; i++) agent_spawn(&s_agents[i], now_ms);
}

void probe_agents_set_target(int n, uint32_t now_ms)
{
    if (n < 1) n = 1;
    if (n > PROBE_AGENTS_MAX) n = PROBE_AGENTS_MAX;
    for (int i = s_n; i < n; i++) agent_spawn(&s_agents[i], now_ms);   // grow: spawn the new slots
    s_n = n;                                                           // shrink: higher slots go dormant
}

uint16_t probe_agent_next_seq(probe_agent_t *a)
{
    uint16_t s = a->seq;
    a->seq = (a->seq + 1u) & 0x0FFFu;
    return s;
}

int probe_agents_count(void) { return s_n; }
const probe_agent_t *probe_agents_at(int i) { return (i >= 0 && i < s_n) ? &s_agents[i] : 0; }

int probe_agents_lifecycle(uint32_t now_ms)
{
    int reborn = 0;
    for (int i = 0; i < s_n; i++) {
        probe_agent_t *a = &s_agents[i];
        if (a->alive && (now_ms - a->born_ms) >= a->life_ms) {
            agent_spawn(a, now_ms);      // dies, then reincarnates with a fresh random identity
            reborn++;
            continue;
        }
        // intra-life MAC rotation: a real phone rotates its Wi-Fi MAC within a session, independent of
        // the BLE RPA. Fresh privacy identity; keeps arch/duty/born/life/persona_gen (the binding).
        if (a->alive && (int32_t)(now_ms - a->next_mac_rotate_ms) >= 0) {
            probe_random_mac(a->mac);
            a->seq = (uint16_t)(esp_random() & 0x0FFFu);
            a->next_mac_rotate_ms = now_ms + persona_mac_rotate_base();
        }
    }
    return reborn;
}

static uint32_t next_interval(const probe_agent_t *a)
{
    return (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                    : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
}

int probe_agents_due(uint32_t now_ms, probe_agent_t **out, int max)
{
    int k = 0;
    for (int i = 0; i < s_n && k < max; i++) {
        probe_agent_t *a = &s_agents[i];
        if (a->alive && (int32_t)(now_ms - a->next_scan_ms) >= 0) {
            out[k++] = a;
            a->next_scan_ms = now_ms + next_interval(a);   // reschedule with jittered per-duty interval
        }
    }
    return k;
}

int probe_agent_sync(int i, probe_arch_t arch, uint32_t born_ms, uint32_t life_ms, uint32_t generation)
{
    if (i < 0 || i >= s_n) return 0;
    probe_agent_t *a = &s_agents[i];
    if (a->persona_gen == generation && a->alive) return 0;   // already synced to this life
    probe_random_mac(a->mac);                                  // fresh unique MAC
    a->arch        = arch;                                     // adopt persona family's archetype
    a->seq         = (uint16_t)(esp_random() & 0x0FFFu);
    a->duty        = (esp_random() % 3u == 0u) ? DUTY_ACTIVE : DUTY_IDLE;
    a->born_ms     = born_ms;
    a->life_ms     = life_ms;
    a->alive       = true;
    a->persona_gen = generation;
    uint32_t base  = (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                              : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
    a->next_scan_ms = born_ms + (esp_random() % base);
    a->next_mac_rotate_ms = born_ms + persona_mac_rotate_base();
    assign_ssids(a);
    return 1;
}

uint8_t probe_agent_pick_ssid(const probe_agent_t *a, char *out, uint8_t outmax)
{
    if (a->ssid_n == 0) return 0;                                // wildcard-only persona
    if ((esp_random() % 100u) >= SSID_BURST_NAMED_PCT) return 0; // this burst is wildcard
    uint8_t which = (uint8_t)(esp_random() % a->ssid_n);
    return ssid_pool_render(a->ssid_idx[which], a->ssid_sfx[which], out, outmax);  // its OWN suffixed name
}

int probe_glide_next(int current, int target, int step)
{
    if (step < 0) step = -step;
    if (current < target) { int d = target - current; return current + (d < step ? d : step); }
    if (current > target) { int d = current - target; return current - (d < step ? d : step); }
    return current;
}

void probe_agents_glide_set_target(int target, uint32_t now_ms)
{
    s_glide_target = target;
    if (!s_glide_armed) {                          // boot: apply the first target at once (no ramp)
        s_glide_armed = true;
        probe_agents_set_target(target, now_ms);
        s_next_glide_ms = now_ms + rnd_range(GLIDE_MIN_MS, GLIDE_MAX_MS);
    }
}

void probe_agents_glide_tick(uint32_t now_ms)
{
    if (!s_glide_armed) return;                    // nothing to glide toward yet
    if ((int32_t)(now_ms - s_next_glide_ms) < 0) return;   // still within the jittered interval
    int cur = probe_agents_count();
    if (cur == s_glide_target) return;             // already there
    probe_agents_set_target(probe_glide_next(cur, s_glide_target, GLIDE_STEP), now_ms);
    s_next_glide_ms = now_ms + rnd_range(GLIDE_MIN_MS, GLIDE_MAX_MS);   // re-arm with a fresh draw
}
