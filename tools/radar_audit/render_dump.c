// Render smoke/text harness for the CYD dashboard views. Compiles radar_render.c ALONE by stubbing
// every gfx/geom/sigil primitive: text calls print "TXT <x> <y> <str>", the rest are no-ops. This
// lets host tests assert WHAT TEXT a view draws (e.g. the shade-form breakdown on STATS) without a
// pixel framebuffer. One full-height band so each draw_* runs exactly once.
//
//   render_dump <view> [restless wandering bound active_devices roster target threat_count]
//   view: 0 HOME 1 RADAR 2 DETAIL 3 STATS 4 LIBRARY 5 CONTROL 6 INFO 8 NODE (via --node) 9 THREAT (via --threat)
//   INFO 2-page console via --info <page nodes sigver sigcount linkage libcount libcap cardmb sdok decoys target pop uptime>
//   CONTROL live-vs-pending via --control <sel live flash clear_armed>  (live: 0-4 preset, 5 CUSTOM, 254 MIXED, 255 none)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "radar_render.h"
#include "radar_sigil.h"
#include "threat_sig.h"

// --- stubbed primitives (no framebuffer; capture text only) ---
void radar_gfx_clear(radar_gfx_t *g, uint16_t c) { (void)g; (void)c; }
void radar_gfx_pixel(radar_gfx_t *g, int x, int y, uint16_t c) { (void)g; (void)x; (void)y; (void)c; }
void radar_gfx_hline(radar_gfx_t *g, int a, int b, int y, uint16_t c) { (void)g;(void)a;(void)b;(void)y;(void)c; }
void radar_gfx_vline(radar_gfx_t *g, int x, int a, int b, uint16_t c) { (void)g;(void)x;(void)a;(void)b;(void)c; }
void radar_gfx_line(radar_gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c) { (void)g;(void)x0;(void)y0;(void)x1;(void)y1;(void)c; }
void radar_gfx_fill_rect(radar_gfx_t *g, int x, int y, int w, int h, uint16_t c) { (void)g;(void)x;(void)y;(void)w;(void)h;(void)c; }
void radar_gfx_circle(radar_gfx_t *g, int cx, int cy, int r, uint16_t c) { (void)g;(void)cx;(void)cy;(void)r;(void)c; }
void radar_gfx_text(radar_gfx_t *g, int x, int y, const char *s, uint16_t c) { (void)g; (void)c; printf("TXT %d %d %s\n", x, y, s); }
uint16_t radar_rssi_to_radius(int8_t rssi, uint16_t lo, uint16_t hi) { (void)rssi; (void)hi; return lo; }
uint16_t radar_hash_to_angle(uint32_t h) { (void)h; return 0; }
void radar_polar_to_xy(int cx, int cy, uint16_t r, uint16_t a, int *x, int *y) { (void)r; (void)a; *x = cx; *y = cy; }
void radar_sigil_draw(radar_gfx_t *g, sigil_id_t id, int cx, int cy, int r, uint16_t c) { (void)g;(void)id;(void)cx;(void)cy;(void)r;(void)c; }

