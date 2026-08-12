#include "fleet_status.h"
#include <string.h>

// Age of a record, guarding against a timestamp from the FUTURE.
//
// The caller samples `now` once per UI frame, but records get stamped later within that same frame
// (the ESP-NOW RX drain runs mid-loop), so last_ms can legitimately exceed now by a few ms. Plain
// unsigned subtraction turns that into ~49 days, so every node that had *just* reported read as
// STALE: the fleet aggregate emptied to 0 devices / 0 threats for one frame every second, and the
// display flipped RADAR -> HOME -> RADAR continuously. Treat a future stamp as age zero.
static uint32_t node_age_ms(uint32_t now_ms, uint32_t last_ms)
{
    int32_t d = (int32_t)(now_ms - last_ms);
    return d < 0 ? 0u : (uint32_t)d;
}

void fleet_status_reset(fleet_status_t *f){ memset(f, 0, sizeof(*f)); }

void fleet_status_forget(fleet_status_t *f, uint8_t node_id)
{
    for (int i = 0; i < FLEET_STATUS_MAX; i++)
        if (f->nodes[i].used && f->nodes[i].id == node_id) { memset(&f->nodes[i], 0, sizeof f->nodes[i]); return; }
}

void fleet_status_upsert(fleet_status_t *f, uint8_t node_id, const radar_wire_status_t *st, uint32_t now_ms)
{
    int free_slot = -1;
    for (int i = 0; i < FLEET_STATUS_MAX; i++) {
        if (f->nodes[i].used && f->nodes[i].id == node_id) {
            f->nodes[i].st = *st; f->nodes[i].last_ms = now_ms; return;
        }
        if (!f->nodes[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        f->nodes[free_slot].used = true; f->nodes[free_slot].id = node_id;
        f->nodes[free_slot].st = *st; f->nodes[free_slot].last_ms = now_ms;
    }   // table full: drop (v1 fleet <= FLEET_STATUS_MAX)
}

void fleet_status_prune(fleet_status_t *f, uint32_t now_ms, uint32_t max_age_ms)
{
    for (int i = 0; i < FLEET_STATUS_MAX; i++)
        if (f->nodes[i].used && node_age_ms(now_ms, f->nodes[i].last_ms) >= max_age_ms)
            memset(&f->nodes[i], 0, sizeof f->nodes[i]);
}

int fleet_status_count(const fleet_status_t *f)
{ int n = 0; for (int i = 0; i < FLEET_STATUS_MAX; i++) if (f->nodes[i].used) n++; return n; }

bool fleet_status_at(const fleet_status_t *f, int i, uint8_t *id,
                     const radar_wire_status_t **st, bool *alive, uint32_t now_ms)
{
    int seen = 0;
    for (int k = 0; k < FLEET_STATUS_MAX; k++) {
        if (!f->nodes[k].used) continue;
        if (seen++ != i) continue;
        if (id) *id = f->nodes[k].id;
        if (st) *st = &f->nodes[k].st;
        if (alive) *alive = node_age_ms(now_ms, f->nodes[k].last_ms) < FLEET_STATUS_STALE_MS;
        return true;
    }
    return false;
}

uint32_t fleet_status_age_ms(const fleet_status_t *f, int i, uint32_t now_ms)
{
    int seen = 0;
    for (int k = 0; k < FLEET_STATUS_MAX; k++) {
        if (!f->nodes[k].used) continue;
        if (seen++ != i) continue;
        return node_age_ms(now_ms, f->nodes[k].last_ms);
    }
    return 0;
}

static uint16_t sat_add16(uint16_t a, uint16_t b){ uint32_t s = (uint32_t)a + b; return s > 0xFFFFu ? 0xFFFFu : (uint16_t)s; }
static uint8_t  sat_add8 (uint8_t  a, uint8_t  b){ uint16_t s = (uint16_t)a + b; return s > 0xFFu ? 0xFFu : (uint8_t)s; }

void fleet_status_aggregate(const fleet_status_t *f, uint32_t now_ms, radar_wire_status_t *out)
{
    memset(out, 0, sizeof(*out));
    int agg_preset = -1;   // -1 = no alive node yet
    for (int i = 0; i < FLEET_STATUS_MAX; i++) {
        const fleet_node_t *nd = &f->nodes[i];
        if (!nd->used) continue;
        if (node_age_ms(now_ms, nd->last_ms) >= FLEET_STATUS_STALE_MS) continue;   // alive nodes only
        const radar_wire_status_t *st = &nd->st;
        if (agg_preset == -1)              agg_preset = st->preset;   // first alive node
        else if (agg_preset != st->preset) agg_preset = 0xFE;         // disagreement -> MIXED
        out->active_devices = sat_add16(out->active_devices, st->active_devices);
        out->roster_size    = sat_add16(out->roster_size, st->roster_size);
        out->probes_sent   += st->probes_sent;
        out->total_obs     += st->total_obs;
        out->active_target  = sat_add8(out->active_target, st->active_target);
        out->form_restless  = sat_add8(out->form_restless, st->form_restless);
        out->form_wandering = sat_add8(out->form_wandering, st->form_wandering);
        out->form_bound     = sat_add8(out->form_bound, st->form_bound);
        if (st->uptime_s > out->uptime_s) out->uptime_s = st->uptime_s;
        if (st->epoch    > out->epoch)    out->epoch    = st->epoch;
        if (st->pop_ewma > out->pop_ewma) out->pop_ewma = st->pop_ewma;
        out->flags |= st->flags;
        // Union the followers by hash: a device followed by ANY node is one fleet threat. Keep the
        // closest sighting (highest best_rssi) and the strongest recurrence / known-class detail.
        uint8_t tc = st->threat_count; if (tc > RADAR_MAX_THREATS) tc = RADAR_MAX_THREATS;
        for (uint8_t t = 0; t < tc; t++) {
            int found = -1;
            for (uint8_t k = 0; k < out->threat_count; k++)
                if (out->threats[k].hash == st->threats[t].hash) { found = k; break; }
            if (found >= 0) {
                if (st->threats[t].best_rssi   > out->threats[found].best_rssi)   out->threats[found].best_rssi   = st->threats[t].best_rssi;
                if (st->threats[t].epochs      > out->threats[found].epochs)      out->threats[found].epochs      = st->threats[t].epochs;
                if (st->threats[t].sessions_seen > out->threats[found].sessions_seen) out->threats[found].sessions_seen = st->threats[t].sessions_seen;
                if (st->threats[t].places_seen > out->threats[found].places_seen) out->threats[found].places_seen = st->threats[t].places_seen;
                if (out->threats[found].kind != DETECT_KIND_KNOWN && st->threats[t].kind == DETECT_KIND_KNOWN) {
                    out->threats[found].kind       = DETECT_KIND_KNOWN;
                    out->threats[found].class_id   = st->threats[t].class_id;
                    out->threats[found].category   = st->threats[t].category;
                    out->threats[found].confidence = st->threats[t].confidence;
                    out->threats[found].vendor     = st->threats[t].vendor;
                }
            } else if (out->threat_count < RADAR_MAX_THREATS) {
                out->threats[out->threat_count++] = st->threats[t];
            }
        }
    }
    out->preset = (agg_preset == -1) ? 0xFF : (uint8_t)agg_preset;
}
