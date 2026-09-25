#!/bin/sh
# Build the Vigil console for the browser. This is what CI runs (.github/workflows/flasher.yml);
# build_web.ps1 is the Windows equivalent. KEEP THE SOURCE LISTS IN SYNC -- this repo has been
# bitten before by a file added to one build recipe and not the other.
#
# Needs emcc on PATH (CI uses the emscripten/emsdk container).
# Output lands in web/ beside vigil.html, and is gitignored.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
rad="$root/components/simulacra_radar"
cyd="$root/cyd/main"
out="$root/web"

emcc -O2 -I "$here/host_stubs" -I "$rad" -I "$cyd" \
  "$here/vigil_web.c" "$here/sim_fleet.c" \
  "$rad/radar_render.c" "$rad/radar_gfx.c" "$rad/radar_sigil.c" "$rad/radar_geom.c" \
  "$rad/radar_ui.c" "$rad/exposure.c" "$cyd/fleet_status.c" \
  -o "$out/vigil_web.js" \
  -s "EXPORTED_FUNCTIONS=['_wv_init','_wv_tick','_wv_width','_wv_height','_wv_view','_wv_set_view','_wv_back','_wv_select_node','_wv_select_threat','_wv_info_flip','_wv_expo_restart','_wv_expo_running','_wv_ctrl_next','_wv_ctrl_send','_wv_event','_wv_node_count','_wv_threat_count','_wv_posture','_wv_last_event','_malloc','_free']" \
  -s "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPU8']" \
  -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s "EXPORT_NAME='VigilModule'"

ls -l "$out/vigil_web.js" "$out/vigil_web.wasm"
