#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe_frame.h"
#include "probe_agents.h"
#include "uniq_id.h"
#include "phantom.h"
#include "wifi_density.h"
#include "ssid_pool.h"
#include "surveil_oui.h"

/*
 * Host dumper for the probe-request archetype builder.
 *
 *   probe_dump <arch_idx> <channel> <band5:0|1>   -> one hex line: the built frame
 *   probe_dump --pick <seed> <n>                  -> n lines, each a picked archetype index
 *
 * A fixed source MAC keeps frame output deterministic for byte-exact fixtures.
 */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--agentrot") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ticks  = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 35;
        unsigned tickms = argc > 4 ? (unsigned)strtoul(argv[4], 0, 10) : 60000;
        srand(seed);
        probe_agents_init(1, 0);
        probe_agent_sync(0, ARCH_IPHONE, 0, 2400000u, 1);   // bound agent, 40 min life, gen 1
        char last[13] = "";
        uint32_t t = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            probe_agents_lifecycle(t);
            const probe_agent_t *a = probe_agents_at(0);
            char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", a->mac[b]);
            if (strcmp(hex, last) != 0) { printf("%u %s %u\n", (unsigned)t, hex, (unsigned)a->persona_gen); strcpy(last, hex); }
        }
        return 0;
    }
    // TURBO mode: unbound agents (no persona binding), MAC rotation only, on the fast turbo band
    // instead of the 8-15 min persona band. Guard against the mode ever silently reverting to the
    // slow band.
    if (argc > 1 && strcmp(argv[1], "--turborot") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ticks  = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 20;
        unsigned tickms = argc > 4 ? (unsigned)strtoul(argv[4], 0, 10) : 1000;
        srand(seed);
        probe_agents_set_turbo(1, 0);
        probe_agents_set_target(1, 0);
        char last[13] = "";
        uint32_t t = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            probe_agents_rotate_tick(t);
            const probe_agent_t *a = probe_agents_at(0);
            char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", a->mac[b]);
            if (strcmp(hex, last) != 0) { printf("%u %s\n", (unsigned)t, hex); strcpy(last, hex); }
        }
        return 0;
    }
    // Regression guard for the I-1 fix: init normally (turbo off -- agents land on the normal
    // 1-10 min life / 30-180s idle-scan / 8-15 min persona-band MAC rotation), run some ticks so
    // some agents are genuinely idle and mid-way through a slow rotation deadline, THEN flip
    // turbo on mid-run. Before the fix, probe_agents_set_turbo only touched a flag read at
    // spawn time, so a pre-existing idle agent stayed idle on the slow rotation band for the rest
    // of the turbo session; the fix forces duty=ACTIVE and pulls next_mac_rotate_ms into the
    // turbo band on the spot. Prints "A <t> <slot> active|idle" once right after the switch (duty
    // snapshot), then "R <t> <slot> <mac>" for every rotation observed afterward.
    if (argc > 1 && strcmp(argv[1], "--agents-lateturbo") == 0) {
        unsigned seed      = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n         = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        int      pre_ticks = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 5;
        int      post_ticks= argc > 5 ? (int)strtoul(argv[5], 0, 10) : 20;
        unsigned tickms    = argc > 6 ? (unsigned)strtoul(argv[6], 0, 10) : 1000;
        srand(seed);
        probe_agents_init(n, 0);                        // turbo OFF: normal duty mix + slow rotation
        uint32_t t = 0;
        for (int s = 0; s < pre_ticks; s++) { t += tickms; probe_agents_rotate_tick(t); }
        probe_agents_set_turbo(1, t);                    // flip mid-run: the fix under test
        for (int i = 0; i < n; i++) {
            const probe_agent_t *a = probe_agents_at(i);
            printf("A %u %d %s\n", (unsigned)t, i, a->duty == DUTY_ACTIVE ? "active" : "idle");
        }
        static char last[PROBE_AGENTS_MAX][13];
        for (int i = 0; i < n; i++) {
            const probe_agent_t *a = probe_agents_at(i);
            for (int b = 0; b < 6; b++) sprintf(last[i] + b * 2, "%02x", a->mac[b]);
        }
        for (int s = 0; s <= post_ticks; s++) {
            if (s) t += tickms;
            probe_agents_rotate_tick(t);
            for (int i = 0; i < n; i++) {
                const probe_agent_t *a = probe_agents_at(i);
                char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", a->mac[b]);
                if (strcmp(hex, last[i]) != 0) {
                    printf("R %u %d %s\n", (unsigned)t, i, hex);
                    strcpy(last[i], hex);
                }
            }
        }
        return 0;
    }
    // Drives EXACTLY the call sequence coexist_task uses in the shipped combined build --
    // phantom_lifecycle + phantom_sync_wifi + probe_agents_rotate_tick, and deliberately NOT
    // probe_agents_lifecycle (that runs only under SIMULACRA_PROBE). --agentrot passes even when
    // the shipped build never rotates, which is how BUG-1 survived; this mode is the guard.
    if (argc > 1 && strcmp(argv[1], "--coexistrot") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nph    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 4;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 60;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 60000;
        srand(seed);
        probe_agents_init(nph, 0);
        phantom_init(nph, 0);
        phantom_sync_wifi(0);
        char last[PROBE_AGENTS_MAX][13];
        memset(last, 0, sizeof last);
        uint32_t t = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            phantom_sync_wifi(t);
            probe_agents_rotate_tick(t);
            for (int i = 0; i < probe_agents_count() && i < PROBE_AGENTS_MAX; i++) {
                const probe_agent_t *a = probe_agents_at(i);
                char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", a->mac[b]);
                if (strcmp(hex, last[i]) != 0) {   // t, agent, mac, persona generation
                    printf("%u %d %s %u\n", (unsigned)t, i, hex, (unsigned)a->persona_gen);
                    strcpy(last[i], hex);
                }
            }
        }
        return 0;
    }
    // Persona/agent count coupling. The Wi-Fi agent count tracks room density via the glide, while
    // the persona registry was fixed at boot -- so in a quiet room the surplus personas advertised a
    // phone on BLE that never probed on Wi-Fi (a single-radio ghost), and in a busy room the surplus
    // agents had no persona and no lifecycle at all. Prints the two counts after each glide step.
    if (argc > 1 && strcmp(argv[1], "--personabind") == 0) {
        unsigned seed  = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n0    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        int      ticks = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 40;
        srand(seed);
        probe_agents_init(n0, 0);
        phantom_init(n0, 0);
        uint32_t t = 0;
        for (int s = 0; s < ticks; s++) {
            t += 60000u;
            // targets swing well above and below the boot count, as room density does
            int target = (s % 8 < 4) ? 2 : PROBE_AGENTS_MAX;
            probe_agents_glide_set_target(target, t);
            probe_agents_glide_tick(t);
            phantom_set_count(probe_agents_count(), t);   // the coupling under test
            phantom_sync_wifi(t);
            probe_agents_rotate_tick(t);
            printf("%u %d %d\n", (unsigned)t, probe_agents_count(), phantom_count());
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--settarget") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n0   = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        srand(seed);
        probe_agents_init(n0, 0);
        printf("%d\n", probe_agents_count());
        for (int i = 4; i < argc; i++) {
            probe_agents_set_target((int)strtol(argv[i], 0, 10), (uint32_t)(i * 1000));
            printf("%d\n", probe_agents_count());
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--wifiobs") == 0) {
        char line[64], cmd[16], mh[16];
        unsigned u;
        while (fgets(line, sizeof line, stdin)) {
            if (sscanf(line, "%15s", cmd) != 1) continue;
            if (strcmp(cmd, "reset") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                wifi_obs_reset(u);
            } else if (strcmp(cmd, "note") == 0 && sscanf(line, "%*s %12s %u", mh, &u) == 2) {
                uint8_t m[6];
                for (int i = 0; i < 6; i++) { char b[3] = { mh[2 * i], mh[2 * i + 1], 0 }; m[i] = (uint8_t)strtoul(b, 0, 16); }
                wifi_obs_note(m, u);
            } else if (strcmp(cmd, "density") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", wifi_obs_density(u));
            } else if (strcmp(cmd, "target") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", wifi_obs_target(u));
            }
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--routecheck") == 0) {
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        uniq_reset();
        uint8_t m[6];
        probe_random_mac(m);
        printf("%d\n", uniq_try(m) ? 1 : 0);   // 0 = routed (recorded), 1 = not routed
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--uniq") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 1000;
        srand(seed);
        uniq_reset();
        for (int i = 0; i < n; i++) {           // one distinct pass of n addresses
            uint8_t a[6];
            do { for (int b = 0; b < 6; b++) a[b] = (uint8_t)(rand() & 0xff); } while (!uniq_try(a));
            for (int b = 0; b < 6; b++) printf("%02x", a[b]);
            printf("\n");
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--uniqreset") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 200;
        for (int half = 0; half < 2; half++) {
            srand(seed);
            uniq_reset();
            for (int i = 0; i < n / 2; i++) {
                uint8_t a[6];
                do { for (int b = 0; b < 6; b++) a[b] = (uint8_t)(rand() & 0xff); } while (!uniq_try(a));
                for (int b = 0; b < 6; b++) printf("%02x", a[b]);
                printf("\n");
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--agents") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nag    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 2000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 2000;
        srand(seed);
        uint32_t t = 0;
        probe_agents_init(nag, t);
        uint32_t last_born[PROBE_AGENTS_MAX];
        for (int i = 0; i < PROBE_AGENTS_MAX; i++) last_born[i] = 0u;
        // A record: one per (re)born agent identity -> arch, born_ms, wildcard(1=wildcard-only life), mac
        for (int i = 0; i < probe_agents_count(); i++) {
            const probe_agent_t *a = probe_agents_at(i);
            printf("A %d %u %d ", (int)a->arch, (unsigned)a->born_ms, (a->ssid_n == 0) ? 1 : 0);
            for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
            printf("\n");
            last_born[i] = a->born_ms;
        }
        for (int s = 0; s < ticks; s++) {
            t += tickms;
            probe_agents_lifecycle(t);
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                if (a->born_ms != last_born[i]) {               // reincarnated this tick
                    printf("A %d %u %d ", (int)a->arch, (unsigned)a->born_ms, (a->ssid_n == 0) ? 1 : 0);
                    for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                    printf("\n");
                    last_born[i] = a->born_ms;
                }
            }
            probe_agent_t *due[PROBE_AGENTS_MAX];
            int nd = probe_agents_due(t, due, PROBE_AGENTS_MAX);
            for (int i = 0; i < nd; i++) {
                uint16_t sq = probe_agent_next_seq(due[i]);
                printf("E %u ", (unsigned)t);
                for (int b = 0; b < 6; b++) printf("%02x", due[i]->mac[b]);
                printf(" %u\n", (unsigned)sq);
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--phantoms") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n      = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 12;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        srand(seed);
        uint32_t t = 0;
        phantom_init(n, t);
        static uint32_t gen_seen[PHANTOM_MAX];
        for (int i = 0; i < n && i < PHANTOM_MAX; i++) gen_seen[i] = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            for (int i = 0; i < phantom_count(); i++) {
                const phantom_t *ph = phantom_at(i);
                if (ph->generation != gen_seen[i]) {         // emit on each new life
                    gen_seen[i] = ph->generation;
                    printf("P %u %d %d %d %04x %u\n", (unsigned)t, i, (int)ph->family,
                           (int)phantom_arch(ph->family), (unsigned)phantom_company(ph->family),
                           (unsigned)ph->generation);
                }
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--wbind") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n      = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 12;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        srand(seed);
        uint32_t t = 0;
        phantom_init(n, t);
        probe_agents_init(n, t);
        phantom_sync_wifi(t);
        static uint32_t gen_seen[PROBE_AGENTS_MAX];
        for (int i = 0; i < n && i < PROBE_AGENTS_MAX; i++) gen_seen[i] = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            phantom_sync_wifi(t);
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                if (a->persona_gen != gen_seen[i]) {
                    gen_seen[i] = a->persona_gen;
                    printf("W %u %d ", (unsigned)t, i);
                    for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                    printf(" %d %u\n", (int)a->arch, (unsigned)a->persona_gen);
                }
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--ssidburst") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int n         = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int bursts    = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 50;
        srand(seed);
        probe_agents_init(n, 0);
        for (int i = 0; i < probe_agents_count(); i++) {
            const probe_agent_t *a = probe_agents_at(i);
            int named = 0; char sb[40];
            for (int b = 0; b < bursts; b++) if (probe_agent_pick_ssid(a, sb, sizeof sb)) named++;
            printf("%d %d %d\n", i, (int)a->ssid_n, named);   // agent, assigned count, # named of `bursts`
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--ssidstable") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int n         = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        srand(seed);
        probe_agents_init(n, 0);
        for (int i = 0; i < n; i++) probe_agent_sync(i, probe_pick_archetype(), 0, 2400000u, 1); // 40min bound
        for (int phase = 0; phase < 2; phase++) {
            if (phase == 1) probe_agents_lifecycle(600000u);  // 10 min: past the 8-15min rotation floor
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                printf("%c %d %d", phase ? 'A' : 'B', i, (int)a->ssid_n);
                for (int j = 0; j < a->ssid_n; j++) printf(" %d", (int)a->ssid_idx[j]);
                printf(" ");
                for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                printf("\n");
            }
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--ssidrender") == 0) {   // --ssidrender <idx> <seed_hex> -> rendered name
        int idx = argc > 2 ? (int)strtoul(argv[2], 0, 10) : 0;
        uint16_t seed = argc > 3 ? (uint16_t)strtoul(argv[3], 0, 16) : 0;
        char out[40];
        uint8_t L = ssid_pool_render(idx, seed, out, sizeof out);
        printf("%d %u %s\n", (int)ssid_pool_suffix_style(idx), (unsigned)L, out);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--ssidpool") == 0) {
        if (argc > 2) {                                  // weighted-pick histogram: --ssidpool <seed> <n>
            srand((unsigned)strtoul(argv[2], 0, 10));
            int n = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 10000;
            for (int i = 0; i < n; i++) printf("%d\n", ssid_pool_pick_weighted());
            return 0;
        }
        printf("%d\n", ssid_pool_count());               // no args: count, then "<len> <name>" per entry
        for (int i = 0; i < ssid_pool_count(); i++) {
            uint8_t L = 0; const char *s = ssid_pool_at(i, &L);
            printf("%d %s\n", (int)L, s);
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--glidenext") == 0) {   // pure step: --glidenext <cur> <target> <step>
        int cur  = argc > 2 ? (int)strtol(argv[2], 0, 10) : 0;
        int tgt  = argc > 3 ? (int)strtol(argv[3], 0, 10) : 0;
        int step = argc > 4 ? (int)strtol(argv[4], 0, 10) : 1;
        printf("%d\n", probe_glide_next(cur, tgt, step));
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--glide") == 0) {   // stdin-driven glide session (see test_glide.py)
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        char line[64], cmd[16]; unsigned a, b;
        while (fgets(line, sizeof line, stdin)) {
            if (sscanf(line, "%15s", cmd) != 1) continue;
            if (strcmp(cmd, "init") == 0 && sscanf(line, "%*s %u", &a) == 1) {
                probe_agents_init((int)a, 0);
                printf("%d\n", probe_agents_count());
            } else if (strcmp(cmd, "target") == 0 && sscanf(line, "%*s %u %u", &a, &b) == 2) {
                probe_agents_glide_set_target((int)b, a);
                printf("%d\n", probe_agents_count());
            } else if (strcmp(cmd, "tick") == 0 && sscanf(line, "%*s %u", &a) == 1) {
                probe_agents_glide_tick(a);
                printf("%d\n", probe_agents_count());
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--surveiloui") == 0) {   // --surveiloui <mac_hex_12>
        uint8_t mac[6] = {0};
        const char *h = argc > 2 ? argv[2] : "";
        for (int i = 0; i < 6 && h[2*i] && h[2*i+1]; i++) {
            char b[3] = { h[2*i], h[2*i+1], 0 }; mac[i] = (uint8_t)strtoul(b, 0, 16);
        }
        uint8_t cls = 255, cat = 255;
        int m = surveil_oui_match(mac, &cls, &cat) ? 1 : 0;
        printf("%d %d %d\n", m, (int)cls, (int)cat);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--surveilssid") == 0) {   // --surveilssid <ascii_ssid>
        const char *s = argc > 2 ? argv[2] : "";
        uint8_t cls = 255, cat = 255;
        int m = surveil_ssid_match((const uint8_t *)s, (uint8_t)strlen(s), &cls, &cat) ? 1 : 0;
        printf("%d %d %d\n", m, (int)cls, (int)cat);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--pick") == 0) {
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        int n = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 1000;
        for (int i = 0; i < n; i++) printf("%d\n", (int)probe_pick_archetype());
        return 0;
    }

    probe_arch_t a = (argc > 1) ? (probe_arch_t)strtoul(argv[1], 0, 10) : ARCH_IPHONE;
    unsigned ch    = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 10) : 6;
    bool band5     = (argc > 3) ? (strtoul(argv[3], 0, 10) != 0) : false;
    const char *ssid = (argc > 4) ? argv[4] : 0;                 // optional directed SSID
    uint8_t ssid_len = ssid ? (uint8_t)strlen(ssid) : 0;

    uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    if (probe_build_request(mac, (uint8_t)ch, a, band5, ssid, ssid_len, f, &n)) {
        fprintf(stderr, "build failed (arch=%u band5=%d)\n", a, band5);
        return 2;
    }
    for (size_t i = 0; i < n; i++) printf("%02x", f[i]);
    printf("\n");
    return 0;
}
