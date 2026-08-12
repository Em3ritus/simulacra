#include "fleet_pop.h"
#include "fleet.h"

// Cached live census so fleet_pop_size() stays clock-free (settings.c and the boot path have no
// `now` to hand). Refreshed once per coexist tick.
static int s_live = 1;

void fleet_pop_refresh(uint32_t now_ms)
{
    int k = fleet_pop_live_size(now_ms);
    s_live = k < 1 ? 1 : k;
}

int fleet_pop_size(void)
{
    int k = s_live;                       // peers actually heard right now
    if (SIMULACRA_FLEET_SIZE > k) k = SIMULACRA_FLEET_SIZE;   // operator-declared lower bound
    return k < 1 ? 1 : k;
}

int fleet_pop_share_k(int target, int k)
{
    if (target <= 0 || k <= 1) return target;
    int s = (target + k / 2) / k;   // round to nearest
    return s < 1 ? 1 : s;           // never drop a whole population class to zero
}

int fleet_pop_share(int target)
{
    return fleet_pop_share_k(target, fleet_pop_size());
}

int fleet_pop_live_size(uint32_t now_ms)
{
    return (int)fleet_node_count(now_ms) + 1;
}
