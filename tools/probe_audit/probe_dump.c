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
