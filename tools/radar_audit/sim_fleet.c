#include "sim_fleet.h"
#include <string.h>
#include <stdio.h>

/* Nodes are pruned well above FLEET_STATUS_STALE_MS (12 s) so a dropped node first reads SILENT and
 * only later vanishes -- that two-stage behaviour is the real pruning logic and is worth showing. */
#define SIM_PRUNE_MS   30000u
#define SIM_EPOCH_MS   20000u

static char s_event[64] = "";

static uint32_t rnd(sim_fleet_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (s->rng = x);
}
static int rnd_range(sim_fleet_t *s, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(rnd(s) % (uint32_t)(hi - lo + 1));
}
static void note(const char *fmt, int a)
{
    snprintf(s_event, sizeof s_event, fmt, a);
}

const char *sim_fleet_last_event(const sim_fleet_t *s) { (void)s; return s_event; }

/* ---------------------------------------------------------------- node -> wire frame */

/* NOTE: radar_wire_status_t carries no TX-self-health field, so "degraded" here shows up the way it
 * actually would on the wire -- a collapsed decoy count and a slumping cell -- rather than as a
 * flag this file invents. A degraded node also stops adopting new presets, which is what makes the
 * fleet read MIXED after a preset change. */
static void build_status(sim_fleet_t *s, const sim_node_t *n, uint32_t now, radar_wire_status_t *o)
{
    memset(o, 0, sizeof *o);
    o->uptime_s       = (now - n->boot_ms) / 1000u;
    o->flags          = n->paused ? 0x01u : 0x00u;
    o->active_devices = n->paused ? 0 : n->decoys;
    o->roster_size    = (uint16_t)(n->decoys + 4);
    o->probes_sent    = n->probes_sent;
    o->epoch          = s->epoch;
    o->pop_ewma       = n->pop_ewma;
    o->total_obs      = n->probes_sent * 3u + 128u;
    o->active_target  = (uint8_t)(n->decoys > 255 ? 255 : n->decoys);
    o->battery_mv     = n->battery_mv;
    o->battery_pct    = n->battery_pct;
    o->preset         = n->preset;

    /* BLE shade-form split (RPA / NRPA / static), summing to the live decoy count. */
    uint16_t live = o->active_devices;
    o->form_restless  = (uint8_t)(live / 2);
    o->form_wandering = (uint8_t)(live / 4);
    o->form_bound     = (uint8_t)(live - o->form_restless - o->form_wandering);

    int tc = 0;
    for (int i = 0; i < RADAR_MAX_THREATS && tc < RADAR_MAX_THREATS; i++) {
        const sim_threat_t *t = &s->threat[i];
        if (!t->active) continue;
        o->threats[tc].hash          = t->hash;
        o->threats[tc].vendor        = t->vendor;
        o->threats[tc].epochs        = t->epochs;
        o->threats[tc].best_rssi     = (int8_t)(t->best_rssi + rnd_range(s, -3, 3));
        o->threats[tc].first_epoch   = (uint16_t)(s->epoch > t->epochs ? s->epoch - t->epochs : 0);
        o->threats[tc].last_epoch    = s->epoch;
        o->threats[tc].kind          = t->kind;
        o->threats[tc].class_id      = t->class_id;
        o->threats[tc].category      = t->category;
        o->threats[tc].confidence    = t->confidence;
        o->threats[tc].sessions_seen = t->sessions_seen;
        o->threats[tc].places_seen   = t->places_seen;
        tc++;
    }
    o->threat_count = (uint8_t)tc;
}

/* ---------------------------------------------------------------- lifecycle */

