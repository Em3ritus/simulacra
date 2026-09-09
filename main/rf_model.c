#include <string.h>
#include "rf_model.h"
#include "identity.h"   // IDENTITY_TX_DEFAULT: never return the sentinel
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "rf_model";

void rf_model_reset(rf_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic = RF_MODEL_MAGIC;
    m->version = RF_MODEL_VERSION;
}

size_t rf_itvl_bin(int32_t ms)
{
    if (ms < 50)   return 0;
    if (ms < 100)  return 1;
    if (ms < 200)  return 2;
    if (ms < 500)  return 3;
    if (ms < 1000) return 4;
    if (ms < 2000) return 5;
    return 6;
}

// Classify serialized AD bytes into a structural bucket. Walks the TLV rather than pattern-matching
// so a malformed or truncated advert degrades to RF_ADS_OTHER instead of being mis-binned.
uint8_t rf_adstruct_bin(const uint8_t *ad, uint8_t len)
{
    bool has_flags = false, has_uuid16 = false, has_svcdata = false, has_other = false;
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t l = ad[i];
        if (l == 0) break;                        // trailing zero padding ends the AD
        if ((uint16_t)i + 1 + l > len) break;     // malformed: stop, classify what was valid
        switch (ad[i + 1]) {
            case 0x01: has_flags   = true; break;                 // Flags
            case 0x02: case 0x03: has_uuid16  = true; break;      // 16-bit service uuid list
            case 0x16: has_svcdata = true; break;                 // 16-bit service DATA
            default:   has_other   = true; break;
        }
        i = (uint8_t)(i + 1 + l);
    }
    if (has_svcdata) return RF_ADS_SVCDATA;       // strongest signal: beacon/tracker shaped
    if (has_uuid16)  return RF_ADS_UUID16;
    if (has_flags && !has_other) return RF_ADS_FLAGS_ONLY;
    return RF_ADS_OTHER;
}

void rf_model_observe_adstruct(rf_model_t *m, uint8_t bin)
{
    if (bin < RF_ADSTRUCT_BINS) m->adstruct_bins[bin]++;
}

// Weighted draw over the learned mix. `r` is caller-supplied randomness so this stays pure and
// host-testable. Refuses to answer below a floor: a handful of observations would otherwise let
// one early advert dictate the whole crowd's shape, which is the single-capture overfit again in
// miniature. The caller keeps its cold-start default until the model has actually seen something.
//
// RF_ADS_OTHER is EXCLUDED from the draw. It counts name-only and empty advertisers, shapes the
// generator has no template for and deliberately does not emit. Returning it would force the caller
// to substitute something, and whatever it substituted would be emitted at OTHER's full observed
// weight -- on the 2026-08-25 baseline that was 25% of the no-mfg mass being spent on a shape the
// room contained none of. Redistributing that mass proportionally across the three EMITTABLE bins
// is the honest approximation: it says "of the shapes we can actually make, here is the real mix".
#define RF_ADSTRUCT_MIN_OBS 24
bool rf_adstruct_sample(const rf_model_t *m, uint32_t r, uint8_t *out_bin)
{
    uint32_t all = 0;
    for (size_t b = 0; b < RF_ADSTRUCT_BINS; b++) all += m->adstruct_bins[b];
    if (all < RF_ADSTRUCT_MIN_OBS) return false;      // gate on TOTAL evidence, including OTHER

    uint32_t tot = 0;                                  // but draw only over what we can emit
    for (size_t b = 0; b < RF_ADS_OTHER; b++) tot += m->adstruct_bins[b];
    if (tot == 0) return false;                        // only unrepresentable shapes seen: cold-start
    uint32_t x = r % tot;
    for (size_t b = 0; b < RF_ADS_OTHER; b++) {
        if (x < m->adstruct_bins[b]) { *out_bin = (uint8_t)b; return true; }
        x -= m->adstruct_bins[b];
    }
    *out_bin = RF_ADS_SVCDATA;
    return true;
}

