# run_vrinput.ps1 -- EyeCaptureVrInput.exe (the VR controller hotkey for SteamVR games, src/openvr/vrinput_main.cpp) against
# mock_openvr.dll's IVRInput_010 (MOCK_OPENVR_INPUT_MS = when every action is pressed) and status_sim.exe as the "game" on a
# private status block. CPU only: no GPU, no headset, no SteamVR, never the real status block or the real tray.
#   hold       pressed 1.0 .. 2.6 s, hold 1.0 s -> exactly one toggle sent, actions delivered, manifest + 6 binding files (valid JSON)
#   short      pressed 1.0 .. 1.5 s (shorter than the hold) -> no toggle
#   twice      pressed twice -> two toggles (re-armed after the release)
#   inactive   SteamVR gives no input -> the 10 s warning, no toggle
#   game_ends  the helper exits when its game (status_sim) ends
#   off        vr_hotkey = off -> exits at once, nothing written
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$rel = Join-Path $root 'build\Release'
$exe = Join-Path $rel 'EyeCaptureVrInput.exe'
$mock = Join-Path $rel 'mock_openvr.dll'
$sim = Join-Path $rel 'status_sim.exe'
foreach ($f in $exe, $mock, $sim) { if (-not (Test-Path $f)) { Write-Host "missing $f (build first)" -ForegroundColor Red; exit 2 } }
$work = Join-Path $root 'build\vrinput_test'
$shm = 'Local\EyeCapture-VrInput-Test-Status'
$env:XR_EYE_CAPTURE_VRINPUT_OPENVR_DLL = $mock
$env:XR_EYE_CAPTURE_TEST_SHM = $shm
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
$results = @()

