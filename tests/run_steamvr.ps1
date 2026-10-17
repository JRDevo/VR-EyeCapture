<#
    run_steamvr.ps1 -- the VR EyeCapture SteamVR proxy end to end, without SteamVR or a headset (GPU: D3D11 + the
    hardware HEVC encoder, like run_e2e.ps1).
      powershell -ExecutionPolicy Bypass -File tests\run_steamvr.ps1 [-Only <regex>]
    Setup (all under build\steamvr_e2e, nothing outside it is written): game\openvr_api.dll = the built proxy,
    game\openvr_api_real.dll = tests/mock_openvr.dll (stands in for Valve's DLL), game\eye_capture_proxy.ini = the sidecar
    (home = build\steamvr_e2e\home, with a minimal config.ini), APPDATA = an empty scratch folder (so no %APPDATA% config
    is read), XR_EYE_CAPTURE_TEST_HOME_KEY = a test HKCU key (the real HKCU\Software\OpenXR-EyeCapture is never read),
    XR_EYE_CAPTURE_REQUIRE_TRAY=0 and XR_EYE_CAPTURE_TRAY_IPC=0 (a running tray neither arms nor converts anything),
    XR_EYE_CAPTURE_AUTORECORD=4. The fake game is build\Release\openvr_e2e.exe (tests/openvr_e2e.cpp); it fails on its own
    when a Submit result differs from the mock's, when the mock did not receive exactly the game's Submits / WaitGetPoses,
    when an unhooked compositor slot was called or the accessor jump stubs lost an argument.
    Cases:
      layout (two | wide | array | msaa | typeless | fp16) x interface (cpp | fntable), raw eyes (symmetric_fov 0): a valid
        MP4 (verify_mp4.ps1: size 3840x1080, 90 fps, st3d, audio, grey steps = gamma, left half = left eye: bounds / slices /
        eye order), log lines for the load banner, the patch (vtable / FnTable), the layout and the record start.
      two / cpp with the right eye submitted first.
      symmetric_fov 1 with --pose wait / --pose submit (the mock then has no WaitGetPoses pose) / IVRSystem_019 only: the
        frame is the symmetric size openvr_e2e computes from the mock's GetProjectionRaw (E2E_SYM) -- wrong fov signs or
        no pose give the raw 3840x1080 instead; --pose none: raw size and the log says "none (raw eyes)".
      submit error (MOCK_OPENVR_SUBMIT_ERROR 101): the game gets 101 on every Submit, the recording is still made, logged.
      unknown IVRCompositor_099: forwarded untouched, logged "does not know", no MP4, the game runs.
      steamvr_enabled = 0: pure forwarder (no MP4, logged).
      dormant (require_tray, no tray): no MP4, logged "dormant" -- skipped while an EyeCaptureTray runs.
      no sidecar, no registry value: pure forwarder, NO log file anywhere; registry fallback (test key Home value): logged
        "home from registry".
      openvr_api_real.dll missing: the game gets VR init error 100 (no crash), the log says NOT LOADED.
    SteamVR quick mode (EyeCaptureMirror.exe with XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = the mock, whose FnTable:IVRCompositor_022
    hands out pattern mirror textures and whose IVRSystem_022 sends VREvent_Quit after MOCK_OPENVR_QUIT_AFTER_MS):
      two textures / one texture for both eyes: Background init + interface versions logged, 2880x1600 SBS at 90 fps, a valid
        MP4 (verify_mp4: pattern, left half = left eye), VREvent_Quit ends it, 2 mirror gets = 2 releases;
      projection = vr180 refused (logged, recorded flat); steamvr_mode = per_game: exits at once; no SteamVR server: waits,
        the tray's quit event ends it; another live owner of a private status block (status_sim): idle, no MP4; the scene
        app is a game with the proxy loaded (openvr_e2e, MOCK_OPENVR_SCENE_PID): idle, no MP4 (per game wins);
      tray: a scratch tray (steamvr_mode = quick, EYECAP_TRAY_TEST_VRSERVER = a renamed status_sim) starts the mirror while
        that "vrserver" runs and stops it (quit event, MP4 finalized) when it exits.
    Skips (exit 0 with a message) while amtrucks / eurotrucks2 / obs64 / Crisis VRigade 2 / vrserver / vrcompositor runs.
#>
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$exe = Join-Path $bin 'openvr_e2e.exe'
foreach ($f in @($exe, (Join-Path $bin 'openvr_api.dll'), (Join-Path $bin 'mock_openvr.dll'), (Join-Path $bin 'EyeCaptureMirror.exe'), (Join-Path $bin 'status_sim.exe'))) { if (-not (Test-Path $f)) { throw "build first: $f" } }
$busy = Get-Process amtrucks, eurotrucks2, obs64, 'Crisis VRigade 2', vrserver, vrcompositor -ErrorAction SilentlyContinue
if ($busy) {
    Write-Host ("run_steamvr: skipped -- {0} is running (GPU tests would disturb it)" -f (($busy | ForEach-Object Name) -join ', ')) -ForegroundColor Yellow
    exit 0
}

$b = Join-Path $root 'build\steamvr_e2e'
if (Test-Path $b) { Remove-Item -Recurse -Force $b }
$game = Join-Path $b 'game'; $home2 = Join-Path $b 'home'; $appdata = Join-Path $b 'appdata'; $outRoot = Join-Path $b 'out'
New-Item -ItemType Directory -Force $game, $home2, $appdata, $outRoot | Out-Null
Copy-Item (Join-Path $bin 'openvr_api.dll') (Join-Path $game 'openvr_api.dll')
Copy-Item (Join-Path $bin 'mock_openvr.dll') (Join-Path $game 'openvr_api_real.dll')
$sidecar = Join-Path $game 'eye_capture_proxy.ini'
[System.IO.File]::WriteAllText($sidecar, "home = $home2`r`ngame = openvr_e2e test`r`n", (New-Object System.Text.UTF8Encoding $false))
[System.IO.File]::WriteAllText((Join-Path $home2 'config.ini'), "[capture]`r`naudio_enabled = 1`r`n[debug]`r`nlog_level = info`r`n", (New-Object System.Text.UTF8Encoding $false))
$log = Join-Path $home2 'logs\eye_capture.log'
$testKey = "Software\OpenXR-EyeCapture-Test\SteamVrE2E$PID"
$saved = @{}
foreach ($n in 'APPDATA', 'XR_EYE_CAPTURE_TEST_HOME_KEY', 'XR_EYE_CAPTURE_REQUIRE_TRAY', 'XR_EYE_CAPTURE_TRAY_IPC', 'XR_EYE_CAPTURE_AUTORECORD',
               'XR_EYE_CAPTURE_CONFIG', 'MOCK_OPENVR_SYSTEM', 'XR_EYE_CAPTURE_OUTPUT_DIR', 'XR_EYE_CAPTURE_MIRROR_OPENVR_DLL',
               'XR_EYE_CAPTURE_TEST_SHM', 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_VRSERVER', 'MOCK_OPENVR_QUIT_AFTER_MS',
               'MOCK_OPENVR_MIRROR_COMBINED', 'MOCK_OPENVR_NO_SERVER', 'MOCK_OPENVR_SCENE_PID', 'MOCK_OPENVR_MIRROR_KEYEDMUTEX') { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
$env:APPDATA = $appdata
$env:XR_EYE_CAPTURE_TEST_HOME_KEY = $testKey
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
$env:XR_EYE_CAPTURE_TRAY_IPC = '0'
$env:XR_EYE_CAPTURE_AUTORECORD = '4'
$rawCfg = 'projection=flat;symmetric_fov=0;head_smoothing=0;record_for_youtube=0;max_width=0;max_fps=0;codec=hevc;swap_eyes=0;gamma=auto'
$symCfg = 'projection=flat;symmetric_fov=1;head_smoothing=0;record_for_youtube=0;max_width=0;max_fps=0;codec=hevc;swap_eyes=0;gamma=auto'
$results = New-Object System.Collections.Generic.List[object]

function RunCase($name, [string[]]$e2eArgs, $cfg) {
    $out = Join-Path $outRoot $name
    if (Test-Path $log) { Remove-Item $log -Force }
    $env:XR_EYE_CAPTURE_CONFIG = $cfg
    Write-Host "`n=== $name" -ForegroundColor Cyan
    $o = & $exe --game-dir $game --out-dir $out @e2eArgs 2>&1 | Out-String
    $code = $LASTEXITCODE
    $o.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
    $lt = if (Test-Path $log) { Get-Content $log -Raw -Encoding UTF8 } else { '' }
    $m = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
    $s = [regex]::Match($o, 'E2E_SYM=(\d+)x(\d+)')
    return @{ out = $o; code = $code; log = $lt; mp4 = $(if ($m.Success) { $m.Groups[1].Value.Trim() } else { $null });
              symW = $(if ($s.Success) { [int]$s.Groups[1].Value } else { 0 }); symH = $(if ($s.Success) { [int]$s.Groups[2].Value } else { 0 }) }
}
function Need($ok, $what, [ref]$fails) { if (-not $ok) { $fails.Value += $what; Write-Host "  FAIL $what" -ForegroundColor Red } }
function Record($name, $fails) {
    $ok = $fails.Count -eq 0
    Write-Host ("  -> {0}" -f $(if ($ok) { 'ok' } else { 'FAIL: ' + ($fails -join '; ') })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
    $results.Add([pscustomobject]@{ case = $name; ok = $ok; why = ($fails -join '; ') })
}
function Want($name) { return (-not $Only) -or ($name -match $Only) }
# one decoded pixel (r, g, b; full range like verify_mp4.ps1) of the frame at 2 s
$ffDir = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffmpeg.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1).DirectoryName
function PixelAt($mp4, [int]$x, [int]$y) {
    if (-not $ffDir) { return $null }
    $raw = Join-Path $b 'px.raw'
    & (Join-Path $ffDir 'ffmpeg.exe') -v error -y -ss 2 -i $mp4 -frames:v 1 -vf "scale=in_color_matrix=bt709:in_range=limited:out_range=full,crop=1:1:${x}:${y},format=rgb24" -f rawvideo $raw
    if (-not (Test-Path $raw)) { return $null }
    $bt = [System.IO.File]::ReadAllBytes($raw)
    Remove-Item $raw
    if ($bt.Length -lt 3) { return $null }
    return @([int]$bt[0], [int]$bt[1], [int]$bt[2])
}

# ---- layouts x interfaces (raw eyes) ----
foreach ($iface in 'cpp', 'fntable') {
    foreach ($layout in 'two', 'wide', 'array', 'msaa', 'typeless', 'fp16') {
        $name = "${layout}_$iface"
        if (-not (Want $name)) { continue }
        $r = RunCase $name @('--layout', $layout, '--iface', $iface, '--seconds', '5') $rawCfg
        $f = @()
        Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
        Need ($r.log -match '==== VR EyeCapture SteamVR proxy v[\d.]+ loaded into .*openvr_e2e\.exe.* -- real .*openvr_api_real\.dll loaded') 'load banner' ([ref]$f)
        if ($iface -eq 'cpp') { Need ($r.log -match 'IVRCompositor_028 \(C\+\+ object .*\): vtable .* patched in place -- slot 2 WaitGetPoses, slot 5 Submit') 'vtable patch line' ([ref]$f) }
        else { Need ($r.log -match 'FnTable:IVRCompositor_028 \(FnTable .*\): copied \d+ entries') 'FnTable copy line' ([ref]$f) }
        Need ($r.log -match 'frame layout \(pair \d+\): ') 'layout line' ([ref]$f)
        Need ($r.log -match 'record: start .*_SBS\.mp4 \| SteamVR D3D11 \| 3840x1080 SBS .*\| 90 fps') 'record start line (3840x1080, 90 fps)' ([ref]$f)
        Need ($r.log -match 'recording summary: ') 'recording summary (finalized)' ([ref]$f)
        Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
        if ($r.mp4) {
            $fmt = if ($layout -eq 'fp16') { 'unorm' } else { 'srgb' }
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -Format $fmt -ExpectAudio
            Need ($LASTEXITCODE -eq 0) 'verify_mp4' ([ref]$f)
        }
        Record $name $f
    }
}
if (Want 'two_cpp_rightfirst') {
    $r = RunCase 'two_cpp_rightfirst' @('--layout', 'two', '--iface', 'cpp', '--seconds', '5', '--right-first') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) { & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio; Need ($LASTEXITCODE -eq 0) 'verify_mp4 (left half must still be the left eye)' ([ref]$f) }
    Record 'two_cpp_rightfirst' $f
}

# ---- two VR sessions in one process: shutdown restores the hooks, the re-init re-arms them and records again ----
foreach ($iface in 'cpp', 'fntable') {
    $name = "reinit_$iface"
    if (-not (Want $name)) { continue }
    $r = RunCase $name @('--layout', 'two', '--iface', $iface, '--seconds', '5', '--cycles', '2') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed (counts / shutdowns / Submit results over both sessions)' ([ref]$f)
    Need ($r.out -match 'E2E_MP4S=2') 'two MP4s (one per VR session)' ([ref]$f)
    Need (([regex]::Matches($r.log, 'recording summary: ')).Count -eq 2) 'two finalized recordings' ([ref]$f)
    Need (([regex]::Matches($r.log, 'shutdown -- 1 vtable\(s\) restored|VR_ShutdownInternal after')).Count -ge 2) 'both shutdowns logged' ([ref]$f)
    if ($iface -eq 'cpp') { Need ($r.log -match 'vtable .* patched again \(same vtable as before the last shutdown\) .*\(1 record\(s\)\)') 'the re-init re-armed the same record (still 1)' ([ref]$f) }
    Record $name $f
}

# ---- a threaded game: WaitGetPoses on its own thread, Submit on the main thread ----
foreach ($iface in 'cpp', 'fntable') {
    $name = "threads_$iface"
    if (-not (Want $name)) { continue }
    $r = RunCase $name @('--layout', 'two', '--iface', $iface, '--seconds', '5', '--threads', '--pose', 'wait') $symCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed (the mock must get every call 1:1 from both threads)' ([ref]$f)
    Need ($r.log -match 'head pose of the recorded frames: WaitGetPoses render pose') 'the WaitGetPoses pose is used' ([ref]$f)
    Need ($r.log -match 'recording summary: ') 'recording finalized' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) {
        & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW ($r.symW / 2) -EyeH $r.symH -OutW $r.symW -OutH $r.symH -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio -SkipPixels
        Need ($LASTEXITCODE -eq 0) 'verify_mp4 (symmetric size)' ([ref]$f)
    }
    Record $name $f
}

