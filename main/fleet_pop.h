#pragma once
#include <stdint.h>
// Fleet population share. In a mesh of K decoy nodes sharing one environment, each node runs 1/K of
// the fleet-wide population so the aggregate matches observed density (design law 4: population-match)
// and the crowd originates from K physical points instead of one.
//
// K is the LIVE node census (peers heard recently + self), refreshed by the coexist tick, with
// -DSIMULACRA_FLEET_SIZE=K as an optional lower bound for deployments that know their size up front.
// It used to be the static constant alone, defaulting to 1 -- and nothing ever set it, so every node
// in a 3-node fleet sized its crowd as if standalone and the room saw ~3x the intended BLE density,
// re-opening the density tell that population-match exists to close. The Wi-Fi side already used the
// live census; this makes both radios agree.

#ifndef SIMULACRA_FLEET_SIZE
#define SIMULACRA_FLEET_SIZE 1
#endif

// Fleet size K = max(live census, SIMULACRA_FLEET_SIZE), clamped to >= 1 so the divisor is always
// safe. Reads the cached census (see fleet_pop_refresh) -- no clock needed, callable from anywhere.
int fleet_pop_size(void);

// Recompute the cached live census. Call once per coordinator tick; cheap (a small table scan).
// Until it is first called the census is 1 (standalone), which is the correct boot-time answer:
// no peers have been heard yet, so this node legitimately owns the whole crowd.
void fleet_pop_refresh(uint32_t now_ms);

// round(target / k), floored at 1 for target > 0 (a node never zeroes a whole population class).
// Pure in k -> testable at any k without recompiling. target <= 0 or k <= 1 returns target unchanged.
int fleet_pop_share_k(int target, int k);

// This node's share of a fleet-wide target: fleet_pop_share_k(target, fleet_pop_size()).
int fleet_pop_share(int target);

// Live fleet size: distinct peer NODES heard from recently (fleet_node_count), + this node.
// Falls back to 1 (standalone) with no peers heard -- the correct, safe default, achieved for
// free (fleet_node_count returns 0 when nothing has been noted yet).
int fleet_pop_live_size(uint32_t now_ms);
