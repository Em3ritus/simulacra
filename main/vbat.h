#pragma once
#include <stdint.h>
#include <stdbool.h>

// Battery telemetry with two build-selectable backends (pick one; default = disabled, zero cost):
//
//   MAX17048/MAX17049 fuel gauge over I2C (SparkFun Thing Plus C6, or a Qwiic breakout):
//     -DSIMULACRA_VBAT_MAX17048=1 -DSIMULACRA_VBAT_SDA=<gpio> -DSIMULACRA_VBAT_SCL=<gpio>
//     [-DSIMULACRA_VBAT_LOW_PCT=15]      (flag LOW below this state-of-charge %)
//     e.g. SparkFun Thing Plus C6: SDA=4 SCL=7 (gauge at I2C 0x36).
//
//   ADC voltage divider (Waveshare ESP32-C5-WIFI6-KIT: BAT --200k--+--100k-- GND, node -> GPIO6):
//     -DSIMULACRA_VBAT_ADC=1 -DSIMULACRA_VBAT_ADC_GPIO=<gpio> [-DSIMULACRA_VBAT_ADC_DIV=<n>]
//     [-DSIMULACRA_VBAT_LOW_MV=3400]     (flag LOW below this cell mV)
//     e.g. Waveshare C5: GPIO=6 DIV=3 (200k/100k divider -> Vbat = Vadc*3).
//
// Neither defined -> the backend DEFAULTS BY TARGET in vbat.c (C5 -> ADC on GPIO6, C6 ->
// MAX17048 on 4/7), because the sense hardware is a property of the board, not of the build
// invocation. Nothing passed these flags for a long time, so every image shipped with the sense
// path compiled out and the console reported every node as USB-powered. An unknown target still
// compiles to no-ops returning "absent".

void vbat_init(void);        // set up the backend once at boot; no-op if disabled or none present
const char *vbat_backend(void);  // "max17048" | "adc" | "none" -- which backend was COMPILED IN
bool vbat_present(void);     // true iff a battery / gauge is readable
int  vbat_mv(void);          // fresh cell voltage in mV, or -1 if absent
int  vbat_soc_pct(void);     // fresh state-of-charge %, or -1 if unavailable (ADC backend has none)
bool vbat_low(void);         // present AND below the configured low threshold (early brownout warning)