# ---- poses: symmetric_fov needs pose + fov ----
foreach ($c in @(@{ n = 'pose_wait_sym'; a = @('--pose', 'wait'); from = 'WaitGetPoses render pose'; sys = '' },
                 @{ n = 'pose_submit_sym'; a = @('--pose', 'submit'); from = 'Submit \(TextureWithPose\)'; sys = '' },
                 @{ n = 'pose_wait_sym_system019'; a = @('--pose', 'wait', '--iface', 'fntable'); from = 'WaitGetPoses render pose'; sys = '019' },
                 @{ n = 'pose_none_sym'; a = @('--pose', 'none'); from = 'none \(raw eyes\)'; sys = '' })) {
    if (-not (Want $c.n)) { continue }
    $env:MOCK_OPENVR_SYSTEM = $c.sys
    $r = RunCase $c.n (@('--layout', 'two', '--seconds', '5') + $c.a) $symCfg
    $env:MOCK_OPENVR_SYSTEM = $null
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($r.log -match ('head pose of the recorded frames: ' + $c.from)) "pose source line ($($c.from))" ([ref]$f)
    if ($c.sys -eq '019') { Need ($r.log -match 'steamvr: IVRSystem_019 \(slots: GetProjectionRaw 2, GetEyeToHeadTransform 4, float property 23\) \| display 90\.00 Hz') 'IVRSystem_019 fallback + 90 Hz through slot 23' ([ref]$f) }
    else { Need ($r.log -match 'steamvr: IVRSystem_022 .* \| display 90\.00 Hz') 'IVRSystem_022 + display 90 Hz' ([ref]$f) }
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) {
        if ($c.n -eq 'pose_none_sym') { $w = 3840; $h = 1080 } else { $w = $r.symW; $h = $r.symH }
        Write-Host "  expected frame ${w}x${h} (E2E_SYM $($r.symW)x$($r.symH))"
        & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW ($w / 2) -EyeH $h -OutW $w -OutH $h -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio -SkipPixels
        Need ($LASTEXITCODE -eq 0) "verify_mp4 at ${w}x${h}" ([ref]$f)
        Need ($r.symW -gt 0 -and $r.symW -ne 3840) 'E2E_SYM differs from the raw size (the test can tell them apart)' ([ref]$f)
        if ($c.n -ne 'pose_none_sym') {
            # fov SIGN: the mock's frustum is asymmetric vertically (t -1.0 = 45 deg up, b 0.9 down), symmetric_fov keeps +-0.9:
            # the picture loses rows at the TOP only, so the eye-id band (source rows h/2 .. 5h/8) covers the output rows
            # 0.472 .. 0.604. With up / down swapped it would cover 0.528 .. 0.660 and rows 0.48 / 0.50 would show band B.
            $ok = $true
            foreach ($fy in 0.48, 0.50) {
                foreach ($eye in 0, 1) {
                    $px = PixelAt $r.mp4 ([int]($w / 4 + $eye * $w / 2)) ([int]($h * $fy))
                    $want = if ($eye -eq 0) { @(200, 40, 40) } else { @(40, 40, 200) }
                    $good = $px -and ([math]::Abs($px[0] - $want[0]) -le 30) -and ([math]::Abs($px[1] - $want[1]) -le 30) -and ([math]::Abs($px[2] - $want[2]) -le 30)
                    Write-Host ("  fov sign: row {0:F2} {1} eye -> {2} (want {3})" -f $fy, $(if ($eye) { 'right' } else { 'left' }), ($px -join ','), ($want -join ','))
                    if (-not $good) { $ok = $false }
                }
            }
            Need $ok 'fov sign: the eye-id band sits where +45 deg up / -42 deg down puts it (a swapped up/down sign fails here)' ([ref]$f)
        }
    }
    Record $c.n $f
}