// Classify an MFG-BEARING advert by its most distinctive extra element. Priority order, because a
// device may carry several and the generator emits one variant: a name is the most visible thing an
// advert can add, appearance and tx-power are structural markers, and the absence of a flags
// element is itself distinctive (bare "ff" was 43.1% of one capture's vendor devices).
uint8_t rf_mfgstruct_bin(const uint8_t *ad, uint8_t len)
{
    bool has_flags = false, has_name = false, has_appear = false, has_txp = false;
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t l = ad[i];
        if (l == 0) break;
        if ((uint16_t)i + 1 + l > len) break;
        switch (ad[i + 1]) {
            case 0x01: has_flags  = true; break;
            case 0x08: case 0x09: has_name = true; break;
            case 0x19: has_appear = true; break;
            case 0x0A: has_txp    = true; break;
            default: break;
        }
        i = (uint8_t)(i + 1 + l);
    }
    if (has_name)   return RF_MFGS_NAME;
    if (has_appear) return RF_MFGS_APPEARANCE;
    if (has_txp)    return RF_MFGS_TXPOWER;
    return has_flags ? RF_MFGS_FLAGS_MFG : RF_MFGS_MFG_ONLY;
}

void rf_model_observe_mfgstruct(rf_model_t *m, uint8_t bin)
{
    if (bin < RF_MFGSTRUCT_BINS) m->mfgstruct_bins[bin]++;
}

// Every bucket here is emittable (unlike RF_ADS_OTHER), so the draw spans all of them.
bool rf_mfgstruct_sample(const rf_model_t *m, uint32_t r, uint8_t *out_bin)
{
    uint32_t tot = 0;
    for (size_t b = 0; b < RF_MFGSTRUCT_BINS; b++) tot += m->mfgstruct_bins[b];
    if (tot < RF_ADSTRUCT_MIN_OBS) return false;
    uint32_t x = r % tot;
    for (size_t b = 0; b < RF_MFGSTRUCT_BINS; b++) {
        if (x < m->mfgstruct_bins[b]) { *out_bin = (uint8_t)b; return true; }
        x -= m->mfgstruct_bins[b];
    }
    *out_bin = RF_MFGS_FLAGS_MFG;
    return true;
}

size_t rf_rssi_bin(int8_t rssi)
{
    int idx = (rssi + 100) / 10;          // -100 -> 0, -20 -> 8
    if (idx < 0) idx = 0;
    if (idx >= RF_RSSI_BINS) idx = RF_RSSI_BINS - 1;
    return (size_t)idx;
}

size_t rf_pdu_bin(uint8_t pdu_type)
{
    return (pdu_type < RF_PDU_BINS) ? pdu_type : (RF_PDU_BINS - 1);
}

int rf_vendor_index(const rf_model_t *m, uint16_t company_id)
{
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++)
        if (m->vendors[i].count > 0 && m->vendors[i].company_id == company_id) return (int)i;
    return -1;
}

static uint32_t decayed(uint32_t x)             // subtract 1/DEN, but always at least 1 so small
{ uint32_t d = x / RF_DECAY_DEN; if (d == 0) d = 1; return (x > d) ? x - d : 0; }  // counts eventually reclaim

