#include "radar_render.h"
#include "radar_gfx.h"
#include "radar_geom.h"
#include "radar_theme.h"
#include "radar_sigil.h"
#include "sig_class_name.h"
#include "threat_escalation.h"
#include <stdio.h>
#include <string.h>
// Legacy view colors now alias the necromancer theme so every sub-view (radar/followers/stats/
// library/control) reskins from one place and stays cohesive with HOME. See radar_theme.h.
#define COL_BG    COL_VOID
#define COL_FG    COL_BONE
#define COL_DIM   COL_ASH
#define COL_RING  COL_EDGE
#define COL_OK    COL_CHANNEL
#define COL_WARN  COL_HUNTER
#define COL_SWEEP RGB565(0x3A,0x22,0x55)   // dim arcane — the radar sweep's trailing energy
#define RCX 120
#define RCY 120
#define RR 100

#define POSTURE_MIN_CROWD 2       // at/below this many ambient devices there's no crowd to hide in

radar_posture_t radar_posture(const radar_wire_status_t *st){
    for(uint8_t i=0;i<st->threat_count;i++)                       // a CONFIRMED follower is the top fact
        if(threat_escalation_level(st->threats[i].sessions_seen, st->threats[i].places_seen) != ESCALATION_NEW)
            return RADAR_POSTURE_HUNTED;
    if((st->flags & 0x1) || st->active_devices == 0) return RADAR_POSTURE_DARK;   // paused / not emitting
    if(st->pop_ewma <= POSTURE_MIN_CROWD) return RADAR_POSTURE_EXPOSED;           // empty RF space
    return RADAR_POSTURE_CLOAKED;
}
static const char *posture_label(radar_posture_t p){
    return p==RADAR_POSTURE_HUNTED?"HUNTED":p==RADAR_POSTURE_EXPOSED?"EXPOSED":
           p==RADAR_POSTURE_DARK?"DARK":"CLOAKED";
}
static uint16_t posture_color(radar_posture_t p){
    return p==RADAR_POSTURE_HUNTED?COL_HUNTER:p==RADAR_POSTURE_EXPOSED?COL_WARD:
           p==RADAR_POSTURE_DARK?COL_ASH:COL_CHANNEL;
}

static uint16_t threat_color(uint8_t ep){ return ep>=5?COL_HUNTER:(ep>=2?COL_WARD:COL_ARCANE); }
static uint16_t escalation_color(detect_escalation_t e){
    return e==ESCALATION_PERSISTENT ? COL_HUNTER   // red   — a confirmed follower
         : e==ESCALATION_RECURRING  ? COL_WARD     // amber — seen across sessions
                                    : COL_ARCANE;  // arcane — NEW this session
}