static void flush_noop(int y0, int h, const uint16_t *buf, void *ctx) { (void)y0; (void)h; (void)buf; (void)ctx; }

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--expo") == 0) {
        int step = argc > 2 ? atoi(argv[2]) : 0;                    // 0 idle 1 baseline 2 watch 3 result
        exposure_t e; expo_reset(&e);
        if (step >= 1) expo_start(&e, 0);
        if (step >= 2) expo_tick(&e, EXPO_BASELINE_MS + 1);         // -> WATCH
        if (step >= 3) {
            unsigned fp = argc > 3 ? (unsigned)strtoul(argv[3], 0, 16) : 0xbb;
            int probes  = argc > 4 ? atoi(argv[4]) : 5;
            int ambig   = argc > 5 ? atoi(argv[5]) : 0;
            if (!ambig) {
                for (int i = 0; i < probes; i++) expo_probe(&e, fp, 0, 0, EXPO_BASELINE_MS + 100);
                if (argc > 6 && argv[6][0]) {                       // csv named SSIDs
                    char buf[256]; strncpy(buf, argv[6], sizeof buf - 1); buf[sizeof buf - 1] = 0;
                    for (char *t = strtok(buf, ","); t; t = strtok(0, ","))
                        expo_probe(&e, fp, t, (uint8_t)strlen(t), EXPO_BASELINE_MS + 100);
                }
            }
            expo_tick(&e, EXPO_BASELINE_MS + EXPO_WATCH_MS + 2);    // -> RESULT
        }
        static uint16_t eband[240 * 320];
        radar_render_view(RADAR_VIEW_EXPOSURE, 0, 0, 0, -1, -1, 0, 0, &e, NULL, 0, eband, 320, 240, 320, flush_noop, 0);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--node") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 0; a++;
        int id    = argc > a ? atoi(argv[a]) : 0; a++;
        int alive = argc > a ? atoi(argv[a]) : 1; a++;
        unsigned age = argc > a ? (unsigned)strtoul(argv[a], 0, 10) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        if (argc > a) st.active_devices = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.active_target  = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.roster_size    = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.form_restless  = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.form_wandering = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.form_bound     = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.pop_ewma       = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.battery_mv     = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.battery_pct    = (uint8_t)atoi(argv[a]); else st.battery_pct = 0xFF; a++;
        if (argc > a) st.epoch          = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.probes_sent    = (uint32_t)strtoul(argv[a], 0, 10); a++;
        if (argc > a) st.flags          = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.uptime_s       = (uint32_t)strtoul(argv[a], 0, 10); a++;
        int threats = argc > a ? atoi(argv[a]) : 0; a++;
        int ncam    = argc > a ? atoi(argv[a]) : 0; a++;
        st.threat_count = (uint8_t)threats;
        for (int i = 0; i < threats && i < RADAR_MAX_THREATS; i++) st.threats[i].best_rssi = -55;
        for (int i = 0; i < ncam && i < threats && i < RADAR_MAX_THREATS; i++) {
            st.threats[i].kind = DETECT_KIND_KNOWN; st.threats[i].category = SIG_CAT_CAMERA;
            st.threats[i].class_id = SIG_CLASS_FLOCK;
        }
        static uint16_t nband[240 * 320];
        radar_node_view_t nodes[1] = { { (uint8_t)id, &st, alive != 0, age } };
        radar_render_view(RADAR_VIEW_NODE, &st, nodes, 1, sel, -1, 0, 0, NULL, NULL, 0,
                          nband, 320, 240, 320, flush_noop, 0);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--threat") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 0; a++;
        int count = argc > a ? atoi(argv[a]) : 1; a++;
        int kind  = argc > a ? atoi(argv[a]) : 1; a++;
        int cls   = argc > a ? atoi(argv[a]) : 0; a++;
        int cat   = argc > a ? atoi(argv[a]) : 0; a++;
        int conf  = argc > a ? atoi(argv[a]) : 0; a++;
        int vendor= argc > a ? (int)strtol(argv[a], 0, 0) : 0; a++;
        int rssi  = argc > a ? atoi(argv[a]) : 0; a++;
        int epochs= argc > a ? atoi(argv[a]) : 0; a++;
        int first = argc > a ? atoi(argv[a]) : 0; a++;
        int last  = argc > a ? atoi(argv[a]) : 0; a++;
        int sess  = argc > a ? atoi(argv[a]) : 0; a++;
        int places= argc > a ? atoi(argv[a]) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        if (count > RADAR_MAX_THREATS) count = RADAR_MAX_THREATS;
        st.threat_count = (uint8_t)count;
        int t = (sel >= 0 && sel < count) ? sel : 0;
        st.threats[t].hash = 0xABCD1234u;
        st.threats[t].kind = (uint8_t)kind; st.threats[t].class_id = (uint8_t)cls;
        st.threats[t].category = (uint8_t)cat; st.threats[t].confidence = (uint8_t)conf;
        st.threats[t].vendor = (uint16_t)vendor; st.threats[t].best_rssi = (int8_t)rssi;
        st.threats[t].epochs = (uint8_t)epochs;
        st.threats[t].first_epoch = (uint16_t)first; st.threats[t].last_epoch = (uint16_t)last;
        st.threats[t].sessions_seen = (uint8_t)sess; st.threats[t].places_seen = (uint8_t)places;
        static uint16_t tband[240 * 320];
        radar_render_view(RADAR_VIEW_THREAT, &st, 0, 0, -1, sel, 0, 0, NULL, NULL, 0,
                          tband, 320, 240, 320, flush_noop, 0);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--info") == 0) {
        int a = 2;
        int page   = argc > a ? atoi(argv[a]) : 0; a++;
        int nodes  = argc > a ? atoi(argv[a]) : 0; a++;
        int sigver = argc > a ? atoi(argv[a]) : 0; a++;
        int sigcnt = argc > a ? atoi(argv[a]) : 0; a++;
        unsigned long linkage = argc > a ? strtoul(argv[a], 0, 10) : 0; a++;
        int libcount = argc > a ? atoi(argv[a]) : 0; a++;
        int libcap   = argc > a ? atoi(argv[a]) : 0; a++;
        int cardmb   = argc > a ? atoi(argv[a]) : 0; a++;
        int sdok     = argc > a ? atoi(argv[a]) : 0; a++;
        int decoys   = argc > a ? atoi(argv[a]) : 0; a++;
        int target   = argc > a ? atoi(argv[a]) : 0; a++;
        int pop      = argc > a ? atoi(argv[a]) : 0; a++;
        unsigned long uptime = argc > a ? strtoul(argv[a], 0, 10) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        st.active_devices = (uint16_t)decoys; st.active_target = (uint8_t)target;
        st.pop_ewma = (uint16_t)pop; st.uptime_s = (uint32_t)uptime;
        radar_lib_info_t lib; memset(&lib, 0, sizeof lib);
        lib.sd_ok = sdok != 0; lib.card_mb = (uint32_t)cardmb;
        lib.lib_count = (uint16_t)libcount; lib.lib_cap = (uint16_t)libcap;
        radar_sys_info_t sys; memset(&sys, 0, sizeof sys);
        sys.node_count = (uint8_t)nodes; sys.sig_ver = (uint16_t)sigver;
        sys.sig_count = (uint16_t)sigcnt; sys.link_age_s = (uint32_t)linkage;
        sys.build = "cydtest"; sys.page = (uint8_t)page;
        static uint16_t iband[240 * 320];
        radar_render_view(RADAR_VIEW_INFO, &st, 0, 0, -1, -1, &lib, 0, 0, &sys, 0,
                          iband, 320, 240, 320, flush_noop, 0);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--control") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 2; a++;
        int live  = argc > a ? atoi(argv[a]) : 0xFF; a++;
        int flash = argc > a ? atoi(argv[a]) : 0; a++;
        int carm  = argc > a ? atoi(argv[a]) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        radar_ctrl_info_t ctrl; memset(&ctrl, 0, sizeof ctrl);
        ctrl.sel_preset = (uint8_t)sel; ctrl.live_preset = (uint8_t)live; ctrl.send_flash = flash != 0;
        ctrl.clear_armed = carm != 0;
        static uint16_t cband[240 * 320];
        radar_render_view(RADAR_VIEW_CONTROL, &st, 0, 0, -1, -1, 0, &ctrl, NULL, NULL, 0,
                          cband, 320, 240, 320, flush_noop, 0);
        return 0;
    }

    int view = argc > 1 ? atoi(argv[1]) : RADAR_VIEW_STATS;
    radar_wire_status_t st; memset(&st, 0, sizeof st);
    if (argc > 2) st.form_restless  = (uint8_t)atoi(argv[2]);
    if (argc > 3) st.form_wandering = (uint8_t)atoi(argv[3]);
    if (argc > 4) st.form_bound     = (uint8_t)atoi(argv[4]);
    if (argc > 5) st.active_devices = (uint8_t)atoi(argv[5]);
    if (argc > 6) st.roster_size    = (uint8_t)atoi(argv[6]);
    if (argc > 7) st.active_target  = (uint8_t)atoi(argv[7]);
    if (argc > 8) st.threat_count   = (uint8_t)atoi(argv[8]);
    if (argc > 9) st.pop_ewma       = (uint16_t)atoi(argv[9]);
    // arg 10: threat escalation for ALL threats -- 0 NEW(1/1), 1 RECURRING(places 3), 2 PERSISTENT(sessions 3)
    int esc = argc > 10 ? atoi(argv[10]) : 0;
    for (int i = 0; i < st.threat_count && i < RADAR_MAX_THREATS; i++) {
        st.threats[i].sessions_seen = (esc >= 2) ? 3 : 1;
        st.threats[i].places_seen   = (esc >= 1) ? 3 : 1;
    }
    if (argc > 11) st.flags         = (uint8_t)atoi(argv[11]);
    if (argc > 12) st.uptime_s      = (uint32_t)strtoul(argv[12], 0, 10);
    // arg 13: how many threats are surveillance; arg 14: kind (0=Flock/CAMERA, 1=Axon/BODYCAM).
    int ncam = argc > 13 ? atoi(argv[13]) : 0;
    int surv_kind = argc > 14 ? atoi(argv[14]) : 0;
    uint8_t sv_cat = surv_kind ? SIG_CAT_BODYCAM : SIG_CAT_CAMERA;
    uint8_t sv_cls = surv_kind ? SIG_CLASS_AXON  : SIG_CLASS_FLOCK;
    for (int i = 0; i < ncam && i < st.threat_count && i < RADAR_MAX_THREATS; i++) {
        st.threats[i].kind     = DETECT_KIND_KNOWN;
        st.threats[i].category = sv_cat;
        st.threats[i].class_id = sv_cls;
        st.threats[i].best_rssi = -55;
    }

    radar_lib_info_t lib; memset(&lib, 0, sizeof lib);
    radar_ctrl_info_t ctrl; memset(&ctrl, 0, sizeof ctrl);
    radar_node_view_t nodes[1] = { { 0, &st, true, 0 } };

    static uint16_t band[240 * 320];
    // One full-height band: each draw_* runs once, so text is emitted a single time.
    radar_render_view((radar_view_t)view, &st, nodes, 1, -1, -1, &lib, &ctrl, NULL, NULL, 0,
                      band, 320, 240, 320, flush_noop, NULL);
    return 0;
}