# ---- submit error passed through ----
if (Want 'submit_error') {
    $r = RunCase 'submit_error' @('--layout', 'two', '--seconds', '5', '--submit-error', '101') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed (the game did not get 101 on every Submit, or counts differ)' ([ref]$f)
    Need ($r.log -match "the runtime's Submit\((left|right)\) returned error 101") 'error logged' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4 (the capture runs before the Submit is forwarded)' ([ref]$f)
    Record 'submit_error' $f
}

# ---- unknown compositor version ----
if (Want 'unknown_version') {
    $r = RunCase 'unknown_version' @('--layout', 'two', '--seconds', '3', '--version', '099') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($r.log -match 'IVRCompositor_099 is a version this proxy does not know -- forwarded untouched, this game is NOT recorded') 'not-known line' ([ref]$f)
    Need ($null -eq $r.mp4) 'an MP4 was made' ([ref]$f)
    Record 'unknown_version' $f
}

# ---- steamvr_enabled = 0 ----
if (Want 'steamvr_disabled') {
    $r = RunCase 'steamvr_disabled' @('--layout', 'two', '--seconds', '3') ($rawCfg + ';steamvr_enabled=0')
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($r.log -match 'steamvr_enabled = 0: pure forwarder') 'pure forwarder line' ([ref]$f)
    Need ($r.log -notmatch 'patched in place|record: start') 'something was hooked / recorded' ([ref]$f)
    Need ($null -eq $r.mp4) 'an MP4 was made' ([ref]$f)
    Record 'steamvr_disabled' $f
}

