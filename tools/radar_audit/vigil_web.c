/* vigil_web - the Vigil console in a browser, via emscripten.
 *
 * Same core as vigil_sim.c, different front-end. Everything that decides what you see is the real
 * firmware: radar_render.c, radar_gfx.c, radar_sigil.c, radar_geom.c, radar_ui.c and
 * cyd/main/fleet_status.c all compile here unmodified. sim_fleet.c stands in for the radio and is
 * the only invented part.
 *
 * NO FIRMWARE FILE IS MODIFIED BY THIS. They are read-only inputs to the wasm build.
 *
 * Hit-testing lives in JavaScript (see vigil_web.html), NOT here, and deliberately does not share
 * an implementation with cyd_main.c. That is a considered trade: the firmware's hit geometry is
 * tangled with ESP-NOW sends, enrollment calls and a dozen device-local statics that cannot compile
 * to wasm, and for a demo a tap zone being a few pixels off is cosmetic rather than incorrect.
 * (Contrast keypatch.js, where two implementations of one format MUST agree or boards brick --
 * that one has a byte-diff test for exactly that reason.)
 *
 * Build: see build_web.ps1.
 */
#include <emscripten/emscripten.h>
#include <string.h>
#include <stdio.h>
#include "sim_fleet.h"
#include "radar_render.h"
#include "radar_ui.h"

/* The Vigil renders PORTRAIT: 240 wide x 320 tall. draw_home fills a 240-wide header and
 * draws its bottom rule at y=298, and render_dump.c passes (band, 320, 240, 320) =
 * band_h=320, w=240, h=320. Getting this backwards clips every view and leaves an unpainted
 * strip down the right-hand side. */
#define SCR_W 240
#define SCR_H 320

static uint16_t      g_fb[SCR_W * SCR_H];        /* RGB565, what the panel would hold */
static uint8_t       g_rgba[SCR_W * SCR_H * 4];  /* what canvas ImageData wants */
static sim_fleet_t   g_sim;
static radar_ui_t    g_ui;
static radar_view_t  g_view     = RADAR_VIEW_HOME;
static int           g_sel_node = -1, g_sel_threat = -1;
static uint16_t      g_sweep    = 0;
static uint32_t      g_clock    = 1000;
static uint8_t       g_info_page = 0;   /* radar_sys_info_t.page: 0 console, 1 legend */

static void flush_band(int y0, int h, const uint16_t *buf, void *ctx)
{
    (void)ctx;
    for (int r = 0; r < h; r++) {
        int y = y0 + r;
        if (y < 0 || y >= SCR_H) continue;
        memcpy(&g_fb[y * SCR_W], &buf[r * SCR_W], SCR_W * sizeof(uint16_t));
    }
}

static void expand_rgba(void)
{
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint16_t c = g_fb[i];
        uint32_t r = (uint32_t)((c >> 11) & 0x1F), g = (uint32_t)((c >> 5) & 0x3F), b = (uint32_t)(c & 0x1F);
        g_rgba[i * 4 + 0] = (uint8_t)((r * 255u + 15u) / 31u);
        g_rgba[i * 4 + 1] = (uint8_t)((g * 255u + 31u) / 63u);
        g_rgba[i * 4 + 2] = (uint8_t)((b * 255u + 15u) / 31u);
        g_rgba[i * 4 + 3] = 255;
    }
}

static void draw(void)
{
    static uint16_t band[SCR_W * SCR_H];

    radar_lib_info_t lib = { .sd_ok = true, .card_mb = 7580, .lib_count = 63, .lib_cap = 128,
                             .offer_age_s = 4, .sync_age_s = 19, .save_age_s = 122,
                             .save_bytes = 8144 };
    radar_ctrl_info_t ctrl = { .sel_preset = g_ui.sel_preset,
                               .send_flash = (g_ui.send_flash_ms != 0 &&
                                              g_clock - g_ui.send_flash_ms < RADAR_CTRL_FLASH_MS),
                               .live_preset = g_sim.agg.preset,
                               .clear_armed = false, .turbo_armed = false,
                               .pair_shown = true, .pair_state = RADAR_PAIR_IDLE,
                               .pair_secs = 0, .pair_rotate_armed = false, .pair_fp = NULL };
    radar_sys_info_t sys = { .node_count = (uint8_t)g_sim.view_count, .sig_ver = 7, .sig_count = 41,
                             .link_age_s = 1, .build = "vigil-web (demo)", .page = g_info_page,
                             .req_repeats = 1 };

    radar_render_view(g_view, &g_sim.agg, g_sim.views, g_sim.view_count,
                      g_sel_node, g_sel_threat, &lib, &ctrl, sim_expo_state(&g_sim), &sys, g_sweep,
                      band, SCR_H, SCR_W, SCR_H, flush_band, NULL);
    expand_rgba();
}

/* ------------------------------------------------------------------ exported API */

EMSCRIPTEN_KEEPALIVE void wv_init(int story)
{
    g_clock = 1000;
    sim_fleet_init(&g_sim, g_clock, story ? true : false);
    radar_ui_reset(&g_ui, g_clock, 0);
    g_view = RADAR_VIEW_HOME;
    g_sel_node = -1; g_sel_threat = -1; g_sweep = 0;
    sim_fleet_refresh(&g_sim, g_clock);
    draw();
}

