#pragma once
#include <stdint.h>

// A fixed, compiled-in pool of GENERIC PUBLIC SSIDs (ubiquitous open/carrier/retail hotspot names
// that appear in a very large number of real devices' preferred-network lists everywhere). A decoy
// probing one is indistinguishable from the real background and reveals nothing about THIS user.
//
// SAFETY: this pool is the ONLY source of directed-probe SSIDs. It is NEVER populated from observed
// or learned traffic -- probing a locally-real SSID would announce the user's actual associations.
// This header/impl must not include any observe/learn/capture source (structurally tested).
#define SSID_POOL_MAX_LEN 32   // 802.11 SSID element maximum

int         ssid_pool_count(void);
const char *ssid_pool_at(int i, uint8_t *len_out);   // NUL-terminated name + byte length; NULL if OOR
int         ssid_pool_pick_weighted(void);           // weighted random index (uses esp_random)
