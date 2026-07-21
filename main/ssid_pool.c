#include "ssid_pool.h"
#include "esp_random.h"
#include <string.h>

// Curated generic PUBLIC network names + draw weights (popular ones recur across personas = realistic
// overlap). All are well-documented open/carrier/retail hotspot SSIDs found in countless PNLs; none
// implies a specific private/home network. Contents are data -- freely editable without redesign.
static const struct { const char *name; uint8_t weight; } POOL[] = {
    { "xfinitywifi",       30 },
    { "attwifi",           22 },
    { "XFINITY",           12 },
    { "Google Starbucks",  10 },
    { "eduroam",            8 },
    { "GuestWiFi",          8 },
    { "Guest",              7 },
    { "SpectrumWiFi",       6 },
    { "optimumwifi",        6 },
    { "Boingo Hotspot",     4 },
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
