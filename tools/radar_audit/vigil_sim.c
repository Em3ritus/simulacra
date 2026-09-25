/* vigil_sim - the Vigil (CYD) console, running on the desktop.
 *
 * Every pixel here is drawn by the firmware. This file links the REAL radar_render.c,
 * radar_gfx.c, radar_sigil.c, radar_geom.c, radar_ui.c and fleet_status.c and gives them a Win32
 * window instead of an ILI9341 panel. The ONLY invented part is sim_fleet.c, which stands in for
 * the radio.
 *
 * Deliberately keyboard-driven, not click-driven: the CYD's touch hit-testing lives in
 * cyd/main/cyd_main.c, which does not host-compile. Reimplementing it here would create a second
 * implementation that drifts from the real one -- the same failure the keypatch.py/keypatch.js
 * byte-diff test exists to prevent. Keys drive the real radar_ui_* state machine instead.
 *
 * Build: see run.ps1 (MSVC) or the Makefile (gcc/CI).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "sim_fleet.h"
#include "radar_render.h"
#include "radar_ui.h"

/* The Vigil renders PORTRAIT: 240 wide x 320 tall. draw_home fills a 240-wide header and
 * draws its bottom rule at y=298, and render_dump.c passes (band, 320, 240, 320) =
 * band_h=320, w=240, h=320. Getting this backwards clips every view and leaves an unpainted
 * strip down the right-hand side. */
#define SCR_W 240
#define SCR_H 320

static uint16_t g_fb[SCR_W * SCR_H];     /* RGB565, what the panel would hold */
static uint32_t g_bgra[SCR_W * SCR_H];   /* expanded for StretchDIBits */
static int      g_scale  = 3;
static bool     g_paused = false;
static bool     g_quit   = false;
static bool     g_help   = true;

static sim_fleet_t   g_sim;
static radar_ui_t    g_ui;
static radar_view_t  g_view     = RADAR_VIEW_HOME;
static int           g_sel_node = -1, g_sel_threat = -1;
static uint16_t      g_sweep    = 0;
static uint32_t      g_clock    = 0;     /* simulated ms; advances only while unpaused */
static uint8_t       g_info_page = 0;    /* radar_sys_info_t.page: 0 console, 1 legend */

/* ------------------------------------------------------------------ panel emulation */

static void flush_band(int y0, int h, const uint16_t *buf, void *ctx)
{
    (void)ctx;
    for (int r = 0; r < h; r++) {
        int y = y0 + r;
        if (y < 0 || y >= SCR_H) continue;
        memcpy(&g_fb[y * SCR_W], &buf[r * SCR_W], SCR_W * sizeof(uint16_t));
    }
}

static void fb_to_bgra(void)
{
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint16_t c = g_fb[i];
        uint32_t r = (uint32_t)((c >> 11) & 0x1F), g = (uint32_t)((c >> 5) & 0x3F), b = (uint32_t)(c & 0x1F);
        r = (r * 255u + 15u) / 31u;
        g = (g * 255u + 31u) / 63u;
        b = (b * 255u + 15u) / 31u;
        g_bgra[i] = (r << 16) | (g << 8) | b;      /* BI_RGB 32bpp is 0x00RRGGBB */
    }
}

/* ------------------------------------------------------------------ frame */

static void build_frame(void)
{
    static uint16_t band[SCR_W * SCR_H];         /* one full-height band: simplest correct case */

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
                             .link_age_s = 1, .build = "vigil-sim (host)", .page = g_info_page,
                             .req_repeats = 1 };

    radar_render_view(g_view, &g_sim.agg, g_sim.views, g_sim.view_count,
                      g_sel_node, g_sel_threat, &lib, &ctrl, sim_expo_state(&g_sim), &sys, g_sweep,
                      band, SCR_H, SCR_W, SCR_H, flush_band, NULL);
    fb_to_bgra();
}

/* ------------------------------------------------------------------ input */

static void set_view(radar_view_t v)
{
    g_view = v;
    radar_ui_select_view(&g_ui, v, g_clock);
    if (v == RADAR_VIEW_NODE   && g_sel_node   < 0) g_sel_node   = 0;
    if (v == RADAR_VIEW_THREAT && g_sel_threat < 0) g_sel_threat = 0;
    if (v == RADAR_VIEW_EXPOSURE) sim_expo_start(&g_sim, g_clock);
}

