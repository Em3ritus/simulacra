#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef _MSC_VER
#define __attribute__(x)
#endif
#include "radar_wire.h"

#define FLEET_STATUS_MAX       8
#define FLEET_STATUS_STALE_MS  12000u   // no status this long -> node reads "silent"

typedef struct { uint8_t id; radar_wire_status_t st; uint32_t last_ms; bool used; } fleet_node_t;
typedef struct { fleet_node_t nodes[FLEET_STATUS_MAX]; } fleet_status_t;

void   fleet_status_reset(fleet_status_t *f);
// Drop one node's record, because its id is being recycled for a DIFFERENT device. Without this the
// new occupant inherits the departed node's decoy/threat counts until its own status lands.
void   fleet_status_forget(fleet_status_t *f, uint8_t node_id);
// Drop records silent for longer than max_age_ms. Nodes are keyed by a MAC the decoys re-randomise
// every boot, so a reboot or reflash leaves the old identity behind as a permanently SILENT record.
// Those never expired: they occupied slots in a 4-entry table of which HOME can only draw 3, so a
// dead entry in an early slot pushed a LIVE node off the display -- a board that looked like it had
// dropped off while it was still meshing. Should be well above FLEET_STATUS_STALE_MS so a briefly
// quiet node still shows as SILENT rather than vanishing.
void   fleet_status_prune(fleet_status_t *f, uint32_t now_ms, uint32_t max_age_ms);
void   fleet_status_upsert(fleet_status_t *f, uint8_t node_id, const radar_wire_status_t *st, uint32_t now_ms);
int    fleet_status_count(const fleet_status_t *f);                       // used slots
bool   fleet_status_at(const fleet_status_t *f, int i, uint8_t *id,
                       const radar_wire_status_t **st, bool *alive, uint32_t now_ms);
// Milliseconds since node i (i-th used slot, same indexing as fleet_status_at) last reported. 0 if absent.
uint32_t fleet_status_age_ms(const fleet_status_t *f, int i, uint32_t now_ms);
// Fold every ALIVE node into one fleet-wide status for the sub-views: counts sum (saturating),
// epoch/uptime/pop take the max, flags OR together, and threats union by hash (closest RSSI +
// strongest recurrence/known-class kept), capped at RADAR_MAX_THREATS. `out` is fully written.
void   fleet_status_aggregate(const fleet_status_t *f, uint32_t now_ms, radar_wire_status_t *out);
