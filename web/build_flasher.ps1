<#
.SYNOPSIS  Build the three PROVISIONED Simulacra firmwares and merge each into one flashable image
           for the web-flasher (web/firmware/*.bin). -Serve hosts web/ on localhost for a browser
           flash test.

.DESCRIPTION
  Mirrors .github/workflows/flasher.yml exactly, including the build flags, so a local flash test
  exercises the images users actually get. If the two ever disagree, this file is the one to fix.

  Builds into build_ci/ rather than build/, so running this never disturbs the bench build or its
  sdkconfig. IDF -D flags are sticky, and a one-off regime change leaking into the next bench flash
  is a mixed-key fleet that will not decrypt.

  NOTE for the CYD: CI compiles a placeholder secret from sim_ctrl_sk.h.example, but a local build
  uses YOUR real sim_ctrl_sk.h. The browser overwrites it at flash time either way, so a board keyed
  through the page is unaffected. The images left in web/firmware/ do carry it until patched, so do
  not hand those files to anyone.
#>
[CmdletBinding()] param([switch]$Serve)
# "Continue" (not "Stop"): the IDF export + idf.py write progress to stderr, which "Stop" would treat
# as fatal. Correctness is gated on explicit $LASTEXITCODE / Test-Path checks below instead.
$ErrorActionPreference = "Continue"
$web = $PSScriptRoot
$root = Resolve-Path (Join-Path $web "..")
$fw = Join-Path $web "firmware"

if ($Serve) {
    Set-Location $web
    Write-Host "serving $web at http://localhost:8000  (Chrome/Edge, plug in a board, Connect & Flash)" -ForegroundColor Cyan
    & "C:/Program Files/Python312/python.exe" -m http.server 8000
    return
}

New-Item -ItemType Directory -Force $fw | Out-Null

# Exactly the flags .github/workflows/flasher.yml builds with. Keep the two in step: the whole point
# of a local flash test is that it exercises what users receive.
$FLAGS = "-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1"

# target, chip, IDF version, project dir (relative to repo root), output name
$targets = @(
    @{ t="c5";  chip="esp32c5"; idf="v5.5"; proj=".";   out="decoy-c5.bin" },
    @{ t="c6";  chip="esp32c6"; idf="v5.4"; proj=".";   out="decoy-c6.bin" },
    @{ t="cyd"; chip="esp32";   idf="v5.4"; proj="cyd"; out="cyd.bin" }
)

foreach ($x in $targets) {
    Write-Host "=== building provisioned $($x.t) ===" -ForegroundColor Cyan
    $projAbs = Join-Path $root $x.proj
    $outAbs = Join-Path $fw $x.out
    $exportPs1 = "$env:USERPROFILE\esp\$($x.idf)\esp-idf\export.ps1"
    # Build AND merge for this target in ONE fresh powershell so its IDF version/env is fully isolated
    # -- running successive targets in the parent process leaks 5.5's env into 5.4's build
    # (esp_idf_monitor missing). Child-side vars are backtick-escaped; parent vars interpolate.
    # Per-target build dir AND an explicit SDKCONFIG inside it. sdkconfig normally lives at the
    # PROJECT root, so a plain `-B build_ci set-target` would still rewrite the bench's sdkconfig and
    # leave the next bench flash on the wrong chip or regime. C5 and C6 also share the root project,
    # so one shared build_ci would carry a stale target from whichever ran last.
    $bdir = "build_ci_$($x.chip)"
    $child = @"
& '$exportPs1' *> `$null
Set-Location '$projAbs'
idf.py -B $bdir -DIDF_TARGET=$($x.chip) -DSDKCONFIG=$bdir/sdkconfig $FLAGS build
if (`$LASTEXITCODE -ne 0) { exit 1 }
Set-Location (Join-Path '$projAbs' '$bdir')
python -m esptool --chip $($x.chip) merge_bin -o '$outAbs' '@flash_args'
exit `$LASTEXITCODE
"@
    powershell -NoProfile -Command $child
    if ($LASTEXITCODE -ne 0) { throw "build/merge failed for $($x.t)" }
    if (-not (Test-Path $outAbs)) { throw "no output for $($x.t)" }
    Write-Host "  -> $outAbs ($([math]::Round((Get-Item $outAbs).Length/1KB)) KB)" -ForegroundColor Green
}
Write-Host "done. run  web\build_flasher.ps1 -Serve  to flash a board in the browser." -ForegroundColor Cyan