static void on_key(WPARAM k)
{
    switch (k) {
        /* views */
        case 'H': set_view(RADAR_VIEW_HOME);    break;
        case 'R': set_view(RADAR_VIEW_RADAR);   break;
        case 'N': set_view(RADAR_VIEW_NODES);   break;
        case 'D': set_view(RADAR_VIEW_NODE);    break;
        case 'T': set_view(RADAR_VIEW_THREAT);  break;
        case 'I': if (g_view == RADAR_VIEW_INFO) g_info_page ^= 1;   /* = tapping the body */
                  else set_view(RADAR_VIEW_INFO); break;
        case 'C': set_view(RADAR_VIEW_CONTROL); break;
        case 'B': set_view(RADAR_VIEW_LIBRARY); break;
        case 'S': set_view(RADAR_VIEW_STATS);   break;
        case 'V': set_view(RADAR_VIEW_DETAIL);  break;
        case 'Q': set_view(RADAR_VIEW_EXPOSURE); break;   /* restarts the scan */

        /* selection within a list view */
        case VK_LEFT:  if (g_sel_node   > 0) g_sel_node--;   radar_ui_note_input(&g_ui, g_clock); break;
        case VK_RIGHT: if (g_sel_node + 1 < g_sim.view_count) g_sel_node++;
                       radar_ui_note_input(&g_ui, g_clock); break;
        case VK_UP:    if (g_sel_threat > 0) g_sel_threat--; radar_ui_note_input(&g_ui, g_clock); break;
        case VK_DOWN:  if (g_sel_threat + 1 < g_sim.agg.threat_count) g_sel_threat++;
                       radar_ui_note_input(&g_ui, g_clock); break;

        /* CONTROL page, through the real ui state machine */
        case VK_TAB:    radar_ctrl_select_next(&g_ui); radar_ui_note_input(&g_ui, g_clock); break;
        case VK_RETURN: radar_ctrl_mark_sent(&g_ui, g_clock);
                        sim_fleet_cycle_preset(&g_sim); break;

        /* fleet events */
        case 'J': sim_fleet_join(&g_sim, g_clock);            break;
        case 'K': sim_fleet_drop(&g_sim, g_clock);            break;
        case 'G': sim_fleet_degrade(&g_sim, g_clock);         break;
        case 'A': sim_fleet_add_threat(&g_sim, g_clock, true); break;
        case 'F': sim_fleet_add_threat(&g_sim, g_clock, false); break;
        case 'E': sim_fleet_escalate(&g_sim, g_clock);        break;
        case 'X': sim_fleet_clear_threats(&g_sim);            break;
        case 'P': sim_fleet_toggle_pause(&g_sim);             break;
        case 'O': sim_fleet_toggle_crowd(&g_sim);             break;

        /* window */
        case VK_SPACE:  g_paused = !g_paused; break;
        case VK_OEM_PLUS:  case VK_ADD:      if (g_scale < 6) g_scale++; break;
        case VK_OEM_MINUS: case VK_SUBTRACT: if (g_scale > 1) g_scale--; break;
        case VK_F1: g_help = !g_help; break;
        case VK_ESCAPE: g_quit = true; break;
        default: break;
    }
}

static const char *HELP =
    "views  H home  R radar  N nodes  D node  T threat  I info (again = legend)  Q exposure  C control  B library  S stats  V detail\n"
    "select arrows            control  TAB cycle preset   ENTER send\n"
    "fleet  J join  K drop  G degrade  A known-threat  F follower  E escalate  X clear  P pause  O crowd\n"
    "window SPACE freeze  +/- scale  F1 help  ESC quit\n";

/* ------------------------------------------------------------------ win32 */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_KEYDOWN: on_key(w); return 0;
        case WM_CLOSE: g_quit = true; return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            BITMAPINFO bi; memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth       = SCR_W;
            bi.bmiHeader.biHeight      = -SCR_H;          /* negative = top-down */
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            RECT rc; GetClientRect(h, &rc);
            SetStretchBltMode(dc, COLORONCOLOR);          /* nearest-neighbour: keep it crisp */
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, SCR_W, SCR_H,
                          g_bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProc(h, m, w, l);
}

/* Headless: render a populated fleet across every view and write PPMs, then exit. Verifies the
 * render path without a window, and doubles as the frame source if a recording is ever wanted. */