void sim_fleet_init(sim_fleet_t *s, uint32_t now_ms, bool story)
{
    memset(s, 0, sizeof *s);
    s->rng          = 0xC0FFEEu;          /* fixed seed: the demo replays identically */
    s->start_ms     = now_ms;
    s->epoch        = 41;
    s->next_epoch_ms = now_ms + SIM_EPOCH_MS;
    s->story_step   = story ? 0 : -1;
    s->next_story_ms = now_ms + 2000u;
    fleet_status_reset(&s->fleet);
    expo_reset(&s->expo);

    for (int i = 0; i < SIM_MAX_NODES; i++) {
        sim_node_t *n = &s->node[i];
        n->id               = (uint8_t)(i + 1);
        n->present          = (i == 0);   /* one node up at boot; the rest join */
        n->boot_ms          = now_ms;
        n->report_period_ms = (uint16_t)(2300 + i * 370);   /* deliberately not in lockstep */
        n->next_report_ms   = now_ms + 200u * (uint32_t)i;
        n->decoys           = (uint16_t)(24 + i * 4);
        n->pop_ewma         = (uint16_t)(58 + i * 7);
        n->battery_mv       = (uint16_t)(4050 - i * 120);
        n->battery_pct      = (uint8_t)(92 - i * 9);
        n->preset           = 0;
        n->probes_sent      = (uint32_t)(1200 + i * 310);
    }
    snprintf(s_event, sizeof s_event, "boot");
}

static void auto_story(sim_fleet_t *s, uint32_t now)
{
    if (s->story_step < 0 || now < s->next_story_ms) return;
    switch (s->story_step) {
        case 0: sim_fleet_join(s, now);              s->next_story_ms = now + 5000;  break;
        case 1: sim_fleet_join(s, now);              s->next_story_ms = now + 7000;  break;
        case 2: sim_fleet_add_threat(s, now, true);  s->next_story_ms = now + 9000;  break;
        case 3: sim_fleet_escalate(s, now);          s->next_story_ms = now + 8000;  break;
        case 4: sim_fleet_degrade(s, now);           s->next_story_ms = now + 6000;  break;
        case 5: sim_fleet_cycle_preset(s);           s->next_story_ms = now + 8000;  break;
        case 6: sim_fleet_drop(s, now);              s->next_story_ms = now + 14000; break;
        case 7: sim_fleet_clear_threats(s);          s->next_story_ms = now + 6000;  break;
        default: s->story_step = 0; s->next_story_ms = now + 5000; return;
    }
    s->story_step++;
}

void sim_fleet_tick(sim_fleet_t *s, uint32_t now_ms)
{
    if (now_ms >= s->next_epoch_ms) { s->epoch++; s->next_epoch_ms = now_ms + SIM_EPOCH_MS; }
    auto_story(s, now_ms);

    for (int i = 0; i < SIM_MAX_NODES; i++) {
        sim_node_t *n = &s->node[i];
        if (!n->present || now_ms < n->next_report_ms) continue;
        n->next_report_ms = now_ms + n->report_period_ms;

        /* plausible drift: crowd wanders, decoys track it, battery trickles down */
        int d = rnd_range(s, -2, 2);
        int pop = (int)n->pop_ewma + rnd_range(s, -4, 4);
        if (pop < 0)   pop = 0;
        if (pop > 400) pop = 400;
        n->pop_ewma = (uint16_t)pop;

        int dec = (int)n->decoys + d;
        if (dec < 0)  dec = 0;
        if (dec > 96) dec = 96;
        n->decoys = (uint16_t)dec;

        n->probes_sent += (uint32_t)rnd_range(s, 3, 11);
        if (n->battery_mv > 3400 && (rnd(s) & 7u) == 0u) {
            n->battery_mv -= 1;
            if (n->battery_pct != 0xFF && n->battery_pct > 0 && (rnd(s) & 31u) == 0u) n->battery_pct--;
        }

        radar_wire_status_t st;
        build_status(s, n, now_ms, &st);
        fleet_status_upsert(&s->fleet, n->id, &st, now_ms);
    }

    fleet_status_prune(&s->fleet, now_ms, SIM_PRUNE_MS);
}

void sim_fleet_refresh(sim_fleet_t *s, uint32_t now_ms)
{
    s->view_count = 0;
    int used = fleet_status_count(&s->fleet);
    for (int i = 0; i < used && s->view_count < FLEET_STATUS_MAX; i++) {
        uint8_t id = 0; const radar_wire_status_t *st = NULL; bool alive = false;
        if (!fleet_status_at(&s->fleet, i, &id, &st, &alive, now_ms) || !st) continue;
        int k = s->view_count++;
        s->view_st[k]      = *st;                 /* own the copy; the table may churn */
        s->views[k].id     = id;
        s->views[k].st     = &s->view_st[k];
        s->views[k].alive  = alive;
        s->views[k].age_s  = fleet_status_age_ms(&s->fleet, i, now_ms) / 1000u;
    }
    fleet_status_aggregate(&s->fleet, now_ms, &s->agg);
}

