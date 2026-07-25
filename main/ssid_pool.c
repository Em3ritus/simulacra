#include "ssid_pool.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

// Curated generic network names + draw weights, calibrated to a real decoy-free driving capture
// (private/workdrive2.kismet, 13457 devices): the SSIDs that real devices actually probe are
// overwhelmingly ROUTER/GATEWAY DEFAULTS -- not the public retail hotspots we first guessed (xfinitywifi
// et al. appeared ZERO times). So the pool is now default-heavy: manufacturer/ISP gateway defaults +
// generic guest names, all ubiquitous and NON-IDENTIFYING (a probe for "NETGEAR" implies nothing about
// this user -- millions of PNLs hold it). SAFETY: never a personal, business, or observed/local SSID.
// Popular ones recur across personas = realistic overlap. Contents are data -- freely editable.
static const struct { const char *name; uint8_t weight; uint8_t sfx; } POOL[] = {
    // router / gateway defaults (the bulk of real named probes). Suffix styles mirror the real
    // per-unit patterns seen in the driving capture (spectrumsetup-4c, ARRIS-a3f1, NETGEAR92).
    { "NETGEAR",        18, SSID_SFX_DIGIT },
    { "XFINITY",        15, SSID_SFX_NONE  },
    { "Linksys",        13, SSID_SFX_NONE  },
    { "spectrumsetup",  12, SSID_SFX_HEX2  },
    { "MySpectrumWiFi", 10, SSID_SFX_NONE  },
    { "Orbi",            8, SSID_SFX_NONE  },
    { "eero",            7, SSID_SFX_NONE  },
    { "NETGEAR-Guest",   7, SSID_SFX_NONE  },
    { "TP-Link_2.4GHz",  6, SSID_SFX_NONE  },
    { "ARRIS",           6, SSID_SFX_HEX4  },
    { "CenturyLink",     6, SSID_SFX_NONE  },
    { "ATT-Fiber",       5, SSID_SFX_NONE  },
    { "dlink",           5, SSID_SFX_NONE  },
    { "Frontier",        4, SSID_SFX_NONE  },
    { "Belkin.setup",    4, SSID_SFX_NONE  },
    // generic / IoT-setup / guest -- extremely common, non-identifying
    { "Guest",          10, SSID_SFX_NONE  },
    { "Home",            8, SSID_SFX_NONE  },
    { "setup",           6, SSID_SFX_HEX4  },
    { "wifi",            5, SSID_SFX_NONE  },
    { "GuestWiFi",       5, SSID_SFX_NONE  },
    // still-in-many-PNLs open hotspots, kept minimal (rare in the real capture)
    { "xfinitywifi",     5, SSID_SFX_NONE  },
    { "attwifi",         4, SSID_SFX_NONE  },
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

uint8_t ssid_pool_suffix_style(int i) { return (i >= 0 && i < POOL_N) ? POOL[i].sfx : 0; }

uint8_t ssid_pool_render(int i, uint16_t seed, char *out, uint8_t outmax)
{
    if (i < 0 || i >= POOL_N || !out || outmax == 0) return 0;
    int n = 0;
    switch (POOL[i].sfx) {
        case SSID_SFX_HEX2:  n = snprintf(out, outmax, "%s-%02x", POOL[i].name, seed & 0xff); break;
        case SSID_SFX_HEX4:  n = snprintf(out, outmax, "%s-%04x", POOL[i].name, seed);        break;
        case SSID_SFX_DIGIT: n = snprintf(out, outmax, "%s%u",    POOL[i].name, 10 + seed % 90u); break;
        default:             n = snprintf(out, outmax, "%s",      POOL[i].name);               break;
    }
    if (n < 0) { out[0] = 0; return 0; }
    if (n >= outmax) n = outmax - 1;                 // snprintf truncated; report the written length
    return (uint8_t)n;
}