// Shared themed header for the text-data sub-views (STATS/DETAIL/LIBRARY/INFO). Matches HOME's top
// bar (crypt fill + edge hairline) plus CONTROL's BACK affordance -- any tap on these views returns
// HOME (cyd_main.c radar_ui_on_input), so "< BACK" is truthful. Title is right-aligned (8px/glyph).
static void draw_header(radar_gfx_t *g, const char *title){
    radar_gfx_fill_rect(g, 0, 0, 240, 26, COL_CRYPT);
    radar_gfx_hline(g, 0, 239, 26, COL_EDGE);
    radar_gfx_text(g, 8, 9, "< BACK", COL_ARCANE);
    int tx = 232 - (int)strlen(title) * 8;                 // right-align, 8px pad from the edge
    radar_gfx_text(g, tx, 9, title, COL_BONE);
}
static void draw_radar(radar_gfx_t *g, const radar_wire_status_t *st, uint16_t sweep){
    radar_gfx_circle(g,RCX,RCY,RR,COL_RING); radar_gfx_circle(g,RCX,RCY,RR*2/3,COL_RING);
    radar_gfx_circle(g,RCX,RCY,RR/3,COL_RING);
    radar_gfx_hline(g,RCX-RR,RCX+RR,RCY,COL_RING); radar_gfx_vline(g,RCX,RCY-RR,RCY+RR,COL_RING);
    int sx,sy; radar_polar_to_xy(RCX,RCY,RR,sweep,&sx,&sy); radar_gfx_line(g,RCX,RCY,sx,sy,COL_SWEEP);
    for(uint8_t i=0;i<st->threat_count;i++){
        uint16_t rr=radar_rssi_to_radius(st->threats[i].best_rssi,RR/4,RR);
        uint16_t an=radar_hash_to_angle(st->threats[i].hash);
        int x,y; radar_polar_to_xy(RCX,RCY,rr,an,&x,&y);
        detect_escalation_t e = threat_escalation_level(st->threats[i].sessions_seen, st->threats[i].places_seen);
        radar_gfx_fill_rect(g,x-2,y-2,5,5,escalation_color(e)); }
    char b[24];
    if(st->threat_count==0) radar_gfx_text(g,84,250,"CLEAR",COL_OK);
    else { snprintf(b,sizeof b,"! %u FOLLOWERS",(unsigned)st->threat_count); radar_gfx_text(g,40,250,b,COL_WARN); }
    char l[40]; snprintf(l,sizeof l,"decoys %u  up %lus",(unsigned)st->active_devices,(unsigned long)st->uptime_s);
    radar_gfx_text(g,10,296,l,COL_DIM);
}
static void draw_detail(radar_gfx_t *g, const radar_wire_status_t *st){
    draw_header(g,"FOLLOWERS");
    // Partition threats: behavioral followers vs. SIG_CAT_CAMERA surveillance infrastructure.
    int followers=0, cameras=0, flagged=0;
    for(uint8_t i=0;i<st->threat_count;i++){
        if(st->threats[i].category==SIG_CAT_CAMERA){ cameras++; continue; }
        followers++;
        if(threat_escalation_level(st->threats[i].sessions_seen,st->threats[i].places_seen)!=ESCALATION_NEW) flagged++;
    }
    if(followers==0) radar_gfx_text(g,16,40,"none detected",COL_ASH);
    else { char s[32]; snprintf(s,sizeof s,"%u seen  %d flagged",(unsigned)followers,flagged);
           radar_gfx_text(g,8,34,s,COL_ASH); }
    radar_gfx_hline(g,8,231,50,COL_EDGE);
    // one clean row per follower: [escalation dot] name   recurrence ........ rssi
    int y=58;
    for(uint8_t i=0;i<st->threat_count && y<250;i++){
        if(st->threats[i].category==SIG_CAT_CAMERA) continue;              // cameras render below
        detect_escalation_t e = threat_escalation_level(st->threats[i].sessions_seen,st->threats[i].places_seen);
        uint16_t c = escalation_color(e);
        radar_gfx_fill_rect(g,8,y+2,6,6,c);                        // escalation dot (arcane/amber/red)
        char name[16];
        if(st->threats[i].kind==DETECT_KIND_KNOWN) snprintf(name,sizeof name,"%s",sig_class_name(st->threats[i].class_id));
        else snprintf(name,sizeof name,"%08lx",(unsigned long)st->threats[i].hash);
        radar_gfx_text(g,20,y,name,c);
        char rec[12];
        if(e==ESCALATION_NEW) snprintf(rec,sizeof rec,"new");
        else snprintf(rec,sizeof rec,"%up %us",(unsigned)st->threats[i].places_seen,(unsigned)st->threats[i].sessions_seen);
        radar_gfx_text(g,112,y,rec,COL_ASH);
        char r[12]; snprintf(r,sizeof r,"%ddB",(int)st->threats[i].best_rssi);
        radar_gfx_text(g,224-(int)strlen(r)*8,y,r,COL_ASH);        // rssi, right-aligned
        y+=18;
    }
    // SURVEILLANCE section: fixed infrastructure (Flock/Raven) -- present, not "following".
    if(cameras>0){
        y+=6;
        radar_gfx_text(g,8,y,"SURVEILLANCE",COL_HUNTER); y+=20;
        for(uint8_t i=0;i<st->threat_count && y<310;i++){
            if(st->threats[i].category!=SIG_CAT_CAMERA) continue;
            radar_gfx_fill_rect(g,8,y+2,6,6,COL_HUNTER);
            radar_gfx_text(g,20,y,sig_class_name(st->threats[i].class_id),COL_HUNTER);
            char r[12]; snprintf(r,sizeof r,"%ddB",(int)st->threats[i].best_rssi);
            radar_gfx_text(g,224-(int)strlen(r)*8,y,r,COL_ASH);
            y+=18;
        }
    }
}
// --- shared data-page primitives: section headers + aligned label/value rows ------------------
static void fmt_uptime(char *out, size_t n, uint32_t s){    // 47143s -> "13h 5m"; keeps the panel legible
    if (s < 3600)       snprintf(out, n, "%um", (unsigned)(s / 60));
    else if (s < 86400) snprintf(out, n, "%uh %um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    else                snprintf(out, n, "%ud %uh", (unsigned)(s / 86400), (unsigned)((s % 86400) / 3600));
}
static void row_kv(radar_gfx_t *g, int y, const char *label, const char *val){
    radar_gfx_text(g, 16, y, label, COL_ASH);                        // dim label, indented
    radar_gfx_text(g, 224 - (int)strlen(val) * 8, y, val, COL_BONE); // bright value, right-aligned column
}
static void row_section(radar_gfx_t *g, int y, const char *title){
    radar_gfx_text(g, 8, y, title, COL_ARCANE);                      // accent section header
}

static void draw_stats(radar_gfx_t *g, const radar_wire_status_t *st){
    draw_header(g,"DECOYS");
    char v[24]; int y = 36;
    row_section(g, y, "DECOY CROWD"); y += 18;
    // Fleet-wide TOTAL active decoys (summed across nodes). Roster capacity is a per-node constant, so
    // summing it is meaningless -- show the projected count vs the target instead.
    snprintf(v,sizeof v,"%u",(unsigned)st->active_devices); row_kv(g,y,"projecting",v); y+=16;
    snprintf(v,sizeof v,"%u",(unsigned)st->active_target); row_kv(g,y,"target",v); y+=16;
    // Shade-form breakdown (Milestone-A showcase): BLE privacy-address split rpa/nrpa/static.
    snprintf(v,sizeof v,"%u / %u / %u",(unsigned)st->form_restless,(unsigned)st->form_wandering,(unsigned)st->form_bound);
    row_kv(g,y,"rpa/nrpa/static",v); y+=22;
    row_section(g, y, "ENVIRONMENT"); y += 18;
    snprintf(v,sizeof v,"%u",(unsigned)st->pop_ewma); row_kv(g,y,"real crowd",v); y+=16;
    snprintf(v,sizeof v,"%lu",(unsigned long)st->total_obs); row_kv(g,y,"observed",v); y+=22;
    row_section(g, y, "SYSTEM"); y += 18;
    snprintf(v,sizeof v,"%u",(unsigned)st->epoch); row_kv(g,y,"epoch",v); y+=16;
    snprintf(v,sizeof v,"%lu",(unsigned long)st->probes_sent); row_kv(g,y,"probes",v); y+=16;
    row_kv(g,y,"churn",(st->flags&0x1)?"PAUSED":"running"); y+=16;
    fmt_uptime(v,sizeof v,st->uptime_s); row_kv(g,y,"uptime",v);
}
static void fmt_age_v(char *out, size_t n, uint32_t age_s){   // value-only age for a row_kv cell
    if (age_s == UINT32_MAX) snprintf(out, n, "never");
    else                     snprintf(out, n, "%lus ago", (unsigned long)age_s);
}
static void draw_library(radar_gfx_t *g, const radar_lib_info_t *lib){
    draw_header(g,"LIBRARY");
    if (!lib) { radar_gfx_text(g,16,40,"not a librarian",COL_ASH); return; }
    char v[24]; int y = 36;
    row_section(g, y, "STORAGE"); y += 18;
    if (lib->sd_ok) { snprintf(v,sizeof v,"OK %luMB",(unsigned long)lib->card_mb); row_kv(g,y,"card",v); }
    else            row_kv(g,y,"card","ABSENT");
    y += 16;
    snprintf(v,sizeof v,"%u / %u",(unsigned)lib->lib_count,(unsigned)lib->lib_cap); row_kv(g,y,"shapes",v); y+=22;
    row_section(g, y, "SYNC"); y += 18;
    fmt_age_v(v,sizeof v,lib->offer_age_s); row_kv(g,y,"offer rx",v); y+=16;
    fmt_age_v(v,sizeof v,lib->sync_age_s);  row_kv(g,y,"sync tx",v);  y+=16;
    if (lib->save_age_s == UINT32_MAX) row_kv(g,y,"last save","never");
    else { snprintf(v,sizeof v,"%lus (%luB)",(unsigned long)lib->save_age_s,(unsigned long)lib->save_bytes);
           row_kv(g,y,"last save",v); }
}
static const char *CTRL_LABELS[5] = { "PAUSE", "STEALTH", "NORMAL", "DENSE", "MAX" };
static void draw_control(radar_gfx_t *g, const radar_ctrl_info_t *c){
    radar_gfx_text(g, 8, 6, "< BACK", COL_ARCANE);       // top strip taps home
    radar_gfx_text(g, 152, 6, "CONTROL", COL_ASH);
    uint8_t sel = c ? c->sel_preset : 2;
    radar_gfx_text(g, 20, 120, "<", COL_DIM);
    radar_gfx_text(g, 200, 120, ">", COL_DIM);
    char box[16]; snprintf(box, sizeof box, "[ %s ]", CTRL_LABELS[sel % 5]);
    radar_gfx_text(g, 70, 120, box, COL_FG);
    radar_gfx_fill_rect(g, 60, 210, 120, 40, COL_RING);      // SEND button
    radar_gfx_text(g, 96, 224, c && c->send_flash ? "SENT" : "SEND",
                   c && c->send_flash ? COL_OK : COL_FG);
    radar_gfx_text(g, 30, 296, "broadcast to all decoys", COL_DIM);
}
// ---- necromancer HOME: fleet strip + sigil grid + ticker (theme palette) ----
static void draw_home(radar_gfx_t *g, const radar_wire_status_t *st, const radar_node_view_t *nodes, int nc){
    radar_gfx_clear(g, COL_VOID);
    radar_gfx_fill_rect(g, 0, 0, 240, 26, COL_CRYPT);
    radar_gfx_hline(g, 0, 239, 26, COL_EDGE);
    radar_sigil_draw(g, SIGIL_CIRCLE, 12, 13, 7, COL_ARCANE);
    radar_gfx_text(g, 26, 9, "SIMULACRA", COL_BONE);
    // Protection posture: a dim "STATUS" label + the honest one-word verdict (coloured), right-aligned
    // in the top bar (8px/glyph) so a new user reads it as "the system's current status".
    radar_posture_t p = radar_posture(st);
    const char *pl = posture_label(p);
    int px = 232 - (int)strlen(pl) * 8;
    radar_gfx_text(g, px, 9, pl, posture_color(p));
    radar_gfx_text(g, px - 8 - 6 * 8, 9, "STATUS", COL_ASH);   // "STATUS" = 6 glyphs, 8px gap before the word
    // Surveillance-presence count (Flock/Raven, category CAMERA): a compact "!N" left of the wordmark's
    // status area when >=1 is seen. Distinct from HUNTED (a follower) -- this is fixed infra nearby.
    int nsurv=0;
    for(uint8_t i=0;i<st->threat_count;i++) if(st->threats[i].category==SIG_CAT_CAMERA) nsurv++;
    if(nsurv>0){ char sb[8]; snprintf(sb,sizeof sb,"!%d",nsurv); radar_gfx_text(g, 100, 9, sb, COL_HUNTER); }
    int cols = nc < 1 ? 0 : (nc > 3 ? 3 : nc);
    for(int i=0;i<cols;i++){
        int x=i*80, y=30;
        radar_gfx_fill_rect(g, x+2, y, 76, 70, COL_CRYPT);
        // Per-node health from the status flags: bit3 LOW BATT (fuel gauge), bit2 DEGRADED (probe TX
        // wedged). Both read amber, distinct from CHANNEL (healthy) and SILENT (gone). Battery wins.
        bool alive = nodes[i].alive;
        bool low_batt = alive && (nodes[i].st->flags & 0x08);
        bool degraded = alive && (nodes[i].st->flags & 0x04);
        uint16_t sc = !alive ? COL_ASH : (low_batt || degraded) ? COL_WARD : COL_CHANNEL;
        const char *health = !alive ? "SILENT" : low_batt ? "LOW BATT" : degraded ? "DEGRADED" : "CHANNEL";
        char b[12]; snprintf(b,sizeof b,"N%u",(unsigned)nodes[i].id); radar_gfx_text(g, x+8, y+6, b, COL_BONE);
        radar_gfx_fill_rect(g, x+68, y+8, 4, 4, sc);
        snprintf(b,sizeof b,"%u",(unsigned)(alive?nodes[i].st->active_devices:0)); radar_gfx_text(g, x+8, y+24, b, COL_BONE);
        // Battery readout: SoC%% if a fuel gauge provides it, else cell voltage; amber when low. Blank on USB.
        if (alive && nodes[i].st->battery_mv) {
            uint16_t mv = nodes[i].st->battery_mv; uint8_t pc = nodes[i].st->battery_pct;
            if (pc != 0xFF) snprintf(b,sizeof b,"%u%% %u.%01uV",(unsigned)pc,(unsigned)(mv/1000),(unsigned)((mv%1000)/100));
            else            snprintf(b,sizeof b,"%u.%02uV",(unsigned)(mv/1000),(unsigned)((mv%1000)/10));
            radar_gfx_text(g, x+8, y+40, b, low_batt ? COL_WARD : COL_ASH);
        }
        radar_gfx_text(g, x+8, y+54, health, sc);
    }
    static const sigil_id_t sig[7]={SIGIL_CIRCLE,SIGIL_HUNTER,SIGIL_LIVING,SIGIL_RITE,SIGIL_WARD,SIGIL_GRIMOIRE,SIGIL_CIRCLE};
    static const char *lbl[7]={"RADAR","FOLLOWERS","DECOYS","CONTROL","LIBRARY","INFO","EXPOSURE"};
    for(int i=0;i<7;i++){                                          // 4 rows @ 48px to fit the 7th tile
        int cx=(i%2)*120, cy=104+(i/2)*48;
        radar_gfx_fill_rect(g, cx+1, cy+1, 118, 46, COL_CRYPT);
        radar_sigil_draw(g, sig[i], cx+18, cy+23, 10, COL_ARCANE);
        radar_gfx_text(g, cx+36, cy+19, lbl[i], COL_BONE);
    }
    radar_gfx_hline(g, 0, 239, 298, COL_EDGE);
    radar_gfx_text(g, 6, 304, "TAP AN ICON TO OPEN", COL_ASH);
}
static void draw_info(radar_gfx_t *g, const radar_wire_status_t *st){
    draw_header(g, "INFO");
    char v[24]; int y = 36;
    row_section(g, y, "SYSTEM"); y += 18;
    snprintf(v,sizeof v,"%u",(unsigned)st->epoch); row_kv(g,y,"epoch",v); y+=16;
    fmt_uptime(v,sizeof v,st->uptime_s);           row_kv(g,y,"uptime",v); y+=16;
    row_kv(g,y,"firmware","cyd v1");
}
static void draw_exposure(radar_gfx_t *g, const exposure_t *e){
    draw_header(g, "EXPOSURE");
    if(!e || e->state == EXPO_IDLE){
        radar_gfx_text(g, 24, 120, "TAP TO SCAN THE AIR", COL_BONE);
        radar_gfx_text(g, 24, 150, "see what your phone leaks", COL_ASH);
        return;
    }
    if(e->state == EXPO_BASELINE){
        radar_gfx_text(g, 24, 130, "listening...", COL_ARCANE);
        return;
    }
    if(e->state == EXPO_WATCH){
        radar_gfx_text(g, 16, 120, "TOGGLE YOUR PHONE'S", COL_BONE);
        radar_gfx_text(g, 16, 144, "WI-FI OFF, THEN ON", COL_BONE);
        radar_gfx_text(g, 16, 176, "watching for the burst", COL_ASH);
        return;
    }
    // RESULT
    if(expo_ambiguous(e)){
        radar_gfx_text(g, 24, 120, "no clear signal", COL_WARD);
        radar_gfx_text(g, 24, 150, "TAP TO TRY AGAIN", COL_BONE);
        return;
    }
    char l[40]; snprintf(l,sizeof l,"your phone: %d probes", expo_winner_probes(e));
    radar_gfx_text(g, 12, 36, l, COL_HUNTER);
    const char *ss[EXPO_MAX_SSIDS]; int n = expo_winner_ssids(e, ss, EXPO_MAX_SSIDS);
    if(n == 0){
        radar_gfx_text(g, 12, 70, "named no networks (good)", COL_CHANNEL);
        radar_gfx_text(g, 12, 94, "but still announced itself", COL_ASH);
    } else {
        radar_gfx_text(g, 12, 66, "it announced it knows:", COL_ASH);
        int y = 90;
        for(int i=0;i<n && y<300;i++){ radar_gfx_text(g, 20, y, ss[i], COL_BONE); y+=18; }
    }
}
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, uint16_t sweep, uint16_t *band, int band_h, int w, int h,
                       radar_flush_fn flush, void *ctx){
    for(int y0=0;y0<h;y0+=band_h){ radar_gfx_t g={ .buf=band, .w=w, .y0=y0, .h=band_h };
        radar_gfx_clear(&g,COL_BG);
        if(view==RADAR_VIEW_HOME) draw_home(&g,st,nodes,node_count);
        else if(view==RADAR_VIEW_DETAIL) draw_detail(&g,st);
        else if(view==RADAR_VIEW_STATS) draw_stats(&g,st);
        else if(view==RADAR_VIEW_LIBRARY) draw_library(&g,lib);
        else if(view==RADAR_VIEW_CONTROL) draw_control(&g,ctrl);
        else if(view==RADAR_VIEW_INFO) draw_info(&g,st);
        else if(view==RADAR_VIEW_EXPOSURE) draw_exposure(&g,expo);
        else draw_radar(&g,st,sweep);
        flush(y0, band_h, band, ctx); }
}
