param(
    # Build vigil_sim.exe (the desktop Vigil console) instead of running the test suite. A separate
    # switch on purpose: it is a demo/presentation tool, not part of the verification path, and it
    # is Windows-only (Win32 window), so it must never gate the tests.
    [switch]$Sim
)
$tool = $PSScriptRoot
$root = Join-Path $tool "..\.."
$rad  = Join-Path $root "components\simulacra_radar"
$cyd  = Join-Path $root "cyd\main"

if ($Sim) {
    # Links the REAL renderer, rasteriser, sigils, geometry, view state machine and fleet
    # aggregation. sim_fleet.c is the only synthetic piece -- it stands in for the radio.
    # _USE_MATH_DEFINES: radar_geom.c needs M_PI, which MSVC does not define by default. No other
    # harness compiles radar_geom.c, so this only bites here.
    cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES /FIportab.h `
       /I (Join-Path $tool "host_stubs") /I $rad /I $cyd `
       (Join-Path $tool "vigil_sim.c")  (Join-Path $tool "sim_fleet.c") `
       (Join-Path $rad "radar_render.c") (Join-Path $rad "radar_gfx.c") `
       (Join-Path $rad "radar_sigil.c")  (Join-Path $rad "radar_geom.c") `
       (Join-Path $rad "radar_ui.c")     (Join-Path $rad "exposure.c") `
       (Join-Path $cyd "fleet_status.c") `
       /Fe:(Join-Path $tool "vigil_sim.exe") /link user32.lib gdi32.lib | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Error "vigil_sim build failed"; exit 1 }
    Write-Host "built vigil_sim.exe  --  bare for the window, --shot for PPMs, --story for hands-free"
    exit 0
}

cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $rad `
   (Join-Path $tool "ui_dump.c") (Join-Path $rad "radar_ui.c") /Fe:(Join-Path $tool "ui_dump.exe") | Out-Null
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $cyd /I $rad `
   (Join-Path $tool "fleet_dump.c") (Join-Path $cyd "fleet_status.c") /Fe:(Join-Path $tool "fleet_dump.exe") | Out-Null
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /I (Join-Path $tool "host_stubs") /I $rad `
   (Join-Path $tool "render_dump.c") (Join-Path $rad "radar_render.c") (Join-Path $rad "exposure.c") /Fe:(Join-Path $tool "render_dump.exe") | Out-Null
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $rad `
   (Join-Path $tool "expo_dump.c") (Join-Path $rad "exposure.c") /Fe:(Join-Path $tool "expo_dump.exe") | Out-Null
python -m unittest discover -s (Join-Path $tool "tests") -v
