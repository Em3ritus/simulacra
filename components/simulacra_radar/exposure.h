#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EXPO_BASELINE_MS 4000u
#define EXPO_WATCH_MS    6000u
#define EXPO_MIN_SPIKE   3        // min extra probes over baseline expectation to count as "your" burst
#define EXPO_MAX_DEVICES 24
#define EXPO_MAX_SSIDS   8
#define EXPO_SSID_MAX    32

typedef enum { EXPO_IDLE, EXPO_BASELINE, EXPO_WATCH, EXPO_RESULT } expo_state_t;

typedef struct {
    uint32_t fp;
    uint16_t base_n, watch_n;                       // probe counts per phase
    uint8_t  nssid;
    char     ssid[EXPO_MAX_SSIDS][EXPO_SSID_MAX + 1];
} expo_dev_t;

typedef struct {
    expo_state_t state;
    uint32_t     t_phase;                           // ms at entry of the current timed phase
    int          ndev;
    expo_dev_t   dev[EXPO_MAX_DEVICES];
    int          winner;                            // index into dev[], -1 if none/ambiguous
} exposure_t;

void expo_reset(exposure_t *e);                     // -> IDLE, cleared
void expo_start(exposure_t *e, uint32_t now_ms);    // IDLE/RESULT -> BASELINE
// Feed one sniffed probe. ssid NULL/len 0 = wildcard. Ignored unless BASELINE or WATCH.
void expo_probe(exposure_t *e, uint32_t fp, const char *ssid, uint8_t ssid_len, uint32_t now_ms);
void expo_tick(exposure_t *e, uint32_t now_ms);     // advances BASELINE->WATCH->RESULT on the timers

bool     expo_have_result(const exposure_t *e);     // state == RESULT
bool     expo_ambiguous(const exposure_t *e);       // RESULT but no clear spike (winner < 0)
uint32_t expo_winner_fp(const exposure_t *e);       // 0 if none
int      expo_winner_probes(const exposure_t *e);   // winner watch_n, 0 if none
int      expo_winner_ssids(const exposure_t *e, const char **out, int max);  // # named SSIDs, fills out[]
