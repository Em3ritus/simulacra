#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "exposure.h"

// Reads a scripted event stream on stdin and prints a RESULT line for the test harness.
//   start <ms> | probe <fp_hex> <ms> [ssid] | tick <ms>
int main(void)
{
    exposure_t e; expo_reset(&e);
    char line[128], cmd[16], ssid[64];
    unsigned long ms; unsigned fp;
    while (fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "%15s", cmd) != 1) continue;
        if      (!strcmp(cmd, "start") && sscanf(line, "%*s %lu", &ms) == 1) expo_start(&e, (uint32_t)ms);
        else if (!strcmp(cmd, "tick")  && sscanf(line, "%*s %lu", &ms) == 1) expo_tick(&e, (uint32_t)ms);
        else if (!strcmp(cmd, "probe")) {
            int got = sscanf(line, "%*s %x %lu %63s", &fp, &ms, ssid);
            if (got >= 2) expo_probe(&e, (uint32_t)fp, got == 3 ? ssid : 0,
                                     got == 3 ? (uint8_t)strlen(ssid) : 0, (uint32_t)ms);
        }
    }
    const char *S[] = { "IDLE", "BASELINE", "WATCH", "RESULT" };
    const char *sl[EXPO_MAX_SSIDS]; int ns = expo_winner_ssids(&e, sl, EXPO_MAX_SSIDS);
    printf("RESULT state=%s winner_fp=0x%02x probes=%d ambiguous=%d ssids=",
           S[e.state], expo_winner_fp(&e), expo_winner_probes(&e), expo_ambiguous(&e) ? 1 : 0);
    for (int i = 0; i < ns; i++) printf("%s%s", i ? "," : "", sl[i]);
    printf("\n");
    return 0;
}
