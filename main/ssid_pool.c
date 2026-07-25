#include "ssid_pool.h"
#include "esp_random.h"
#include <string.h>

// Curated generic network names + draw weights, calibrated to a real decoy-free driving capture
// (private/workdrive2.kismet, 13457 devices): the SSIDs that real devices actually probe are
// overwhelmingly ROUTER/GATEWAY DEFAULTS -- not the public retail hotspots we first guessed (xfinitywifi
// et al. appeared ZERO times). So the pool is now default-heavy: manufacturer/ISP gateway defaults +
// generic guest names, all ubiquitous and NON-IDENTIFYING (a probe for "NETGEAR" implies nothing about
// this user -- millions of PNLs hold it). SAFETY: never a personal, business, or observed/local SSID.
// Popular ones recur across personas = realistic overlap. Contents are data -- freely editable.
static const struct { const char *name; uint8_t weight; } POOL[] = {
    // router / gateway defaults (the bulk of real named probes)
    { "NETGEAR",        18 },
    { "XFINITY",        15 },
    { "Linksys",        13 },
    { "spectrumsetup",  12 },
    { "MySpectrumWiFi", 10 },
    { "Orbi",            8 },
    { "eero",            7 },
    { "NETGEAR-Guest",   7 },
    { "TP-Link_2.4GHz",  6 },
    { "ARRIS",           6 },
    { "CenturyLink",     6 },
    { "ATT-Fiber",       5 },
    { "dlink",           5 },
    { "Frontier",        4 },
    { "Belkin.setup",    4 },
    // generic / IoT-setup / guest -- extremely common, non-identifying
    { "Guest",          10 },
    { "Home",            8 },
    { "setup",           6 },
    { "wifi",            5 },
    { "GuestWiFi",       5 },
    // still-in-many-PNLs open hotspots, kept minimal (rare in the real capture)
    { "xfinitywifi",     5 },
    { "attwifi",         4 },
};
#define POOL_N ((int)(sizeof(POOL) / sizeof(POOL[0])))

int ssid_pool_count(void) { return POOL_N; }

const char *ssid_pool_at(int i, uint8_t *len_out)
{
    if (i < 0 || i >= POOL_N) return 0;
    if (len_out) *len_out = (uint8_t)strlen(POOL[i].name);
    return POOL[i].name;
}

int ssid_pool_pick_weighted(void)
{
    uint32_t total = 0;
    for (int i = 0; i < POOL_N; i++) total += POOL[i].weight;
    if (!total) return 0;
    uint32_t r = esp_random() % total;
    for (int i = 0; i < POOL_N; i++) {
        if (r < POOL[i].weight) return i;
        r -= POOL[i].weight;
    }
    return 0;
}