/* Advance by dt ms, re-render, and hand back the RGBA buffer for canvas. */
EMSCRIPTEN_KEEPALIVE uint8_t *wv_tick(int dt_ms)
{
    if (dt_ms < 0)   dt_ms = 0;
    if (dt_ms > 250) dt_ms = 250;             /* a backgrounded tab must not fast-forward the sim */
    g_clock += (uint32_t)dt_ms;
    g_sweep  = (uint16_t)((g_sweep + 3) % 360);
    sim_fleet_tick(&g_sim, g_clock);
    sim_expo_tick(&g_sim, g_clock);
    radar_ui_on_tick(&g_ui, g_clock, g_sim.agg.threat_count);
    g_view = g_ui.view;                        /* the real state machine can auto-wake to HUNTERS */
    sim_fleet_refresh(&g_sim, g_clock);
    if (g_sel_node   >= g_sim.view_count)       g_sel_node   = g_sim.view_count - 1;
    if (g_sel_threat >= g_sim.agg.threat_count) g_sel_threat = g_sim.agg.threat_count - 1;
    draw();
    return g_rgba;
}

EMSCRIPTEN_KEEPALIVE int wv_width(void)  { return SCR_W; }
EMSCRIPTEN_KEEPALIVE int wv_height(void) { return SCR_H; }
EMSCRIPTEN_KEEPALIVE int wv_view(void)   { return (int)g_view; }

EMSCRIPTEN_KEEPALIVE void wv_set_view(int v)
{
    if (v < 0 || v >= RADAR_VIEW_COUNT) return;
    g_view = (radar_view_t)v;
    radar_ui_select_view(&g_ui, g_view, g_clock);
    if (g_view == RADAR_VIEW_NODE   && g_sel_node   < 0) g_sel_node   = 0;
    if (g_view == RADAR_VIEW_THREAT && g_sel_threat < 0) g_sel_threat = 0;
    if (g_view == RADAR_VIEW_EXPOSURE) sim_expo_start(&g_sim, g_clock);
}

/* "< BACK" strip: the real state machine's off-home input path. */
EMSCRIPTEN_KEEPALIVE void wv_back(void)
{
    radar_ui_on_input(&g_ui, g_clock);
    g_view = g_ui.view;
}

EMSCRIPTEN_KEEPALIVE void wv_select_node(int i)
{
    if (i >= 0 && i < g_sim.view_count) { g_sel_node = i; wv_set_view(RADAR_VIEW_NODE); }
}
EMSCRIPTEN_KEEPALIVE void wv_select_threat(int i)
{
    if (i >= 0 && i < g_sim.agg.threat_count) { g_sel_threat = i; wv_set_view(RADAR_VIEW_THREAT); }
}
/* INFO body tap flips console <-> legend, exactly as cyd_main.c does (s_info_page ^= 1). */
EMSCRIPTEN_KEEPALIVE void wv_info_flip(void) { g_info_page ^= 1; }

/* A running session ignores taps -- on the device you are busy toggling your phone. */
EMSCRIPTEN_KEEPALIVE void wv_expo_restart(void)
{
    if (!sim_expo_running(&g_sim)) sim_expo_start(&g_sim, g_clock);
}
EMSCRIPTEN_KEEPALIVE int wv_expo_running(void) { return sim_expo_running(&g_sim) ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE void wv_ctrl_next(void)
{
    radar_ctrl_select_next(&g_ui);
    radar_ui_note_input(&g_ui, g_clock);
}
EMSCRIPTEN_KEEPALIVE void wv_ctrl_send(void)
{
    radar_ctrl_mark_sent(&g_ui, g_clock);
    sim_fleet_cycle_preset(&g_sim);
}

/* Fleet events, so a visitor can make something happen rather than wait for it. */
EMSCRIPTEN_KEEPALIVE void wv_event(int code)
{
    switch (code) {
        case 0: sim_fleet_join(&g_sim, g_clock);             break;
        case 1: sim_fleet_drop(&g_sim, g_clock);             break;
        case 2: sim_fleet_degrade(&g_sim, g_clock);          break;
        case 3: sim_fleet_add_threat(&g_sim, g_clock, true);  break;
        case 4: sim_fleet_add_threat(&g_sim, g_clock, false); break;
        case 5: sim_fleet_escalate(&g_sim, g_clock);         break;
        case 6: sim_fleet_clear_threats(&g_sim);             break;
        case 7: sim_fleet_toggle_pause(&g_sim);              break;
        case 8: sim_fleet_toggle_crowd(&g_sim);              break;
        default: break;
    }
}

/* Small readouts for the page's caption strip. */
EMSCRIPTEN_KEEPALIVE int  wv_node_count(void)   { return g_sim.view_count; }
EMSCRIPTEN_KEEPALIVE int  wv_threat_count(void) { return g_sim.agg.threat_count; }
EMSCRIPTEN_KEEPALIVE int  wv_posture(void)      { return (int)radar_posture(&g_sim.agg); }
EMSCRIPTEN_KEEPALIVE const char *wv_last_event(void) { return sim_fleet_last_event(&g_sim); }