void rf_model_decay(rf_model_t *m)
{
    // Every histogram bin below must use decayed() (floor-to-nonzero), not a plain x -= x/DEN: for
    // x in 1..RF_DECAY_DEN-1 that division truncates to 0, so the subtraction is a no-op and the
    // bin sticks at that value forever -- a handful of stray low counts from a vendor/interval/RSSI/
    // PDU bin that's since gone silent never actually ages out, quietly biasing the "recent
    // environment" model this function's own contract promises to maintain. count/other_count
    // already used decayed() for exactly this reason; the four histogram arrays didn't.
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
        rf_vendor_t *v = &m->vendors[i];
        if (v->count == 0) continue;
        v->count = decayed(v->count);
        for (size_t b = 0; b < RF_ITVL_BINS; b++) v->itvl_bins[b] = decayed(v->itvl_bins[b]);
        if (v->count == 0) { v->company_id = 0; memset(v->itvl_bins, 0, sizeof v->itvl_bins); }  // free the slot
    }
    if (m->other_count) m->other_count = decayed(m->other_count);
    for (size_t b = 0; b < RF_ITVL_BINS; b++) m->other_itvl_bins[b] = decayed(m->other_itvl_bins[b]);
    for (size_t b = 0; b < RF_RSSI_BINS; b++) m->rssi_bins[b] = decayed(m->rssi_bins[b]);
    for (size_t b = 0; b < RF_PDU_BINS; b++)  m->pdu_bins[b]  = decayed(m->pdu_bins[b]);
    // Structure ages on the same rolling window as everything else. It must: this histogram exists
    // precisely because a FIXED structural mix does not survive a change of environment, so a mix
    // that never aged out would just reproduce the original defect more slowly.
    for (size_t b = 0; b < RF_ADSTRUCT_BINS; b++)
        m->adstruct_bins[b] = decayed(m->adstruct_bins[b]);
    for (size_t b = 0; b < RF_MFGSTRUCT_BINS; b++)
        m->mfgstruct_bins[b] = decayed(m->mfgstruct_bins[b]);
}

void rf_model_observe(rf_model_t *m, uint16_t company_id, int8_t rssi,
                      uint8_t pdu_type, int32_t interval_ms)
{
    m->total_obs++;
    m->rssi_bins[rf_rssi_bin(rssi)]++;
    m->pdu_bins[rf_pdu_bin(pdu_type)]++;

    // Interval folding only. The vendor CENSUS lives in rf_model_observe_arrival(): counting a
    // vendor here would weight it by how chatty its devices are, not by how many of them exist.
    int vi = rf_vendor_index(m, company_id);
    if (vi >= 0) {
        if (interval_ms >= 0) m->vendors[vi].itvl_bins[rf_itvl_bin(interval_ms)]++;
    } else {
        if (interval_ms >= 0) m->other_itvl_bins[rf_itvl_bin(interval_ms)]++;
    }
}

// One ARRIVAL = one device entering the room. Call once per distinct device per sweep, never per
// advertisement.
//
// The vendor histogram drives which company ids the generator emits, so its weighting decides what
// the synthetic crowd claims to be. Counting adverts makes that "which vendor talks most", which is
// a different question with a very different answer: measured overnight on 2026-09-09, a single
// chatty printer emitting 1400 adverts became 96.9% of the model in a room whose five devices were
// 20% that vendor. GEN_MAX_VENDOR_PCT then clamped the output to its 40% ceiling, so the crowd
// looked plausible while the model underneath was wrong by a factor of five -- the guard silently
// absorbing an error it exists to backstop, not to correct.
//
// tools/decoy_audit fixed exactly this on the analysis side (see capture_profile.py's device-weight
// note, which measured an 11x distortion from advert weighting). This is the same fix in the
// firmware that generates the traffic.
void rf_model_observe_arrival(rf_model_t *m, uint16_t company_id)
{
    if (!m) return;
    int vi = rf_vendor_index(m, company_id);
    if (vi < 0) {                          // claim a free slot if any
        for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
            if (m->vendors[i].count == 0) { m->vendors[i].company_id = company_id; vi = (int)i; break; }
        }
    }
    if (vi >= 0) m->vendors[vi].count++;
    else         m->other_count++;         // table full -> overflow bucket
}

void rf_model_end_sweep(rf_model_t *m, uint32_t distinct_devices, uint32_t window_ms,
                        uint32_t arrivals)
{
    m->sweeps++;
    float pop = (float)distinct_devices;
    float arr = window_ms ? ((float)arrivals * 60000.0f / (float)window_ms) : 0.0f;
    const float a = 0.3f;                  // EWMA weight
    if (m->sweeps == 1) { m->pop_ewma = pop; m->arrival_per_min = arr; }
    else {
        m->pop_ewma        = a * pop + (1.0f - a) * m->pop_ewma;
        m->arrival_per_min = a * arr + (1.0f - a) * m->arrival_per_min;
    }
}

