#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RF_MODEL_MAGIC    0x52464D31u   // "RFM1"
#define RF_MODEL_VERSION  3             // v2 adds adstruct_bins; v3 device-weights vendors[].count
                                        // (v2 counts are advert-weighted and must not be blended)
#define RF_VENDOR_SLOTS   24
#define RF_ITVL_BINS      7    // <50,50-100,100-200,200-500,500-1000,1000-2000,>2000 ms
#define RF_RSSI_BINS      8    // -100..-20 dBm in 10 dBm steps
#define RF_PDU_BINS       5    // ADV_IND,DIR_IND,SCAN_IND,NONCONN_IND,SCAN_RSP (NimBLE evtype 0..4)
#define RF_VENDOR_UNKNOWN 0xFFFF  // no mfg-data / unknown company id

// AD-STRUCTURE histogram over NO-MFG advertisers -- the axis that was never learned.
//
// Every other generation axis samples this model, which is why interval_distribution (0.001-0.004)
// and vendor_histogram (0.002-0.089) stay closed across every capture tested. AD structure was the
// exception: pick_no_mfg_template() hardcoded ~62% flags-only / ~24% flags+uuid16, fitted in
// 2026-07-13 to a single capture where flags-only advertisers were 52.7% of devices. Cross-
// validation on 2026-08-25 measured that same share at 6.7% and 0.0% in two other stationary
// captures, and ad_structure scored 0.153 on the capture it was tuned to against 0.27-0.93 on
// unseen ones -- the worst number on the scorecard, and the only axis the model could not express.
//
// Four buckets, because pick_no_mfg_template only ever chooses between three no-mfg families and
// needs their relative weight. Finer buckets would not change a single decision.
#define RF_ADSTRUCT_BINS  4
enum {
    RF_ADS_FLAGS_ONLY = 0,   // flags and nothing else -- the terse majority in some environments
    RF_ADS_UUID16,           // flags + a 16-bit service uuid list, no service data
    RF_ADS_SVCDATA,          // carries 16-bit service data (beacon/tracker shaped)
    RF_ADS_OTHER,            // name-only, empty, or anything else with no mfg data
};

// MFG-BEARING structure, the same problem one layer down.
//
// Once the no-mfg mix was learned (2026-08-26) the residual separability moved here.
// enc_vendor_mfg emits exactly one shape, flags+mfg ("01,ff"), and a census across four decoy-free
// captures shows that shape's real share is 100.0% / 50.0% / 15.6% / 0.0%. Identical in character
// to the flags-only share (52.7% / 6.7% / 0.3% / 0.0%) that made the hardcoded no-mfg mix a
// single-capture overfit -- so this is learned rather than replaced with a fixed varied set.
//
// Classified by the most DISTINCTIVE element present, because a device may carry several and the
// generator can only emit one variant. Devices whose extra elements we have no variant for land in
// the closest bucket rather than a catch-all; there is no OTHER here, since every advert in this
// population carries mfg data by definition.
#define RF_MFGSTRUCT_BINS 5
enum {
    RF_MFGS_FLAGS_MFG = 0,   // "01,ff" -- flags + mfg, the only shape ever emitted before
    RF_MFGS_MFG_ONLY,        // "ff"    -- mfg with NO flags element at all
    RF_MFGS_NAME,            // carries a local name (0x08/0x09) alongside mfg
    RF_MFGS_APPEARANCE,      // carries appearance (0x19)
    RF_MFGS_TXPOWER,         // carries tx power level (0x0a)
};

typedef struct {
    uint16_t company_id;
    uint32_t count;
    uint32_t itvl_bins[RF_ITVL_BINS];
} rf_vendor_t;

