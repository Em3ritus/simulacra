<#
.SYNOPSIS  Build the three BAKED Simulacra firmwares and merge each into one flashable image for the
           web-flasher (web/firmware/*.bin). -Serve hosts web/ on localhost for a browser flash test.
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
$skill = "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1"

# target, chip, IDF export, build dir (relative to repo root), output name
$targets = @(
    @{ t="c5";  chip="esp32c5"; idf="v5.5"; bdir="build";     out="decoy-c5.bin" },
    @{ t="c6";  chip="esp32c6"; idf="v5.4"; bdir="build";     out="decoy-c6.bin" },
    @{ t="cyd"; chip="esp32";   idf="v5.4"; bdir="cyd\build"; out="cyd.bin" }
)

foreach ($x in $targets) {
    Write-Host "=== building baked $($x.t) ===" -ForegroundColor Cyan
    $bdir = Join-Path $root $x.bdir
    $outAbs = Join-Path $fw $x.out
    $exportPs1 = "$env:USERPROFILE\esp\$($x.idf)\esp-idf\export.ps1"
    # Build AND merge for this target in ONE fresh powershell so its IDF version/env is fully isolated
    # -- calling the skill for successive targets in the parent process leaks 5.5's env into 5.4's build
    # (esp_idf_monitor missing). Child-side vars are backtick-escaped; parent vars interpolate.
    $child = @"
& '$skill' -Target $($x.t) -Fleet -Do build
if (`$LASTEXITCODE -ne 0) { exit 1 }
& '$exportPs1' *> `$null
Set-Location '$bdir'
python -m esptool --chip $($x.chip) merge_bin -o '$outAbs' '@flash_args'
exit `$LASTEXITCODE
"@
    powershell -NoProfile -Command $child
    if ($LASTEXITCODE -ne 0) { throw "build/merge failed for $($x.t)" }
    if (-not (Test-Path $outAbs)) { throw "no output for $($x.t)" }
    Write-Host "  -> $outAbs ($([math]::Round((Get-Item $outAbs).Length/1KB)) KB)" -ForegroundColor Green
}
Write-Host "done. run  web\build_flasher.ps1 -Serve  to flash a board in the browser." -ForegroundColor Cyan
