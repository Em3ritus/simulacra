#pragma once
#include "radar_ui.h"
#include "radar_wire.h"
#include "exposure.h"
typedef void (*radar_flush_fn)(int y0, int h, const uint16_t *buf, void *ctx);
typedef struct {                 // Vigil librarian snapshot for the LIBRARY page
    bool     sd_ok;
    uint32_t card_mb;            // capacity, 0 if unknown/absent
    uint16_t lib_count, lib_cap;
    uint32_t offer_age_s;        // UINT32_MAX = never
    uint32_t sync_age_s;         // UINT32_MAX = never
    uint32_t save_age_s;         // UINT32_MAX = never
    uint32_t save_bytes;         // size of last sealed blob
} radar_lib_info_t;
typedef struct { uint8_t sel_preset; bool send_flash; } radar_ctrl_info_t;   // CONTROL page state
typedef struct {                 // CYD system/fleet snapshot for the INFO page
    uint8_t  node_count;         // meshing nodes
    uint16_t sig_ver;            // signature-DB version
    uint16_t sig_count;          // signatures loaded
    uint32_t link_age_s;         // seconds since last status; UINT32_MAX = never
    const char *build;           // firmware/build tag, e.g. "cyd v2 flood"
    uint8_t  page;               // INFO view: 0 = system console, 1 = legend
} radar_sys_info_t;
// Honest at-a-glance protection posture (HOME headline). Priority HUNTED > DARK > EXPOSED > CLOAKED.
typedef enum {
    RADAR_POSTURE_CLOAKED = 0,  // decoys active + a real ambient crowd to blend into
    RADAR_POSTURE_EXPOSED,      // decoys running but ~no ambient crowd -> nothing hides you (empty RF space)
    RADAR_POSTURE_DARK,         // decoys paused / not emitting
    RADAR_POSTURE_HUNTED,       // a CONFIRMED (recurring/persistent) follower is present
} radar_posture_t;
radar_posture_t radar_posture(const radar_wire_status_t *st);   // pure; reads status, no I/O
// Per-node fleet view for the HOME strip: id + a pointer to that node's last status + liveness.
typedef struct { uint8_t id; const radar_wire_status_t *st; bool alive; uint32_t age_s; } radar_node_view_t;
// Banded full-frame render of `view` from `st` (sweep_deg animates the radar). `band` is a
// scratch buffer of w*band_h uint16; flush() pushes each band to the panel.
// `lib` is the librarian snapshot for RADAR_VIEW_LIBRARY; NULL on non-librarian displays.
// `ctrl` is the CONTROL-page state for RADAR_VIEW_CONTROL; NULL on non-Vigil / non-control.
// `expo` is the exposure-session state for RADAR_VIEW_EXPOSURE; NULL on other views/displays.
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node, int sel_threat,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, const radar_sys_info_t *sys, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