typedef struct {
    uint32_t    magic;
    uint16_t    version;
    uint32_t    sweeps;
    uint32_t    total_obs;
    rf_vendor_t vendors[RF_VENDOR_SLOTS];
    uint32_t    other_count;
    uint32_t    other_itvl_bins[RF_ITVL_BINS];
    uint32_t    rssi_bins[RF_RSSI_BINS];
    uint32_t    pdu_bins[RF_PDU_BINS];
    uint32_t    adstruct_bins[RF_ADSTRUCT_BINS];    // no-mfg AD shape mix (see RF_ADS_* above)
    uint32_t    mfgstruct_bins[RF_MFGSTRUCT_BINS];  // mfg-bearing shape mix (see RF_MFGS_* above)
    float       pop_ewma;          // EWMA of distinct devices per sweep
    float       arrival_per_min;   // EWMA of new distinct devices per minute
} rf_model_t;

#define RF_DECAY_DEN 4   // rolling-window decay: retain (DEN-1)/DEN of each histogram per closed sweep

void   rf_model_reset(rf_model_t *m);
// Fade old observations into a rolling window so the model tracks the RECENT environment and no
// single loud vendor accumulates unbounded, permanent weight. Call once per closed sweep.
void   rf_model_decay(rf_model_t *m);
// Fold one observation's static features. interval_ms < 0 => no interval sample (first sighting).
void   rf_model_observe(rf_model_t *m, uint16_t company_id, int8_t rssi,
                        uint8_t pdu_type, int32_t interval_ms);
// Fold one NO-MFG advert's structural class. Only called for company_id == RF_VENDOR_UNKNOWN:
// an advert carrying mfg data is shaped by its vendor's template, not by this mix.
// One distinct DEVICE arriving, not one advertisement. Drives the vendor histogram, which decides
// which company ids the generator emits. Counting adverts here weights a vendor by how chatty it is
// rather than how common it is -- see the note in rf_model.c.
void   rf_model_observe_arrival(rf_model_t *m, uint16_t company_id);

void   rf_model_observe_adstruct(rf_model_t *m, uint8_t bin);
// Classify serialized AD bytes into an RF_ADS_* bucket. Pure; shared with the host audit tools.
uint8_t rf_adstruct_bin(const uint8_t *ad, uint8_t len);
// Sample the learned no-mfg structure mix. Returns an RF_ADS_* value, or RF_ADS_OTHER-with-false
// when the model holds too little to sample -- callers then keep their cold-start default.
bool   rf_adstruct_sample(const rf_model_t *m, uint32_t r, uint8_t *out_bin);
// Same three, for the MFG-bearing population. Only called when the advert carries mfg data.
void   rf_model_observe_mfgstruct(rf_model_t *m, uint8_t bin);
uint8_t rf_mfgstruct_bin(const uint8_t *ad, uint8_t len);
bool   rf_mfgstruct_sample(const rf_model_t *m, uint32_t r, uint8_t *out_bin);

// Per-identity TX power (dBm), shaped to the ambient RSSI spread the model has observed.
//
// Lives here rather than in generate.c because BOTH populations need it and they are built by
// different code paths: the unbound crowd through generate_roster, and bound personas through
// ble_device_sync. Giving personas their own fixed band left the two halves of one on-air crowd
// drawing from different distributions, which widened the combined spread past ambient's -- worse
// than either alone. `r` is caller-supplied randomness so this stays pure and host-testable.
// `m` may be NULL: callers without a model get a plausible cold-start spread.
int8_t rf_tx_sample(const rf_model_t *m, uint32_t r);
// Fold a completed sweep's distinct-device aggregates (EWMA).
void   rf_model_end_sweep(rf_model_t *m, uint32_t distinct_devices, uint32_t window_ms,
                          uint32_t arrivals);

size_t rf_itvl_bin(int32_t ms);
size_t rf_rssi_bin(int8_t rssi);
size_t rf_pdu_bin(uint8_t pdu_type);
int    rf_vendor_index(const rf_model_t *m, uint16_t company_id);  // occupied slot or -1

void   rf_model_dump(const rf_model_t *m);

// NVS persistence: namespace "splinter", key "rf_model". Return 0 on success.
int    rf_model_save_nvs(const rf_model_t *m);
int    rf_model_load_nvs(rf_model_t *m);   // 0 and fills m if a valid current-version blob exists