/* ---------------------------------------------------------------- operator events */

void sim_fleet_join(sim_fleet_t *s, uint32_t now_ms)
{
    for (int i = 0; i < SIM_MAX_NODES; i++) {
        if (s->node[i].present) continue;
        s->node[i].present        = true;
        s->node[i].degraded       = false;
        s->node[i].boot_ms        = now_ms;
        s->node[i].next_report_ms = now_ms;
        note("node %d joined", s->node[i].id);
        return;
    }
    note("all %d nodes already up", SIM_MAX_NODES);
}

void sim_fleet_drop(sim_fleet_t *s, uint32_t now_ms)
{
    (void)now_ms;
    for (int i = SIM_MAX_NODES - 1; i >= 0; i--) {
        if (!s->node[i].present) continue;
        s->node[i].present = false;
        note("node %d went silent", s->node[i].id);
        return;
    }
    note("no nodes to drop%d", 0);
}

void sim_fleet_degrade(sim_fleet_t *s, uint32_t now_ms)
{
    (void)now_ms;
    for (int i = 0; i < SIM_MAX_NODES; i++) {
        if (!s->node[i].present || s->node[i].degraded) continue;
        s->node[i].degraded   = true;
        s->node[i].decoys     = (uint16_t)(s->node[i].decoys / 4);
        s->node[i].battery_mv = 3520;
        if (s->node[i].battery_pct != 0xFF) s->node[i].battery_pct = 11;
        note("node %d degraded (low batt)", s->node[i].id);
        return;
    }
    note("no healthy node to degrade%d", 0);
}

void sim_fleet_add_threat(sim_fleet_t *s, uint32_t now_ms, bool known)
{
    (void)now_ms;
    for (int i = 0; i < RADAR_MAX_THREATS; i++) {
        sim_threat_t *t = &s->threat[i];
        if (t->active) continue;
        t->active        = true;
        t->hash          = rnd(s) | 1u;
        t->kind          = known ? DETECT_KIND_KNOWN : DETECT_KIND_FOLLOWER;
        t->vendor        = known ? (uint16_t)0x004C : (uint16_t)rnd_range(s, 1, 900);
        t->class_id      = known ? (uint8_t)rnd_range(s, 1, 3) : 0;
        t->category      = known ? 1 : 0;
        t->confidence    = known ? (uint8_t)rnd_range(s, 70, 95) : (uint8_t)rnd_range(s, 35, 60);
        t->best_rssi     = (int8_t)rnd_range(s, -82, -49);
        t->epochs        = (uint8_t)rnd_range(s, 2, 5);
        t->sessions_seen = 1;
        t->places_seen   = 1;
        note("threat %d appeared", i + 1);
        return;
    }
    note("threat table full (%d)", RADAR_MAX_THREATS);
}

void sim_fleet_escalate(sim_fleet_t *s, uint32_t now_ms)
{
    (void)now_ms;
    for (int i = 0; i < RADAR_MAX_THREATS; i++) {
        sim_threat_t *t = &s->threat[i];
        if (!t->active) continue;
        if (t->epochs        < 250) t->epochs        = (uint8_t)(t->epochs + 6);
        if (t->sessions_seen < 250) t->sessions_seen = (uint8_t)(t->sessions_seen + 1);
        if (t->places_seen   < 250) t->places_seen   = (uint8_t)(t->places_seen + 1);
        if (t->confidence    <  99) t->confidence    = (uint8_t)(t->confidence + 3);
        if (t->best_rssi     < -40) t->best_rssi     = (int8_t)(t->best_rssi + 5);
        note("threat %d escalated", i + 1);
        return;
    }
    note("no threat to escalate%d", 0);
}

void sim_fleet_clear_threats(sim_fleet_t *s)
{
    for (int i = 0; i < RADAR_MAX_THREATS; i++) s->threat[i].active = false;
    note("threats cleared%d", 0);
}

