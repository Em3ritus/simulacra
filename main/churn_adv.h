#pragma once
#include <stdint.h>
#include "identity.h"

#define CHURN_HW_INSTANCES 4

// Apply: make hardware `instance` advertise `id` (stop, (re)configure with the
// identity's interval, set random addr, set data, start). Safe to call from the
// churn task after NimBLE host sync. Returns 0 on success, else the failing rc.
int churn_adv_apply(uint8_t instance, const identity_t *id);
// Stop: take hardware `instance` off the air (PAUSE). Same task rules as apply.
int churn_adv_stop(uint8_t instance);