function RunCase($name, $cfg, $pressMs, $simSeconds, $timeoutS, [switch]$inactive) {
    $home_ = Join-Path $work $name
    if (Test-Path $home_) { Remove-Item $home_ -Recurse -Force }
    New-Item -ItemType Directory -Force $home_ | Out-Null
    $env:XR_EYE_CAPTURE_CONFIG = $cfg
    $env:MOCK_OPENVR_INPUT_MS = $pressMs
    $env:MOCK_OPENVR_INPUT_INACTIVE = $(if ($inactive) { '1' } else { $null })
    Write-Host "`n=== $name" -ForegroundColor Cyan
    $g = Start-Process -FilePath $sim -ArgumentList @($shm, "$simSeconds") -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 300
    $p = Start-Process -FilePath $exe -ArgumentList @('--home', "`"$home_`"", '--game-pid', "$($g.Id)") -PassThru
    $done = $p.WaitForExit($timeoutS * 1000)
    if (-not $done) { $p.Kill(); $p.WaitForExit() }
    if (-not $g.HasExited) { $g.Kill() }
    $log = Join-Path $home_ 'logs\eye_capture.log'
    $txt = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    $txt.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
    return @{ home = $home_; log = $txt; exited = $done; code = $p.ExitCode }
}
function Need($ok, $what, [ref]$fails) { if (-not $ok) { $fails.Value += $what; Write-Host "  FAIL $what" -ForegroundColor Red } }
function Record($name, $fails) {
    if ($fails.Count -eq 0) { Write-Host "  PASS $name" -ForegroundColor Green } else { Write-Host "  FAIL $name" -ForegroundColor Red }
    $script:results += [pscustomobject]@{ name = $name; ok = ($fails.Count -eq 0); fails = ($fails -join '; ') }
}
function Want($n) { return (-not $Only) -or ($n -match $Only) }
function Toggles($t) { return ([regex]::Matches($t, 'held 1\.0 s -> toggle sent')).Count }
$base = 'vr_hotkey=thumbsticks;vr_hotkey_hold_s=1.0;log_level=info'

if (Want 'hold') {
    $r = RunCase 'hold' $base '1000-2600' 5 15
    $f = @()
    Need $r.exited 'exits with its game' ([ref]$f)
    Need ($r.code -eq 0) "exit code 0 ($($r.code))" ([ref]$f)
    Need ($r.log -match 'VR_InitInternal2\(Overlay\) OK') 'overlay init' ([ref]$f)
    Need ($r.log -match 'SetActionManifestPath -> error 0') 'manifest accepted' ([ref]$f)
    Need ($r.log -match 'buttons bound per controller: oculus_touch 2/2, knuckles 2/2, vive_controller 2/2, holographic_controller 2/2, hpmotioncontroller 2/2, vive_cosmos_controller 2/2') 'bindings for all 6 controller types' ([ref]$f)
    Need ($r.log -match 'SteamVR delivers the hotkey actions') 'actions delivered' ([ref]$f)
    Need ((Toggles $r.log) -eq 1) "exactly one toggle ($(Toggles $r.log))" ([ref]$f)
    Need ($r.log -match 'the game \(pid \d+\) ended') 'game end seen' ([ref]$f)
    $dir = Join-Path $r.home 'steamvr_input'
    foreach ($j in @('actions.json', 'bindings_oculus_touch.json', 'bindings_knuckles.json', 'bindings_vive_controller.json', 'bindings_holographic_controller.json', 'bindings_hpmotioncontroller.json', 'bindings_vive_cosmos_controller.json')) {
        $p = Join-Path $dir $j
        $okJson = $false
        if (Test-Path $p) { try { $null = Get-Content $p -Raw | ConvertFrom-Json; $okJson = $true } catch { } }
        Need $okJson "$j written, valid JSON" ([ref]$f)
    }
    Record 'hold' $f
}
if (Want 'short') {
    $r = RunCase 'short' $base '1000-1500' 4 15
    $f = @()
    Need ((Toggles $r.log) -eq 0) "no toggle for a 0.5 s press ($(Toggles $r.log))" ([ref]$f)
    Record 'short' $f
}
if (Want 'twice') {
    $r = RunCase 'twice' $base '800-2000,2600-3800' 5 15
    $f = @()
    Need ((Toggles $r.log) -eq 2) "two toggles ($(Toggles $r.log))" ([ref]$f)
    Record 'twice' $f
}
if (Want 'inactive') {
    $r = RunCase 'inactive' $base '1000-2600' 11 20 -inactive
    $f = @()
    Need ($r.log -match 'no hotkey action is active after 10 s') '10 s warning' ([ref]$f)
    Need ((Toggles $r.log) -eq 0) 'no toggle' ([ref]$f)
    Record 'inactive' $f
}
if (Want 'game_ends') {
    $r = RunCase 'game_ends' $base '' 2 10
    $f = @()
    Need $r.exited 'exited within 10 s of start (game alive ~3.5 s)' ([ref]$f)
    Need ($r.log -match 'the game \(pid \d+\) ended') 'game end logged' ([ref]$f)
    Record 'game_ends' $f
}
if (Want 'off') {
    $r = RunCase 'off' 'vr_hotkey=off' '' 2 10
    $f = @()
    Need ($r.exited -and $r.code -eq 0) 'exits at once with code 0' ([ref]$f)
    Need ($r.log -match 'vr_hotkey = off -- nothing to do') 'off logged' ([ref]$f)
    Need (-not (Test-Path (Join-Path $r.home 'steamvr_input'))) 'no files written' ([ref]$f)
    Record 'off' $f
}

foreach ($n in 'XR_EYE_CAPTURE_VRINPUT_OPENVR_DLL', 'XR_EYE_CAPTURE_TEST_SHM', 'XR_EYE_CAPTURE_REQUIRE_TRAY', 'XR_EYE_CAPTURE_CONFIG',
                'MOCK_OPENVR_INPUT_MS', 'MOCK_OPENVR_INPUT_INACTIVE') { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
$bad = @($results | Where-Object { -not $_.ok })
Write-Host ''
$results | Format-Table -AutoSize | Out-String | Write-Host
if ($bad.Count) { Write-Host "run_vrinput: $($bad.Count) of $($results.Count) cases FAILED" -ForegroundColor Red; exit 1 }
Write-Host "run_vrinput: all $($results.Count) cases passed" -ForegroundColor Green
exit 0