void sim_fleet_cycle_preset(sim_fleet_t *s)
{
    uint8_t next = 0;
    for (int i = 0; i < SIM_MAX_NODES; i++) {
        if (!s->node[i].present || s->node[i].degraded) continue;
        next = (uint8_t)((s->node[i].preset + 1u) % RADAR_CTRL_PRESET_COUNT);
        break;
    }
    int held = 0;
    for (int i = 0; i < SIM_MAX_NODES; i++) {
        if (!s->node[i].present) continue;
        if (s->node[i].degraded) { held++; continue; }   /* a degraded node keeps the old preset */
        s->node[i].preset = next;
    }
    if (held) note("preset -> %d (a node held back: MIXED)", next);
    else      note("preset -> %d (fleet agrees)", next);
}

void sim_fleet_toggle_pause(sim_fleet_t *s)
{
    bool any = false;
    for (int i = 0; i < SIM_MAX_NODES; i++) if (s->node[i].present && !s->node[i].paused) any = true;
    for (int i = 0; i < SIM_MAX_NODES; i++) if (s->node[i].present) s->node[i].paused = any;
    snprintf(s_event, sizeof s_event, any ? "decoys paused (DARK)" : "decoys resumed");
}

void sim_fleet_toggle_crowd(sim_fleet_t *s)
{
    bool empty = false;
    for (int i = 0; i < SIM_MAX_NODES; i++) if (s->node[i].present && s->node[i].pop_ewma > 10) empty = true;
    for (int i = 0; i < SIM_MAX_NODES; i++) {
        if (!s->node[i].present) continue;
        s->node[i].pop_ewma = empty ? (uint16_t)rnd_range(s, 0, 2) : (uint16_t)rnd_range(s, 52, 74);
    }
    snprintf(s_event, sizeof s_event, empty ? "ambient crowd gone (EXPOSED)" : "ambient crowd back");
}

/* ---------------------------------------------------------------- EXPOSURE simulation */

/* Invented, in the router-default style ssid_pool.c uses. NOT from any capture. The last one is
 * deliberately personal-looking, because that is the point the screen is making: a phone shouts
 * the names of networks it remembers, and one of them is usually your home. */
static const char *PHONE_SSID[] = { "NETGEAR73", "spectrumsetup-a4", "Hartley-5G", "CoffeeHouse_Guest" };
#define PHONE_FP   0x5Au
#define AMBIENT_N  3
static const uint32_t AMBIENT_FP[AMBIENT_N] = { 0x11u, 0x22u, 0x33u };

void sim_expo_start(sim_fleet_t *s, uint32_t now_ms)
{
    expo_reset(&s->expo);
    expo_start(&s->expo, now_ms);
    s->expo_next_ms   = now_ms;
    s->expo_phone_seq = 0;
    snprintf(s_event, sizeof s_event, "exposure scan started");
}

bool sim_expo_running(const sim_fleet_t *s)
{
    return s->expo.state == EXPO_BASELINE || s->expo.state == EXPO_WATCH;
}

const exposure_t *sim_expo_state(const sim_fleet_t *s) { return &s->expo; }

void sim_expo_tick(sim_fleet_t *s, uint32_t now_ms)
{
    if (sim_expo_running(s)) {
        /* One slot every 120 ms. Ambient devices probe at a flat rate through BOTH phases, so they
         * establish a baseline and never spike. The phone is quiet during BASELINE and loud during
         * WATCH, which is exactly the shape expo_probe's winner test looks for. */
        while ((int32_t)(now_ms - s->expo_next_ms) >= 0) {
            uint32_t t = s->expo_next_ms;
            for (int i = 0; i < AMBIENT_N; i++)
                if ((rnd(s) & 3u) == 0u) expo_probe(&s->expo, AMBIENT_FP[i], NULL, 0, t);

            if (s->expo.state == EXPO_BASELINE) {
                if ((rnd(s) & 15u) == 0u) expo_probe(&s->expo, PHONE_FP, NULL, 0, t);
            } else {
                /* the burst: every slot, cycling through the saved-network names */
                const char *ss = PHONE_SSID[s->expo_phone_seq % (uint8_t)(sizeof PHONE_SSID / sizeof PHONE_SSID[0])];
                s->expo_phone_seq++;
                expo_probe(&s->expo, PHONE_FP, ss, (uint8_t)strlen(ss), t);
            }
            s->expo_next_ms = t + 120u;
        }
    }
    expo_tick(&s->expo, now_ms);
}
