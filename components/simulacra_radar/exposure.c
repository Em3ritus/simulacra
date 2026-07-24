#include "exposure.h"
#include <string.h>

static expo_dev_t *find_or_add(exposure_t *e, uint32_t fp)
{
    for (int i = 0; i < e->ndev; i++) if (e->dev[i].fp == fp) return &e->dev[i];
    if (e->ndev >= EXPO_MAX_DEVICES) return 0;
    expo_dev_t *d = &e->dev[e->ndev++];
    memset(d, 0, sizeof *d);
    d->fp = fp;
    return d;
}

void expo_reset(exposure_t *e) { memset(e, 0, sizeof *e); e->state = EXPO_IDLE; e->winner = -1; }

void expo_start(exposure_t *e, uint32_t now_ms)
{
    memset(e, 0, sizeof *e);
    e->state = EXPO_BASELINE; e->t_phase = now_ms; e->winner = -1;
}

void expo_probe(exposure_t *e, uint32_t fp, const char *ssid, uint8_t ssid_len, uint32_t now_ms)
{
    (void)now_ms;
    if (e->state != EXPO_BASELINE && e->state != EXPO_WATCH) return;
    expo_dev_t *d = find_or_add(e, fp);
    if (!d) return;
    if (e->state == EXPO_BASELINE) { if (d->base_n  < 0xFFFF) d->base_n++;  }
    else                           { if (d->watch_n < 0xFFFF) d->watch_n++; }
    if (ssid && ssid_len && ssid_len <= EXPO_SSID_MAX && d->nssid < EXPO_MAX_SSIDS) {
        for (int i = 0; i < d->nssid; i++)                       // dedup
            if (strncmp(d->ssid[i], ssid, ssid_len) == 0 && d->ssid[i][ssid_len] == 0) return;
        memcpy(d->ssid[d->nssid], ssid, ssid_len);
        d->ssid[d->nssid][ssid_len] = 0;
        d->nssid++;
    }
}

static void resolve(exposure_t *e)
{
    // spike = watch_n - baseline expectation (baseline rate scaled to the watch window).
    // Pick the max positive spike >= EXPO_MIN_SPIKE.
    int best = -1, best_spike = 0;
    for (int i = 0; i < e->ndev; i++) {
        int expected = (int)((long)e->dev[i].base_n * EXPO_WATCH_MS / EXPO_BASELINE_MS);
        int spike = (int)e->dev[i].watch_n - expected;
        if (spike > best_spike) { best_spike = spike; best = i; }
    }
    e->winner = (best >= 0 && best_spike >= EXPO_MIN_SPIKE) ? best : -1;
}

void expo_tick(exposure_t *e, uint32_t now_ms)
{
    if (e->state == EXPO_BASELINE && (now_ms - e->t_phase) >= EXPO_BASELINE_MS) {
        e->state = EXPO_WATCH; e->t_phase = now_ms;
    } else if (e->state == EXPO_WATCH && (now_ms - e->t_phase) >= EXPO_WATCH_MS) {
        resolve(e); e->state = EXPO_RESULT;
    }
}

bool     expo_have_result(const exposure_t *e) { return e->state == EXPO_RESULT; }
bool     expo_ambiguous(const exposure_t *e)   { return e->state == EXPO_RESULT && e->winner < 0; }
uint32_t expo_winner_fp(const exposure_t *e)   { return e->winner >= 0 ? e->dev[e->winner].fp : 0; }
int      expo_winner_probes(const exposure_t *e){ return e->winner >= 0 ? e->dev[e->winner].watch_n : 0; }

int expo_winner_ssids(const exposure_t *e, const char **out, int max)
{
    if (e->winner < 0) return 0;
    const expo_dev_t *d = &e->dev[e->winner];
    int n = d->nssid < max ? d->nssid : max;
    for (int i = 0; i < n; i++) out[i] = d->ssid[i];
    return n;
}
