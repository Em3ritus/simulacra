#pragma once
#include <stddef.h>
#include <stdint.h>
#include "identity.h"
#include "rf_model.h"

// Library of pre-generated synthetic behaviours (payload / interval / company / archetype).
// ble_devices.c draws an entry at random when it spawns a device and copies the behaviour,
// stamping a fresh address of its own -- the roster entry's own address is discarded.
//
// This used to be a pool cycled IDLE -> ACTIVE -> COOLDOWN by churn.c. Milestone A moved
// lifetime and presence into ble_devices.c and the state machine stopped being driven; every
// entry sat permanently IDLE, so the states, the promote/cooldown API and the three per-identity
// state fields were removed rather than left as a misleading abstraction.
#define CHURN_ROSTER_SIZE 256

void        roster_init(void);
identity_t *roster_at(size_t i);                        // draw source for ble_devices; also tests
void        make_random_static_addr_pub(uint8_t out[6]);// always static-random (top bits 11)
void        make_random_addr(uint8_t out[6], uint8_t top2);   // random addr with given top-2-bits
void        make_random_addr_mixed(uint8_t out[6]);    // random addr, realistic static/RPA/NRPA mix
// M8 live re-profiling: regenerate the behaviour library from a fresh model. Devices already on
// air keep the behaviour they copied at spawn, so the room-matched shapes phase in as devices are
// reborn -- no hard swap.
void        roster_reseed(const rf_model_t *m);
