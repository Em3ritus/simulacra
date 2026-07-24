#pragma once
#include <stdint.h>
#include "exposure.h"

// Promiscuous probe-request sniffer for the CYD exposure view. Feeds a MAC/SSID-independent IE
// fingerprint + any named SSID into the exposure state machine `e`. Modal: while active the CYD
// is off the ESP-NOW channel (hops 1/6/11); expo_sniff_stop() restores channel 1.
void expo_sniff_start(exposure_t *e, uint32_t now_ms);
void expo_sniff_tick(uint32_t now_ms);      // call each CYD loop iteration to hop channels
void expo_sniff_stop(void);