static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  ! cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint32_t p = g_bgra[i];
        unsigned char rgb[3] = { (unsigned char)((p >> 16) & 0xFF),
                                 (unsigned char)((p >> 8) & 0xFF),
                                 (unsigned char)(p & 0xFF) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int shoot(void)
{
    static const struct { radar_view_t v; const char *name; } SHOTS[] = {
        { RADAR_VIEW_HOME,    "home"    }, { RADAR_VIEW_RADAR,   "radar"   },
        { RADAR_VIEW_NODES,   "nodes"   }, { RADAR_VIEW_NODE,    "node"    },
        { RADAR_VIEW_THREAT,  "threat"  }, { RADAR_VIEW_INFO,    "info"    },
        { RADAR_VIEW_CONTROL, "control" }, { RADAR_VIEW_STATS,   "stats"   },
        { RADAR_VIEW_EXPOSURE,"exposure"},
    };
    g_clock = 1000;
    sim_fleet_init(&g_sim, g_clock, false);
    radar_ui_reset(&g_ui, g_clock, 0);
    sim_fleet_join(&g_sim, g_clock);
    sim_fleet_join(&g_sim, g_clock);
    sim_fleet_add_threat(&g_sim, g_clock, true);
    sim_fleet_add_threat(&g_sim, g_clock, false);
    sim_fleet_escalate(&g_sim, g_clock);
    for (int t = 0; t < 120; t++) { g_clock += 250; sim_fleet_tick(&g_sim, g_clock); }
    /* run a full exposure session so the EXPOSURE shot shows a RESULT, not an idle page */
    sim_expo_start(&g_sim, g_clock);
    for (int t = 0; t < 60; t++) { g_clock += 250; sim_expo_tick(&g_sim, g_clock); }
    sim_fleet_refresh(&g_sim, g_clock);
    g_sel_node = 0; g_sel_threat = 0; g_sweep = 210;

    printf("fleet: %d nodes, %d threats, preset %u\n",
           g_sim.view_count, g_sim.agg.threat_count, g_sim.agg.preset);
    for (int i = 0; i < (int)(sizeof SHOTS / sizeof SHOTS[0]); i++) {
        char path[128];
        g_view = SHOTS[i].v;
        memset(g_fb, 0, sizeof g_fb);
        build_frame();
        int nonzero = 0;
        for (int p = 0; p < SCR_W * SCR_H; p++) if (g_fb[p]) nonzero++;
        snprintf(path, sizeof path, "shot_%s.ppm", SHOTS[i].name);
        write_ppm(path);
        printf("  %-8s %6d/%d non-black px -> %s\n",
               SHOTS[i].name, nonzero, SCR_W * SCR_H, path);
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool story = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--shot"))  return shoot();
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--story")) story = true;

    printf("vigil_sim - the real Vigil console, host-rendered.\n");
    printf("Only sim_fleet.c is synthetic; every pixel is drawn by the firmware.\n\n%s\n", HELP);
    if (story) printf("--story: running the scripted sequence hands-free.\n\n");

    g_clock = 1000;
    sim_fleet_init(&g_sim, g_clock, story);
    radar_ui_reset(&g_ui, g_clock, 0);

    WNDCLASS wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "vigil_sim";
    if (!RegisterClass(&wc)) { printf("RegisterClass failed\n"); return 1; }

    RECT want = { 0, 0, SCR_W * g_scale, SCR_H * g_scale };
    AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("vigil_sim", "Simulacra - Vigil console (simulated fleet)",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             want.right - want.left, want.bottom - want.top,
                             NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    int last_scale = g_scale;
    char last_event[64] = "";
    DWORD t_prev = GetTickCount();

    while (!g_quit) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = true;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (g_quit) break;

        DWORD t_now = GetTickCount();
        DWORD dt    = t_now - t_prev;
        if (dt < 33) { Sleep(1); continue; }             /* ~30 fps */
        t_prev = t_now;

        if (!g_paused) {
            g_clock += dt;
            g_sweep  = (uint16_t)((g_sweep + 3) % 360);
            sim_fleet_tick(&g_sim, g_clock);
            sim_expo_tick(&g_sim, g_clock);
            radar_ui_on_tick(&g_ui, g_clock, g_sim.agg.threat_count);
            g_view = g_ui.view;                          /* the real state machine may auto-wake */
        }
        sim_fleet_refresh(&g_sim, g_clock);

        if (g_sel_node   >= g_sim.view_count)          g_sel_node   = g_sim.view_count - 1;
        if (g_sel_threat >= g_sim.agg.threat_count)    g_sel_threat = g_sim.agg.threat_count - 1;

        if (g_scale != last_scale) {
            last_scale = g_scale;
            RECT r = { 0, 0, SCR_W * g_scale, SCR_H * g_scale };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
        const char *ev = sim_fleet_last_event(&g_sim);
        if (strcmp(ev, last_event) != 0) {
            strncpy(last_event, ev, sizeof last_event - 1);
            last_event[sizeof last_event - 1] = 0;
            printf("[%6u ms] %s\n", g_clock, last_event);
            fflush(stdout);
        }

        build_frame();
        InvalidateRect(hwnd, NULL, FALSE);
    }
    printf("bye\n");
    return 0;
}