# ---- dormant: require_tray and no tray ----
if (Want 'dormant') {
    if (Get-Process EyeCaptureTray -ErrorAction SilentlyContinue) {
        Write-Host "`n=== dormant: SKIPPED (an EyeCaptureTray is running, so the proxy would arm)" -ForegroundColor Yellow
        $results.Add([pscustomobject]@{ case = 'dormant'; ok = $true; why = 'skipped: tray running' })
    } else {
        $env:XR_EYE_CAPTURE_REQUIRE_TRAY = $null
        $r = RunCase 'dormant' @('--layout', 'two', '--seconds', '3') $rawCfg
        $env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
        $f = @()
        Need ($r.code -eq 0) 'openvr_e2e failed (counts must be intact)' ([ref]$f)
        Need ($r.log -match 'EyeCaptureTray not running, proxy dormant') 'dormant line' ([ref]$f)
        Need ($r.log -notmatch 'patched in place|record: start') 'hooked / recorded while dormant' ([ref]$f)
        Need ($null -eq $r.mp4) 'an MP4 was made' ([ref]$f)
        Record 'dormant' $f
    }
}

# ---- no sidecar: pure forwarder, nothing written; then the registry fallback (test key) ----
if (Want 'no_home') {
    Move-Item $sidecar "$sidecar.off"
    if (Test-Path (Join-Path $home2 'logs')) { Remove-Item -Recurse -Force (Join-Path $home2 'logs') }
    $r = RunCase 'no_home' @('--layout', 'two', '--seconds', '3') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need (-not (Test-Path (Join-Path $home2 'logs'))) 'a log was written without a home' ([ref]$f)
    Need (-not (Get-ChildItem $game -Recurse -Filter *.log -ErrorAction SilentlyContinue)) 'a log in the game folder' ([ref]$f)
    Need (@(Get-ChildItem $game).Count -eq 3) 'something was created in the game folder (expected: the two DLLs + the moved-away sidecar)' ([ref]$f)
    Need ($null -eq $r.mp4) 'an MP4 was made' ([ref]$f)
    Record 'no_home' $f
    if (Want 'registry_home') {
        $k = New-Item -Path "HKCU:\$testKey" -Force
        New-ItemProperty -Path "HKCU:\$testKey" -Name Home -Value $home2 -PropertyType String -Force | Out-Null
        $r = RunCase 'registry_home' @('--layout', 'two', '--seconds', '5') $rawCfg
        $f = @()
        Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
        Need ($r.log -match [regex]::Escape("home from registry HKCU\$testKey\Home")) 'home from the test registry key' ([ref]$f)
        Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
        Record 'registry_home' $f
        Remove-Item -Path "HKCU:\$testKey" -Recurse -Force
        if (-not (Get-ChildItem 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue)) { Remove-Item 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue }
    }
    Move-Item "$sidecar.off" $sidecar
}