void rf_model_dump(const rf_model_t *m)
{
    // Print floats as rounded integers -- avoids the newlib-nano "%f" pitfall on the C6.
    ESP_LOGW(TAG, "MODEL v%u sweeps=%u obs=%u pop=%u arr/min=%u other=%u",
             m->version, (unsigned)m->sweeps, (unsigned)m->total_obs,
             (unsigned)(m->pop_ewma + 0.5f), (unsigned)(m->arrival_per_min + 0.5f),
             (unsigned)m->other_count);
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
        if (m->vendors[i].count == 0) continue;
        const rf_vendor_t *v = &m->vendors[i];
        ESP_LOGW(TAG, "  vendor 0x%04X n=%u itvl[%u %u %u %u %u %u %u]",
                 v->company_id, (unsigned)v->count,
                 (unsigned)v->itvl_bins[0], (unsigned)v->itvl_bins[1], (unsigned)v->itvl_bins[2],
                 (unsigned)v->itvl_bins[3], (unsigned)v->itvl_bins[4], (unsigned)v->itvl_bins[5],
                 (unsigned)v->itvl_bins[6]);
    }
    ESP_LOGW(TAG, "  rssi[%u %u %u %u %u %u %u %u] pdu[%u %u %u %u %u]",
             (unsigned)m->rssi_bins[0], (unsigned)m->rssi_bins[1], (unsigned)m->rssi_bins[2],
             (unsigned)m->rssi_bins[3], (unsigned)m->rssi_bins[4], (unsigned)m->rssi_bins[5],
             (unsigned)m->rssi_bins[6], (unsigned)m->rssi_bins[7],
             (unsigned)m->pdu_bins[0], (unsigned)m->pdu_bins[1], (unsigned)m->pdu_bins[2],
             (unsigned)m->pdu_bins[3], (unsigned)m->pdu_bins[4]);
}

// Weighted index over counts[0..n) using caller-supplied randomness; -1 if all zero.
static int rf_weighted_pick(const uint32_t *counts, size_t n, uint32_t r)
{
    uint64_t total = 0; for (size_t i = 0; i < n; i++) total += counts[i];
    if (total == 0) return -1;
    uint64_t x = (uint64_t)r % total;
    for (size_t i = 0; i < n; i++) { if (x < counts[i]) return (int)i; x -= counts[i]; }
    return (int)n - 1;
}

// Per-identity TX power, shaped to the ambient RSSI SPREAD the model has actually observed.
//
// The old ladder spanned 15 dB ({-12,-9,-6,-3,0,3}), worth sd 5.12, and contained 0 -- which
// churn_adv read as "controller default", i.e. maximum. Measured against three independent
// decoy-free captures whose across-device RSSI sd is a stable 12.3-14.6, the decoy population came
// out at 9.90: identities clustering ~25-30% tighter than a real crowd. That is the "one emitter
// wearing many costumes" tell, and the bench flattered it -- roughly 8.5 dB of the 9.90 came from
// the three boards being physically apart WITH the sniffer among them. To an observer at realistic
// distance those boards collapse toward a single point and only this function's spread remains.
//
// So the spread is drawn from rf_model's rssi_bins, exactly as interval, vendor and AD structure
// are drawn from their histograms. What that reproduces is the SHAPE of ambient's RSSI spread, not
// its absolute level: every decoy shares one physical location, so an observer's distance shifts
// the whole population together and no tx setting can fake being far away. Shape is the part that
// is ours to control, and it is what the audit scores (median-anchored, placement-invariant).
#define TX_MIN_DBM  (-27)      // within ESP32 BLE range; still audible well past typical rooms
#define TX_MAX_DBM  (3)
#define TX_BASE_DBM (-12)      // population centre; low enough that body-worn decoys do not all
                               // read as "right next to the observer"
