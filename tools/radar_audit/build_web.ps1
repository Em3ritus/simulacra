# Build vigil_web.{js,wasm} -- the Vigil console for a browser.
#
# Needs emscripten. One-time setup:
#   git clone --depth 1 https://github.com/emscripten-core/emsdk C:\Users\<you>\emsdk
#   cd C:\Users\<you>\emsdk; python emsdk.py install latest; python emsdk.py activate latest
# (emsdk.bat did not resolve under a nested cmd /c here -- the python entry point is reliable.)
#
#   pwsh build_web.ps1                 # uses $env:EMSDK or the default path below
#   pwsh build_web.ps1 -Emsdk D:\emsdk
#
# Output is vigil_web.js + vigil_web.wasm, written into web/ beside vigil.html (and gitignored).
# Serve web/ over HTTP to try it; opening the .html off the filesystem will not work, because
# the browser blocks the wasm fetch under file://.
#
# build_web.sh is the POSIX equivalent and is what CI runs. KEEP THE SOURCE LISTS IN SYNC.
param([string]$Emsdk = $(if ($env:EMSDK) { $env:EMSDK } else { "$env:USERPROFILE\emsdk" }))

$envScript = Join-Path $Emsdk 'emsdk_env.ps1'
if (-not (Test-Path $envScript)) { Write-Error "emsdk not found at $Emsdk (see header for setup)"; exit 1 }
. $envScript | Out-Null

$tool = $PSScriptRoot
$root = Join-Path $tool '..\..'
$rad  = Join-Path $root 'components\simulacra_radar'
$cyd  = Join-Path $root 'cyd\main'
$out  = Join-Path $root 'web'

# Keep in sync with run.ps1 -Sim's source list: same core, different front-end. The only difference
# is vigil_web.c in place of vigil_sim.c.
$exports = "['_wv_init','_wv_tick','_wv_width','_wv_height','_wv_view','_wv_set_view','_wv_back'," +
           "'_wv_select_node','_wv_select_threat','_wv_info_flip','_wv_expo_restart','_wv_expo_running'," +
           "'_wv_ctrl_next','_wv_ctrl_send','_wv_event','_wv_node_count','_wv_threat_count'," +
           "'_wv_posture','_wv_last_event','_malloc','_free']"

Push-Location $tool
emcc -O2 -I host_stubs -I $rad -I $cyd `
    vigil_web.c sim_fleet.c `
    (Join-Path $rad 'radar_render.c') (Join-Path $rad 'radar_gfx.c') `
    (Join-Path $rad 'radar_sigil.c')  (Join-Path $rad 'radar_geom.c') `
    (Join-Path $rad 'radar_ui.c')     (Join-Path $rad 'exposure.c') `
    (Join-Path $cyd 'fleet_status.c') `
    -o (Join-Path $out 'vigil_web.js') `
    -s EXPORTED_FUNCTIONS="$exports" `
    -s "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPU8']" `
    -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s "EXPORT_NAME='VigilModule'"
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { Write-Error "emcc failed ($rc)"; exit 1 }

Get-ChildItem (Join-Path $out 'vigil_web.js'), (Join-Path $out 'vigil_web.wasm') |
    Select-Object Name, Length | Format-Table -AutoSize
Write-Host "built vigil_web into web/ -- serve that directory over HTTP to try it"