# ---- the real DLL is missing ----
if (Want 'real_missing') {
    $realDll = Join-Path $game 'openvr_api_real.dll'
    Move-Item $realDll "$realDll.off"
    $r = RunCase 'real_missing' @('--layout', 'two', '--seconds', '1') $rawCfg
    Move-Item "$realDll.off" $realDll
    $f = @()
    Need ($r.code -eq 1 -and $r.out -match 'VR_InitInternal2 -> token 0, error 100') 'the game must get VR init error 100 (and not crash)' ([ref]$f)
    Need ($r.log -match 'openvr_api_real\.dll NOT LOADED') 'NOT LOADED in the banner' ([ref]$f)
    Need ($r.log -match 'missing or has no VR_InitInternal2') 'missing export logged' ([ref]$f)
    Record 'real_missing' $f
}

# ---- SteamVR quick mode: EyeCaptureMirror.exe against the mock (XR_EYE_CAPTURE_MIRROR_OPENVR_DLL) ----
$mirrorExe = Join-Path $bin 'EyeCaptureMirror.exe'
$mockDll = Join-Path $bin 'mock_openvr.dll'
$quickCfg = $rawCfg + ';steamvr_mode=quick'
function RunMirror($name, $cfg, [hashtable]$extraEnv, [int]$timeoutS = 40, [scriptblock]$during = $null) {
    $out = Join-Path $outRoot $name
    if (Test-Path $log) { Remove-Item $log -Force }
    $env:XR_EYE_CAPTURE_CONFIG = $cfg
    $env:XR_EYE_CAPTURE_OUTPUT_DIR = $out
    $env:XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = $mockDll
    $keys = @()
    if ($extraEnv) { foreach ($k in $extraEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $extraEnv[$k]); $keys += $k } }
    Write-Host "`n=== $name" -ForegroundColor Cyan
    $so = Join-Path $b "$name.stdout.txt"
    $p = Start-Process -FilePath $mirrorExe -ArgumentList '--home', "`"$home2`"" -PassThru -RedirectStandardOutput $so
    $null = $p.Handle   # keeps the handle, so ExitCode is available after the exit
    if ($during) { & $during $p }
    $done = $p.WaitForExit($timeoutS * 1000)
    if (-not $done) { Stop-Process -Id $p.Id -Force; Write-Host '  mirror did not exit -- killed' -ForegroundColor Red }
    foreach ($k in $keys) { [Environment]::SetEnvironmentVariable($k, $null) }
    $env:XR_EYE_CAPTURE_OUTPUT_DIR = $null
    $env:XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = $null
    $o = if (Test-Path $so) { Get-Content $so -Raw } else { '' }
    $lt = if (Test-Path $log) { Get-Content $log -Raw -Encoding UTF8 } else { '' }
    ($lt -split "`r?`n") | Where-Object { $_ -match 'mirror|record: start|recording summary' } | Select-Object -First 25 | ForEach-Object { Write-Host "  $_" }
    if ($o) { Write-Host "  stdout: $($o.Trim())" }
    $mp4 = @(Get-ChildItem $out -Filter *.mp4 -ErrorAction SilentlyContinue)
    return @{ code = $(if ($done) { $p.ExitCode } else { -1 }); done = $done; log = $lt; out = $o; mp4 = $(if ($mp4.Count) { $mp4[0].FullName } else { $null }) }
}
foreach ($c in @(@{ n = 'mirror_two_textures'; comb = ''; km = '' }, @{ n = 'mirror_one_texture'; comb = '1'; km = '' },
                 @{ n = 'mirror_keyed_mutex'; comb = ''; km = '1' })) {
    if (-not (Want $c.n)) { continue }
    $r = RunMirror $c.n $quickCfg @{ MOCK_OPENVR_QUIT_AFTER_MS = '7000'; MOCK_OPENVR_MIRROR_COMBINED = $c.comb; MOCK_OPENVR_MIRROR_KEYEDMUTEX = $c.km }
    if ($c.km) { if ($r.log -notmatch 'mirror textures: .*\(keyed mutex\)') { Write-Host '  FAIL keyed mutex not detected' -ForegroundColor Red; $r.code = 99 }
                 if ($r.log -match 'AcquireSync\(0\) on the mirror texture') { Write-Host '  FAIL AcquireSync failed' -ForegroundColor Red; $r.code = 98 } }
    $f = @()
    Need ($r.done -and $r.code -eq 0) "mirror exit code 0 (got $($r.code))" ([ref]$f)
    Need ($r.log -match '==== VR EyeCapture SteamVR mirror \(quick mode\)') 'banner' ([ref]$f)
    Need ($r.log -match 'mirror: VR_InitInternal2\(Background\) OK \| IVRSystem_022 -> \S+ \(error 0\) \| FnTable:IVRCompositor_022 -> \S+ \(error 0\)') 'Background init + the two interface versions logged' ([ref]$f)
    Need ($r.log -match 'mirror: D3D11 device on adapter 0 ') 'device on the adapter of GetDXGIOutputInfo' ([ref]$f)
    if ($c.comb) { Need ($r.log -match 'mirror textures: left 2880x1600 .* -- ONE texture for both eyes') 'one texture for both eyes detected' ([ref]$f) }
    else { Need ($r.log -match 'mirror textures: left 1440x1600 R8G8B8A8_UNORM_SRGB, right 1440x1600') 'two mirror textures' ([ref]$f) }
    Need ($r.log -match 'record: start .*_SBS\.mp4 \| SteamVR mirror \(quick mode\) \| 2880x1600 SBS \(eyes 1440x1600 \+ 1440x1600\) \| 90 fps') 'record start 2880x1600 @ 90 fps' ([ref]$f)
    Need ($r.log -match 'recording summary: ') 'recording finalized' ([ref]$f)
    Need ($r.log -match 'mirror: VREvent_Quit') 'VREvent_Quit -> exit' ([ref]$f)
    Need ($r.out -match 'mirror gets 2 releases 2,' -and $r.out -match 'errors 0') 'mock: 2 GetMirrorTextureD3D11, 2 releases, no invalid call' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) {
        & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1440 -EyeH 1600 -Fps 90 -Seconds 3.7 -DurationTolerance 0.5 -Format srgb -ExpectAudio
        Need ($LASTEXITCODE -eq 0) 'verify_mp4 (pattern, left half = left eye)' ([ref]$f)
    }
    Record $c.n $f
}
if (Want 'mirror_dome_refused') {
    $r = RunMirror 'mirror_dome_refused' ($quickCfg -replace 'projection=flat', 'projection=vr180') @{ MOCK_OPENVR_QUIT_AFTER_MS = '6000' }
    $f = @()
    Need ($r.log -match 'quick mode records flat side-by-side only -- projection = vr180 is ignored') 'dome refused with a log line' ([ref]$f)
    Need ($r.mp4 -and $r.mp4 -match '_SBS\.mp4$') 'recorded flat (_SBS.mp4)' ([ref]$f)
    Record 'mirror_dome_refused' $f
}
if (Want 'mirror_per_game_mode') {
    $r = RunMirror 'mirror_per_game_mode' $rawCfg @{} 10
    $f = @()
    Need ($r.done -and $r.code -eq 0 -and $r.log -match 'mirror: steamvr_mode = per_game -- nothing to do' -and $null -eq $r.mp4) 'steamvr_mode = per_game: exits at once, no MP4' ([ref]$f)
    Record 'mirror_per_game_mode' $f
}
if (Want 'mirror_no_server') {
    $r = RunMirror 'mirror_no_server' $quickCfg @{ MOCK_OPENVR_NO_SERVER = '1' } 20 {
        param($p)
        Start-Sleep -Seconds 4
        $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\OpenXR-EyeCapture-MirrorQuit'); [void]$ev.Set(); $ev.Dispose()
    }
    $f = @()
    Need ($r.log -match 'VR_InitInternal2\(Background\) -> error 121 \(SteamVR is not running\) -- retrying every 2 s') 'no server: waits' ([ref]$f)
    Need ($r.done -and $r.code -eq 0 -and $r.log -match 'mirror: quit requested by the tray') 'the quit event ends it' ([ref]$f)
    Record 'mirror_no_server' $f
}
if (Want 'mirror_yields_to_owner') {
    $shmName = "Local\EyeCapture-MirrorTest-$PID"
    $sim = Start-Process (Join-Path $bin 'status_sim.exe') -ArgumentList $shmName, '20', 'x_SBS.mp4' -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $r = RunMirror 'mirror_yields_to_owner' $quickCfg @{ MOCK_OPENVR_QUIT_AFTER_MS = '6000'; XR_EYE_CAPTURE_TEST_SHM = $shmName }
    if (-not $sim.HasExited) { Stop-Process -Id $sim.Id }
    $f = @()
    Need ($r.log -match "mirror: idle -- pid $($sim.Id) \(SIM\) records this game \(per game wins\)") 'idle while another process owns the status block' ([ref]$f)
    Need ($null -eq $r.mp4) 'no MP4 while yielding (autorecord did not start)' ([ref]$f)
    Record 'mirror_yields_to_owner' $f
}
if (Want 'mirror_yields_to_proxy_game') {
    # a game recorded through the proxy is the SteamVR scene app (MOCK_OPENVR_SCENE_PID): its openvr_api_real.dll is seen
    $env:XR_EYE_CAPTURE_CONFIG = $rawCfg
    $env:XR_EYE_CAPTURE_OUTPUT_DIR = Join-Path $outRoot 'mirror_proxy_game_game'
    $gameP = Start-Process $exe -ArgumentList '--game-dir', "`"$game`"", '--out-dir', "`"$env:XR_EYE_CAPTURE_OUTPUT_DIR`"", '--layout', 'two', '--seconds', '12' -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $r = RunMirror 'mirror_yields_to_proxy_game' $quickCfg @{ MOCK_OPENVR_QUIT_AFTER_MS = '6000'; MOCK_OPENVR_SCENE_PID = "$($gameP.Id)" }
    if (-not $gameP.WaitForExit(20000)) { Stop-Process -Id $gameP.Id -Force }
    $f = @()
    Need ($r.log -match "mirror: idle -- the per-game SteamVR recorder in pid $($gameP.Id) records this game") 'idle while the scene app has the proxy loaded' ([ref]$f)
    Need ($null -eq $r.mp4) 'no mirror MP4' ([ref]$f)
    Record 'mirror_yields_to_proxy_game' $f
}
if (Want 'mirror_tray') {
    # the tray starts EyeCaptureMirror while "vrserver" (here: EYECAP_TRAY_TEST_VRSERVER = a copy of status_sim) runs, and stops it
    $tt = Join-Path $b 'tray'
    New-Item -ItemType Directory -Force $tt | Out-Null
    Copy-Item (Join-Path $bin 'EyeCaptureTray.exe'), $mirrorExe $tt
    $fakeVr = Join-Path $tt 'fakevrserver.exe'
    Copy-Item (Join-Path $bin 'status_sim.exe') $fakeVr
    [System.IO.File]::WriteAllText((Join-Path $tt 'config.ini'), "[capture]`r`naudio_enabled = 0`r`n[steamvr]`r`nsteamvr_mode = quick`r`n[debug]`r`nlog_level = info`r`n", (New-Object System.Text.UTF8Encoding $false))
    $ttLog = Join-Path $tt 'logs\eye_capture.log'
    $env:XR_EYE_CAPTURE_CONFIG = $quickCfg
    $env:XR_EYE_CAPTURE_OUTPUT_DIR = Join-Path $outRoot 'mirror_tray'
    $env:XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = $mockDll
    $env:EYECAP_TRAY_TEST_SHM = "eyecap-mirror-tray-$PID"
    $env:EYECAP_TRAY_TEST_VRSERVER = 'fakevrserver.exe'
    $tray = Start-Process (Join-Path $tt 'EyeCaptureTray.exe') -PassThru
    Start-Sleep -Seconds 3
    $vr = Start-Process $fakeVr -ArgumentList "Local\EyeCapture-FakeVr-$PID", '60', 'x.mp4' -PassThru -WindowStyle Hidden
    $started = $false
    for ($i = 0; $i -lt 20 -and -not $started; $i++) { Start-Sleep -Milliseconds 500; $started = [bool](Get-Process EyeCaptureMirror -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$tt*" }) }
    Start-Sleep -Seconds 6   # autorecord 4 s
    Stop-Process -Id $vr.Id
    $stopped = $false
    for ($i = 0; $i -lt 40 -and -not $stopped; $i++) { Start-Sleep -Milliseconds 500; $stopped = -not (Get-Process EyeCaptureMirror -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$tt*" }) }
    Stop-Process -Id $tray.Id -ErrorAction SilentlyContinue
    # the test-mode tray wrote its folder to the test home key (XR_EYE_CAPTURE_TEST_HOME_KEY): remove it again
    Remove-Item -Path "HKCU:\$testKey" -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Get-ChildItem 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue)) { Remove-Item 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue }
    foreach ($v in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_VRSERVER', 'XR_EYE_CAPTURE_MIRROR_OPENVR_DLL', 'XR_EYE_CAPTURE_OUTPUT_DIR') { [Environment]::SetEnvironmentVariable($v, $null) }
    $tl = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8
    $ml = if (Test-Path $ttLog) { Get-Content $ttLog -Raw -Encoding UTF8 } else { '' }
    ($tl -split "`r?`n") | Where-Object { $_ -match 'quick mode|steamvr_mode' } | ForEach-Object { Write-Host "  $_" }
    $f = @()
    Need ($started -and $tl -match 'tray: SteamVR quick mode: fakevrserver\.exe runs -- started .*EyeCaptureMirror\.exe') 'the tray started the mirror when "vrserver" ran' ([ref]$f)
    Need ($stopped -and $tl -match 'stopping EyeCaptureMirror \(SteamVR is not running\)' -and $ml -match 'mirror: quit requested by the tray') 'the tray stopped it when "vrserver" exited' ([ref]$f)
    Need ($ml -match 'recording summary: ' -and @(Get-ChildItem (Join-Path $outRoot 'mirror_tray') -Filter *_SBS.mp4 -ErrorAction SilentlyContinue).Count -eq 1) 'one finalized MP4 (home = the tray folder)' ([ref]$f)
    Record 'mirror_tray' $f
}
if (Want 'mirror_tray_backoff') {
    # the mirror fails at once (its openvr_api.dll does not exist -> exit 1): after 3 failures within a minute the tray stops
    # relaunching it (one balloon) until the SteamVR state changes
    $tt = Join-Path $b 'tray'
    if (-not (Test-Path (Join-Path $tt 'EyeCaptureTray.exe'))) {
        New-Item -ItemType Directory -Force $tt | Out-Null
        Copy-Item (Join-Path $bin 'EyeCaptureTray.exe'), $mirrorExe $tt
        Copy-Item (Join-Path $bin 'status_sim.exe') (Join-Path $tt 'fakevrserver.exe')
        [System.IO.File]::WriteAllText((Join-Path $tt 'config.ini'), "[capture]`r`naudio_enabled = 0`r`n[steamvr]`r`nsteamvr_mode = quick`r`n[debug]`r`nlog_level = info`r`n", (New-Object System.Text.UTF8Encoding $false))
    }
    if (Test-Path (Join-Path $tt 'logs\tray.log')) { Remove-Item (Join-Path $tt 'logs\tray.log') }
    $env:XR_EYE_CAPTURE_CONFIG = $quickCfg
    $env:XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = Join-Path $b 'no_such_openvr_api.dll'
    $env:EYECAP_TRAY_TEST_SHM = "eyecap-mirror-backoff-$PID"
    $env:EYECAP_TRAY_TEST_VRSERVER = 'fakevrserver.exe'
    $tray = Start-Process (Join-Path $tt 'EyeCaptureTray.exe') -PassThru
    $vr = Start-Process (Join-Path $tt 'fakevrserver.exe') -ArgumentList "Local\EyeCapture-FakeVr2-$PID", '60', 'x.mp4' -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 14
    $tl = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8
    $starts1 = ([regex]::Matches($tl, 'started .*EyeCaptureMirror\.exe')).Count
    Stop-Process -Id $vr.Id
    Start-Sleep -Seconds 3
    $tl = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8
    Stop-Process -Id $tray.Id -ErrorAction SilentlyContinue
    Remove-Item -Path "HKCU:\$testKey" -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Get-ChildItem 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue)) { Remove-Item 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue }
    foreach ($v in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_VRSERVER', 'XR_EYE_CAPTURE_MIRROR_OPENVR_DLL') { [Environment]::SetEnvironmentVariable($v, $null) }
    ($tl -split "`r?`n") | Where-Object { $_ -match 'quick mode' } | ForEach-Object { Write-Host "  $_" }
    $f = @()
    Need ($starts1 -eq 3 -and $tl -match 'failed 3 times within a minute -- not started again' -and ([regex]::Matches($tl, 'balloon \[SteamVR quick mode\] The SteamVR mirror recorder keeps failing')).Count -eq 1) 'three failed starts, then no more (one balloon)' ([ref]$f)
    Need ($tl -match 'the SteamVR state changed -- EyeCaptureMirror may start again') 'unblocked when SteamVR stopped' ([ref]$f)
    Record 'mirror_tray_backoff' $f
}

foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
Write-Host "`n=== run_steamvr summary" -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-String | Write-Host
$bad = @($results | Where-Object { -not $_.ok })
if ($bad.Count) { Write-Host "run_steamvr: $($bad.Count) of $($results.Count) cases FAILED" -ForegroundColor Red; exit 1 }
Write-Host "run_steamvr: all $($results.Count) cases passed" -ForegroundColor Green
exit 0