int8_t rf_tx_sample(const rf_model_t *m, uint32_t r)
{
    int base = TX_BASE_DBM;
    if (m) {
        uint32_t tot = 0;
        for (size_t b = 0; b < RF_RSSI_BINS; b++) tot += m->rssi_bins[b];
        if (tot >= 32) {                       // enough evidence to have a shape at all
            // Weighted median bin, then draw a bin and take the offset between them. Bins are
            // 10 dB wide, so add a sub-bin jitter or the population lands on a 10 dB comb that is
            // itself a signature.
            uint32_t half = tot / 2, run = 0; size_t med = 0;
            for (size_t b = 0; b < RF_RSSI_BINS; b++) {
                run += m->rssi_bins[b];
                if (run >= half) { med = b; break; }
            }
            int pick = rf_weighted_pick(m->rssi_bins, RF_RSSI_BINS, r);
            if (pick >= 0) {
                base += ((int)pick - (int)med) * 10 + (int)((r >> 8) % 10u) - 5;
            }
        }
    } else {
        base += (int)((r >> 8) % 31u) - 15;   // cold start: widened uniform, ~30 dB
    }
    if (base < TX_MIN_DBM) base = TX_MIN_DBM;
    if (base > TX_MAX_DBM) base = TX_MAX_DBM;
    // Never hand back the sentinel: it would mean "controller default" and put this identity at
    // maximum output, which is the collision this whole change exists to remove.
    return (int8_t)(base == IDENTITY_TX_DEFAULT ? TX_MIN_DBM : base);
}

// The NVS pair is excluded from the host audit build (tools/decoy_audit links this file whole so
// the audit exercises the REAL rf_adstruct_sample rather than a stand-in). There is no NVS on a
// host, and roster_stub.c supplies the honest answer -- "no stored model" -- in their place.
#ifndef SIMULACRA_HOST_NO_NVS
int rf_model_save_nvs(const rf_model_t *m)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READWRITE, &h);
    if (e != ESP_OK) return (int)e;
    e = nvs_set_blob(h, "rf_model", m, sizeof(*m));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (int)e;
}

int rf_model_load_nvs(rf_model_t *m)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READONLY, &h);
    if (e != ESP_OK) return (int)e;
    size_t len = sizeof(*m);
    e = nvs_get_blob(h, "rf_model", m, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(*m) ||
        m->magic != RF_MODEL_MAGIC || m->version != RF_MODEL_VERSION) return -1;

    // Restore the environment's SHAPE, never its DENSITY.
    //
    // The histograms (vendor mix, interval bands, RSSI, PDU types) are slow-moving structural
    // knowledge worth carrying across a reboot -- they describe what devices around here look
    // like. pop_ewma and arrival_per_min are neither: they are an instantaneous count of a room
    // the board may no longer be in, and restoring them is actively harmful twice over.
    //
    // 1. CARRIED TO A NEW ROOM. A fleet moved from a busy street to an empty flat boots sized for
    //    the street, and stays that way until enough sweeps decay the estimate.
    //
    // 2. IT LATCHES A MEASURED BUG. The 2026-08-25 capture found a feedback loop: freshly-rotated
    //    fleetmate addresses are unexcluded for up to one broadcast interval, get counted as real
    //    ambient devices, and drive the population up (fleet-wide 32 -> 65 -> 33 -> 42 over an
    //    hour, ambient provably flat throughout). Within a session that self-corrects. But the
    //    inflated pop_ewma is written to flash every OBS_PERSIST_EVERY sweeps, so a reboot during
    //    or after an excursion RESTORES the wrong answer and starts there. Observed directly: a C5
    //    rebooted mid-session came up at active=32 in a room whose true ambient was ~8.
    //
    // Zeroing these means a rebooted board starts at GEN_FLOOR and grows into the room it is
    // actually in, which is the safe direction -- under-populating briefly costs cover, whereas
    // over-populating is the density tell the whole design exists to avoid.
    m->pop_ewma        = 0.0f;
    m->arrival_per_min = 0.0f;
    return 0;
}

#endif  /* SIMULACRA_HOST_NO_NVS */
