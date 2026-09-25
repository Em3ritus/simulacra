/* sim_fleet - synthetic fleet telemetry for the Vigil console simulator.
 *
 * THIS IS THE ONLY FICTION IN vigil_sim. Everything downstream of it is the real firmware:
 * fleet_status.c does the aggregation, radar_ui.c runs the view state machine, and
 * radar_render.c + radar_gfx.c draw every pixel. This file stands in for the radio, producing
 * the radar_wire_status_t frames that decoys would otherwise send over ESP-NOW.
 *
 * It is deliberately NOT a model of decoy behaviour and must never be mistaken for one -- the
 * numbers it emits are plausible, not measured. Nothing here belongs in an audit path.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fleet_status.h"
#include "radar_render.h"
#include "exposure.h"

#define SIM_MAX_NODES 4

typedef struct {
    uint8_t  id;
    bool     present;          /* still reporting at all */
    bool     degraded;         /* TX self-health fault -> node card reads DEGRADED */
    uint32_t boot_ms;          /* for uptime_s */
    uint32_t next_report_ms;   /* each node reports on its own cadence, like the real mesh */
    uint16_t report_period_ms;
    uint16_t decoys;           /* active_devices */
    uint16_t pop_ewma;         /* observed ambient density -- drives CLOAKED vs EXPOSED */
    uint32_t probes_sent;
    uint16_t battery_mv;
    uint8_t  battery_pct;      /* 0xFF = ADC backend, unavailable */
    uint8_t  preset;
    bool     paused;           /* flags bit0 -> DARK posture */
} sim_node_t;

typedef struct {
    uint32_t hash;             /* identity key fleet_status_aggregate unions on */
    uint16_t vendor;
    uint8_t  kind;             /* DETECT_KIND_FOLLOWER | DETECT_KIND_KNOWN */
    uint8_t  class_id, category, confidence;
    int8_t   best_rssi;
    uint8_t  epochs, sessions_seen, places_seen;
    bool     active;
} sim_threat_t;

typedef struct {
    sim_node_t     node[SIM_MAX_NODES];
    sim_threat_t   threat[RADAR_MAX_THREATS];
    fleet_status_t fleet;                       /* REAL aggregation state */
    uint32_t       rng;
    uint32_t       start_ms;
    uint16_t       epoch;
    uint32_t       next_epoch_ms;
    int            story_step;                  /* -1 = story disabled */
    uint32_t       next_story_ms;

    /* EXPOSURE session: real exposure.c state, fed a synthetic probe stream */
    exposure_t expo;
    uint32_t   expo_next_ms;
    uint8_t    expo_phone_seq;

    /* rebuilt each frame from the real fleet_status_* API */
    radar_wire_status_t agg;
    radar_wire_status_t view_st[FLEET_STATUS_MAX];
    radar_node_view_t   views[FLEET_STATUS_MAX];
    int                 view_count;
} sim_fleet_t;

void sim_fleet_init(sim_fleet_t *s, uint32_t now_ms, bool story);
/* Advance the simulation and push any due node reports through fleet_status_upsert(). */
void sim_fleet_tick(sim_fleet_t *s, uint32_t now_ms);
/* Rebuild views[]/agg from the REAL fleet_status_* aggregation. Call after tick, before render. */
void sim_fleet_refresh(sim_fleet_t *s, uint32_t now_ms);

/* --- operator-driven events, so a behaviour can be demonstrated on cue --- */
void sim_fleet_join(sim_fleet_t *s, uint32_t now_ms);        /* bring a node online */
void sim_fleet_drop(sim_fleet_t *s, uint32_t now_ms);        /* newest node stops reporting */
void sim_fleet_degrade(sim_fleet_t *s, uint32_t now_ms);     /* TX fault + battery slump */
void sim_fleet_add_threat(sim_fleet_t *s, uint32_t now_ms, bool known);
void sim_fleet_escalate(sim_fleet_t *s, uint32_t now_ms);    /* push a threat toward HUNTED */
void sim_fleet_clear_threats(sim_fleet_t *s);
void sim_fleet_cycle_preset(sim_fleet_t *s);                 /* cycles; desyncs one node -> MIXED */
void sim_fleet_toggle_pause(sim_fleet_t *s);                 /* -> DARK posture */
void sim_fleet_toggle_crowd(sim_fleet_t *s);                 /* ambient density -> EXPOSED */

/* --- EXPOSURE (the leaked-SSID check) ---------------------------------------
 * Drives the REAL exposure.c state machine with an invented probe stream: a few steady ambient
 * devices, plus one that bursts during WATCH carrying named SSIDs -- the phone whose saved
 * networks the check is meant to reveal. The SSIDs are made up, in the router-default style the
 * project's own ssid_pool.c uses. None of them came from a capture. */
void              sim_expo_start(sim_fleet_t *s, uint32_t now_ms);
void              sim_expo_tick(sim_fleet_t *s, uint32_t now_ms);
bool              sim_expo_running(const sim_fleet_t *s);
const exposure_t *sim_expo_state(const sim_fleet_t *s);

const char *sim_fleet_last_event(const sim_fleet_t *s);
