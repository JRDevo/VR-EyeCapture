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
      layout (two | wide | array | msaa | typeless | fp16 | r10 (10-bit) | bgrx) x interface (cpp | fntable), raw eyes (symmetric_fov 0): a valid
        MP4 (verify_mp4.ps1: size 3840x1080, 90 fps, st3d, audio, grey steps = gamma, left half = left eye: bounds / slices /
        eye order), log lines for the load banner, the patch (vtable / FnTable), the layout and the record start.
      two / cpp with the right eye submitted first.
      D3D12 games (openvr_e2e --api d3d12: TextureType_DirectX12, D3D12TextureData_t with the game's direct queue, textures
        handed over in PIXEL_SHADER_RESOURCE): layouts two / wide (one texture + bounds for both eyes) / array / msaa /
        typeless / fp16 / r10 x cpp / fntable, right eye first, two VR sessions, a threaded game, the in-headset dot (its
        texture on a private D3D11 device): the same MP4 checks as D3D11 + "SteamVR D3D12" in the record start line + the
        D3D12 debug layer of the fake game reports no error (a texture left in another state = a barrier mismatch at the
        game's next frame; skipped with a note when the debug layer is not installed) + the mock saw valid D3D12 Submits.
      head smoothing (0.6 + symmetric_fov) and the VR180 dome, D3D11 and D3D12: re-projection lines, the symmetric size /
        a _VR180.mp4 whose dome size comes from the source density (the poses and fovs reached the recorder).
      live share, D3D11 and D3D12 (tests/live_share_test.exe = the OBS plugin's consumer on a private block): raw eyes
        bit-exact, the producer stops when the consumer closes, nothing recorded.
      in-headset dot (IVROverlay; the mock's "FnTable:IVROverlay_027" records every call, "mock overlay: ..."): one overlay
        with a per-process key, width / HMD-relative position from indicator_size / _x / _y / _z, a 64x64 texture per state
        (the last one = saved, green), shown, pulsed through SetOverlayAlpha (0.35 .. 1.0), destroyed at shutdown, no bad
        handle, no other slot; vr_indicator = 0 or steamvr_indicator = 0: no IVROverlay asked; only IVROverlay_026 (MOCK_OPENVR_OVERLAY=026): its own
        slots; no IVROverlay at all: logged, no dot, recorded anyway.
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
      no DLL override: SteamVR's own openvr_api.dll through an openvrpaths.vrpath (XR_EYE_CAPTURE_TEST_OPENVRPATHS; first
        existing runtime wins) -> records; no vrpath file -> exit code 4 + one log line ("Quick mode needs SteamVR installed");
      projection = vr180 refused (logged, recorded flat); steamvr_mode = per_game: exits at once; no SteamVR server: waits,
        the tray's quit event ends it; another live owner of a private status block (status_sim): idle, no MP4; the scene
        app is a game recorded through the proxy (openvr_e2e, MOCK_OPENVR_SCENE_PID; its marker Local\OpenXR-EyeCapture-Proxy-<pid>):
        idle, no MP4 (per game wins);
      the scene app is an OpenXR game with the build layer (layer_e2e): its marker Local\OpenXR-EyeCapture-Layer-<pid> exists,
        the mirror is idle, no MP4; the scene app has no marker (this PowerShell): the mirror records. (The mirror only opens
        the named markers; it never reads another process.)
      tray: a scratch tray (steamvr_mode = quick, EYECAP_TRAY_TEST_VRSERVER = a renamed status_sim) starts the mirror while
        that "vrserver" runs and stops it (quit event, MP4 finalized) when it exits; a mirror that fails at once 3 times in a
        minute is not restarted (one balloon) until SteamVR changes; exit code 4 (no SteamVR openvr_api.dll) = one start,
        one balloon "Quick mode needs SteamVR installed", no relaunch until SteamVR changes; exit code 3 (another EyeCaptureMirror holds the
        single-instance mutex, held here by the script) is not a failure: tried again every 10 s, no back-off, and the mirror
        starts once the mutex is gone.
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
               'MOCK_OPENVR_MIRROR_COMBINED', 'MOCK_OPENVR_NO_SERVER', 'MOCK_OPENVR_SCENE_PID', 'MOCK_OPENVR_MIRROR_KEYEDMUTEX', 'XR_EYE_CAPTURE_TEST_OPENVRPATHS', 'MOCK_OPENVR_OVERLAY', 'XR_EYE_CAPTURE_TEST_LIVE_SHM') { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
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
    foreach ($layout in 'two', 'wide', 'array', 'msaa', 'typeless', 'fp16', 'r10', 'bgrx') {
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
            $fmt = if ($layout -in @('fp16', 'r10')) { 'unorm' } else { 'srgb' }   # linear data: the recorder applies the sRGB curve
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -Format $fmt -ExpectAudio
            Need ($LASTEXITCODE -eq 0) 'verify_mp4' ([ref]$f)
        }
        Record $name $f
    }
}
# ---- codecs (the layouts above record HEVC): H.264 and AV1 through the proxy ----
foreach ($codec in 'h264', 'av1') {
    $name = "two_cpp_$codec"
    if (-not (Want $name)) { continue }
    $r = RunCase $name @('--layout', 'two', '--iface', 'cpp', '--seconds', '5') ($rawCfg -replace 'codec=hevc', "codec=$codec")
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($r.log -match ('recording summary: OK [^\r\n]*3840x1080 ' + $(if ($codec -eq 'h264') { 'H\.264' } else { 'AV1' }))) "summary says $codec" ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) {
        & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -Codec $codec -ExpectAudio
        Need ($LASTEXITCODE -eq 0) "verify_mp4 ($codec)" ([ref]$f)
    }
    Record $name $f
}
if (Want 'two_cpp_rightfirst') {
    $r = RunCase 'two_cpp_rightfirst' @('--layout', 'two', '--iface', 'cpp', '--seconds', '5', '--right-first') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) { & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio; Need ($LASTEXITCODE -eq 0) 'verify_mp4 (left half must still be the left eye)' ([ref]$f) }
    Record 'two_cpp_rightfirst' $f
}

# ---- in-headset dot (IVROverlay) ----
function OverlayInfo($out) {
    $m = [regex]::Match($out, 'mock overlay: version (\S+), creates (\d+) key (\S+), width ([\d.]+), transform dev (\d+) at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\), textures (\d+) \(last (\d+)x(\d+) centre (-?\d+),(-?\d+),(-?\d+),(-?\d+)\), shows (\d+) hides (\d+), alpha calls (\d+) \(min ([-\d.]+) max ([-\d.]+)\), sort (\d+), destroys (\d+), bad handles (\d+), other slots (\d+)')
    if (-not $m.Success) { return $null }
    $g = $m.Groups
    return @{ ver = $g[1].Value; creates = [int]$g[2].Value; key = $g[3].Value; width = [double]$g[4].Value; dev = [int]$g[5].Value
              x = [double]$g[6].Value; y = [double]$g[7].Value; z = [double]$g[8].Value; textures = [int]$g[9].Value
              texW = [int]$g[10].Value; texH = [int]$g[11].Value; rgba = @([int]$g[12].Value, [int]$g[13].Value, [int]$g[14].Value, [int]$g[15].Value)
              shows = [int]$g[16].Value; hides = [int]$g[17].Value; alphas = [int]$g[18].Value; amin = [double]$g[19].Value; amax = [double]$g[20].Value
              sorts = [int]$g[21].Value; destroys = [int]$g[22].Value; bad = [int]$g[23].Value; other = [int]$g[24].Value }
}
foreach ($iface in 'cpp', 'fntable') {
    $name = "indicator_$iface"
    if (-not (Want $name)) { continue }
    $r = RunCase $name @('--layout', 'two', '--iface', $iface, '--seconds', '5') ($rawCfg + ';vr_indicator=1;indicator_x=0.30;indicator_y=-0.25;indicator_z=-1.0;indicator_size=0.03;indicator_opacity=1.0')
    $f = @()
    $o = OverlayInfo $r.out
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($null -ne $o) 'no "mock overlay" line' ([ref]$f)
    if ($o) {
        Need ($o.ver -eq 'IVROverlay_027' -and $o.creates -eq 1) "one overlay through FnTable:IVROverlay_027 (got $($o.ver), $($o.creates))" ([ref]$f)
        Need ($o.key -match '^jrdevo\.eyecapture\.indicator\.\d+$') "per-process key (got $($o.key))" ([ref]$f)
        Need ([math]::Abs($o.width - 0.03) -lt 0.0005) "width = indicator_size 0.03 (got $($o.width))" ([ref]$f)
        Need ($o.dev -eq 0 -and [math]::Abs($o.x - 0.30) -lt 0.005 -and [math]::Abs($o.y + 0.25) -lt 0.005 -and [math]::Abs($o.z + 1.0) -lt 0.005) "HMD-relative at (0.30, -0.25, -1.00) (got dev $($o.dev) at $($o.x), $($o.y), $($o.z))" ([ref]$f)
        Need ($o.textures -ge 3 -and $o.texW -eq 64 -and $o.texH -eq 64) "a 64x64 texture per state, starting + recording + saved (got $($o.textures), $($o.texW)x$($o.texH))" ([ref]$f)
        Need ($o.rgba[0] -lt 80 -and $o.rgba[1] -gt 180 -and $o.rgba[2] -lt 110 -and $o.rgba[3] -gt 240) "the last texture is the green 'saved' dot (centre $($o.rgba -join ','))" ([ref]$f)
        Need ($o.shows -ge 1 -and $o.alphas -ge 20 -and $o.amin -le 0.40 -and $o.amax -ge 0.99) "shown and pulsed (shows $($o.shows), alpha calls $($o.alphas), $($o.amin)..$($o.amax))" ([ref]$f)
        Need ($o.destroys -eq 1 -and $o.bad -eq 0 -and $o.other -eq 0) "destroyed once, no bad handle, no other slot called (destroys $($o.destroys), bad $($o.bad), other $($o.other))" ([ref]$f)
    }
    Need ($r.log -match 'indicator: SteamVR overlay jrdevo\.eyecapture\.indicator\.\d+ created through FnTable:IVROverlay_027') 'created line' ([ref]$f)
    Need ($r.log -match 'indicator: dot shown in the headset \(state starting\)') 'shown line' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) { & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio; Need ($LASTEXITCODE -eq 0) 'verify_mp4 (the pattern, no dot in the recording)' ([ref]$f) }
    Record $name $f
}
if (Want 'indicator_off') {
    $r = RunCase 'indicator_off' @('--layout', 'two', '--seconds', '4') ($rawCfg + ';vr_indicator=0')
    $f = @()
    $o = OverlayInfo $r.out
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($o -and $o.creates -eq 0 -and $o.ver -eq '-') 'no overlay with vr_indicator = 0' ([ref]$f)
    Need ($r.out -notmatch 'IVROverlay') 'IVROverlay was asked' ([ref]$f)
    Need ($r.log -match 'indicator: off \(vr_indicator = 0\)') 'off line' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    Record 'indicator_off' $f
}
if (Want 'indicator_steamvr_off') {
    $r = RunCase 'indicator_steamvr_off' @('--layout', 'two', '--seconds', '4') ($rawCfg + ';vr_indicator=1;steamvr_indicator=0')
    $f = @()
    $o = OverlayInfo $r.out
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($o -and $o.creates -eq 0 -and $r.out -notmatch 'IVROverlay') 'no overlay with steamvr_indicator = 0' ([ref]$f)
    Need ($r.log -match 'indicator: off \(steamvr_indicator = 0\)') 'off line' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    Record 'indicator_steamvr_off' $f
}
if (Want 'indicator_026') {
    $env:MOCK_OPENVR_OVERLAY = '026'
    $r = RunCase 'indicator_026' @('--layout', 'two', '--seconds', '4') ($rawCfg + ';vr_indicator=1')
    $env:MOCK_OPENVR_OVERLAY = $null
    $f = @()
    $o = OverlayInfo $r.out
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($o -and $o.ver -eq 'IVROverlay_026' -and $o.creates -eq 1 -and $o.shows -ge 1 -and $o.textures -ge 2 -and $o.bad -eq 0 -and $o.other -eq 0) 'IVROverlay_026 fallback with its own slots (show 43, texture 60)' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    Record 'indicator_026' $f
}
if (Want 'indicator_none') {
    $env:MOCK_OPENVR_OVERLAY = 'none'
    $r = RunCase 'indicator_none' @('--layout', 'two', '--seconds', '4') ($rawCfg + ';vr_indicator=1')
    $env:MOCK_OPENVR_OVERLAY = $null
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($r.log -match 'indicator: the runtime offers none of IVROverlay_027 / _026 / _025 -- no dot in the headset for this VR session \(recording unaffected\)') 'no-overlay line' ([ref]$f)
    Need ($r.log -match 'recording summary: OK') 'recorded anyway' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    Record 'indicator_none' $f
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

# ---- dynamic resolution (Half-Life: Alyx, GitHub issue #2): the eye rects change size during the recording ----
#   grow   = the recording starts on small eyes, larger ones later are cut WITH their fov (logged), never stretched
#   shrink = the recording starts on full eyes, smaller ones later fit (nothing cut)
foreach ($c in @(@{ n = 'dynres_grow_sym'; mode = 'grow'; cfg = $symCfg; cut = $true },
                 @{ n = 'dynres_shrink_sym'; mode = 'shrink'; cfg = $symCfg; cut = $false },
                 @{ n = 'dynres_grow_vr180'; mode = 'grow'; cfg = ($symCfg -replace 'projection=flat', 'projection=vr180'); cut = $true })) {
    if (-not (Want $c.n)) { continue }
    $r = RunCase $c.n @('--layout', 'wide', '--seconds', '4', '--dynres', $c.mode) $c.cfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    Need ($r.log -match 'steamvr: the eye size changes \(.* dynamic resolution\)') 'dynamic resolution logged' ([ref]$f)
    Need ($r.log -match 'recording summary: OK') 'recording finalized' ([ref]$f)
    Need ($r.log -match 'steamvr: pose check start: pose WaitGetPoses render pose') 'pose check start line' ([ref]$f)
    Need ($r.log -match 'steamvr: pose check end \(\d+ pairs\): pose from Submit 0, WaitGetPoses \d+, none 0') 'pose check end line' ([ref]$f)
    $cutLine = $r.log -match 'recording: \d+ eye images were larger than at the recording''s start'
    if ($c.cut) { Need $cutLine 'larger eyes cut with their fov (logged)' ([ref]$f) }
    else { Need (-not $cutLine) 'nothing cut (the recording started at the full size)' ([ref]$f) }
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    Record $c.n $f
}

# ---- VR180: eye pixels that never show picture (a game's lens mask) are never painted into the dome ----
#   mask_vr180     --mask: a 0.2 x 0.2 corner of each eye is always black (4 %) + the pattern's black grey step (band A
#                  step 0: 1/9 x 1/4 = 2.8 %) -> about 6.8 % of each eye never shows picture
#   nomask_vr180   only the pattern's black step -> about 2.8 %
foreach ($c in @(@{ n = 'mask_vr180'; a = @('--mask'); lo = 5.5; hi = 8.5 },
                 @{ n = 'nomask_vr180'; a = @(); lo = 1.5; hi = 4.0 })) {
    if (-not (Want $c.n)) { continue }
    $r = RunCase $c.n (@('--layout', 'two', '--seconds', '4') + $c.a) ($symCfg -replace 'projection=flat', 'projection=vr180')
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    $m = [regex]::Match($r.log, 'dome: lens mask = eye pixels that never showed picture \(never painted\): left ([\d.]+)%, right ([\d.]+)%')
    $ok = $m.Success -and [double]$m.Groups[1].Value -ge $c.lo -and [double]$m.Groups[1].Value -le $c.hi -and
          [double]$m.Groups[2].Value -ge $c.lo -and [double]$m.Groups[2].Value -le $c.hi
    Need $ok "lens mask $($c.lo) .. $($c.hi) % per eye ('$($m.Value)')" ([ref]$f)
    Need ($r.log -match 'recording summary: OK') 'recording finalized' ([ref]$f)
    Record $c.n $f
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
            # fov SIGN: the mock's frustum is asymmetric vertically. OpenVR's raw top / bottom are y-down ("Top and bottom are
            # backwards per SteamVR documentation"): t -1.0 = 45 deg DOWN, b 0.9 = 42 deg UP. symmetric_fov keeps +-0.9: the
            # picture loses rows at the BOTTOM only, so the eye-id band (source rows h/2 .. 5h/8) covers the output rows
            # 0.528 .. 0.660. With up / down swapped (before 0.5.1) it covered 0.472 .. 0.604: rows 0.62 / 0.64 showed band D.
            $ok = $true
            foreach ($fy in 0.62, 0.64) {
                foreach ($eye in 0, 1) {
                    $px = PixelAt $r.mp4 ([int]($w / 4 + $eye * $w / 2)) ([int]($h * $fy))
                    $want = if ($eye -eq 0) { @(200, 40, 40) } else { @(40, 40, 200) }
                    $good = $px -and ([math]::Abs($px[0] - $want[0]) -le 30) -and ([math]::Abs($px[1] - $want[1]) -le 30) -and ([math]::Abs($px[2] - $want[2]) -le 30)
                    Write-Host ("  fov sign: row {0:F2} {1} eye -> {2} (want {3})" -f $fy, $(if ($eye) { 'right' } else { 'left' }), ($px -join ','), ($want -join ','))
                    if (-not $good) { $ok = $false }
                }
            }
            Need $ok 'fov sign: the eye-id band sits where +42 deg up / -45 deg down puts it (a swapped up/down sign fails here)' ([ref]$f)
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

# ---- D3D12 games (TextureType_DirectX12: D3D12TextureData_t, resource + the game's direct queue, PIXEL_SHADER_RESOURCE) ----
# Same checks as the D3D11 layouts (two textures, one wide texture + bounds for both eyes, array, MSAA, typeless, fp16,
# 10-bit) + the D3D12 debug layer in the fake game must report no error (a texture left in another state than the game
# handed over = a barrier mismatch at the game's next frame) and the mock must have seen every Submit as a valid D3D12 one.
function D3d12Checks($r, [ref]$f) {
    Need ($r.log -match "capture AVAILABLE on the game's D3D12 device, copies on its direct queue") 'D3D12 source line' $f
    Need ($r.log -match 'steamvr: texture \S+: \d+x\d+ .* D3D12 flags 0x\w+ queue ') 'D3D12 texture line' $f
    Need ($r.out -match 'E2E_D3D12_DEBUG=(on|unavailable) errors 0') 'D3D12 debug layer errors (or no E2E_D3D12_DEBUG line)' $f
    if ($r.out -match 'E2E_D3D12_DEBUG=unavailable') { Write-Host '  note: D3D12 debug layer not installed -- resource states not validated' -ForegroundColor Yellow }
}
foreach ($iface in 'cpp', 'fntable') {
    foreach ($layout in 'two', 'wide', 'array', 'msaa', 'typeless', 'fp16', 'r10') {
        $name = "d3d12_${layout}_$iface"
        if (-not (Want $name)) { continue }
        $r = RunCase $name @('--api', 'd3d12', '--layout', $layout, '--iface', $iface, '--seconds', '5') $rawCfg
        $f = @()
        Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
        D3d12Checks $r ([ref]$f)
        Need ($r.log -match 'record: start .*_SBS\.mp4 \| SteamVR D3D12 \| 3840x1080 SBS .*\| 90 fps') 'record start line (SteamVR D3D12, 3840x1080, 90 fps)' ([ref]$f)
        Need ($r.log -match 'recording summary: OK') 'recording summary OK' ([ref]$f)
        Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
        if ($r.mp4) {
            $fmt = if ($layout -in @('fp16', 'r10')) { 'unorm' } else { 'srgb' }
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -Format $fmt -ExpectAudio
            Need ($LASTEXITCODE -eq 0) 'verify_mp4' ([ref]$f)
        }
        Record $name $f
    }
}
if (Want 'd3d12_two_rightfirst') {
    $r = RunCase 'd3d12_two_rightfirst' @('--api', 'd3d12', '--layout', 'two', '--seconds', '5', '--right-first') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    D3d12Checks $r ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) { & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio; Need ($LASTEXITCODE -eq 0) 'verify_mp4 (left half must still be the left eye)' ([ref]$f) }
    Record 'd3d12_two_rightfirst' $f
}
foreach ($c in @(@{ n = 'd3d12_reinit_cpp'; a = @('--cycles', '2'); cfg = $rawCfg },
                 @{ n = 'd3d12_threads_fntable'; a = @('--iface', 'fntable', '--threads', '--pose', 'wait'); cfg = $symCfg })) {
    if (-not (Want $c.n)) { continue }
    $r = RunCase $c.n (@('--api', 'd3d12', '--layout', 'two', '--seconds', '5') + $c.a) $c.cfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed (counts / Submit results / D3D12 debug layer)' ([ref]$f)
    D3d12Checks $r ([ref]$f)
    if ($c.n -like '*reinit*') {
        Need ($r.out -match 'E2E_MP4S=2') 'two MP4s (one per VR session)' ([ref]$f)
        Need (([regex]::Matches($r.log, "capture AVAILABLE on the game's D3D12 device")).Count -eq 2) 'a new D3D12 source per VR session' ([ref]$f)
    } else {
        Need ($r.log -match 'head pose of the recorded frames: WaitGetPoses render pose') 'the WaitGetPoses pose is used' ([ref]$f)
        Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    }
    Need (([regex]::Matches($r.log, 'recording summary: OK')).Count -ge 1) 'recording finalized' ([ref]$f)
    Record $c.n $f
}
# the in-headset dot on a D3D12 game: the overlay texture lives on a private D3D11 device on the game's adapter
if (Want 'd3d12_indicator') {
    $r = RunCase 'd3d12_indicator' @('--api', 'd3d12', '--layout', 'two', '--seconds', '5') ($rawCfg + ';vr_indicator=1;indicator_x=0.30;indicator_y=-0.25;indicator_z=-1.0;indicator_size=0.03;indicator_opacity=1.0')
    $f = @()
    $o = OverlayInfo $r.out
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    D3d12Checks $r ([ref]$f)
    Need ($r.log -match 'indicator: D3D12 game -- the dot''s texture lives on a small private D3D11 device on the same adapter') 'private D3D11 device line' ([ref]$f)
    Need ($null -ne $o) 'no "mock overlay" line' ([ref]$f)
    if ($o) {
        Need ($o.ver -eq 'IVROverlay_027' -and $o.creates -eq 1 -and $o.textures -ge 3 -and $o.texW -eq 64 -and $o.texH -eq 64) "one overlay, a 64x64 texture per state (got $($o.ver), $($o.creates), $($o.textures), $($o.texW)x$($o.texH))" ([ref]$f)
        Need ($o.rgba[0] -lt 80 -and $o.rgba[1] -gt 180 -and $o.rgba[2] -lt 110 -and $o.rgba[3] -gt 240) "the last texture is the green 'saved' dot (centre $($o.rgba -join ','))" ([ref]$f)
        Need ($o.shows -ge 1 -and $o.alphas -ge 20 -and $o.destroys -eq 1 -and $o.bad -eq 0 -and $o.other -eq 0) "shown, pulsed, destroyed once, no bad handle / other slot (shows $($o.shows), alphas $($o.alphas), destroys $($o.destroys), bad $($o.bad), other $($o.other))" ([ref]$f)
    }
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    if ($r.mp4) { & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio; Need ($LASTEXITCODE -eq 0) 'verify_mp4 (the pattern, no dot in the recording)' ([ref]$f) }
    Record 'd3d12_indicator' $f
}

# ---- re-projection through the proxy, D3D11 and D3D12 alike: head smoothing + symmetric_fov, and the VR180 dome ----
foreach ($api in 'd3d11', 'd3d12') {
    $name = "${api}_head_smoothing"
    if (Want $name) {
        $r = RunCase $name @('--api', $api, '--layout', 'two', '--seconds', '5', '--pose', 'wait') ($symCfg -replace 'head_smoothing=0', 'head_smoothing=0.6')
        $f = @()
        Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
        if ($api -eq 'd3d12') { D3d12Checks $r ([ref]$f) }
        Need ($r.log -match 'recorder: head smoothing ON -- head_smoothing 0\.60') 'head smoothing on' ([ref]$f)
        Need ($r.log -match 're-projection summary \(head smoothing ') 're-projection summary' ([ref]$f)
        Need ($r.log -match 'recording summary: OK') 'recording summary OK' ([ref]$f)
        Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
        if ($r.mp4) {
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $r.mp4 -EyeW ($r.symW / 2) -EyeH $r.symH -OutW $r.symW -OutH $r.symH -Fps 90 -Seconds 3.7 -DurationTolerance 0.4 -ExpectAudio -SkipPixels
            Need ($LASTEXITCODE -eq 0) "verify_mp4 at the symmetric size $($r.symW)x$($r.symH)" ([ref]$f)
        }
        Record $name $f
    }
    $name = "${api}_dome_vr180"
    if (Want $name) {
        $r = RunCase $name @('--api', $api, '--layout', 'two', '--seconds', '5', '--pose', 'wait') ($symCfg -replace 'projection=flat', 'projection=vr180')
        $f = @()
        Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
        if ($api -eq 'd3d12') { D3d12Checks $r ([ref]$f) }
        Need ($r.log -match 'recorder: dome vr180: source density') 'dome line' ([ref]$f)
        Need ($r.log -match 'recording summary: OK') 'recording summary OK' ([ref]$f)
        Need ($r.mp4 -and $r.mp4 -match '_VR180\.mp4$') 'a _VR180.mp4' ([ref]$f)
        Record $name $f
    }
}

# ---- live share (the OBS plugin's data path) through the proxy, D3D11 and D3D12: tests/live_share_test.exe (the plugin's
# consumer code) asks for raw eyes on a private block (XR_EYE_CAPTURE_TEST_LIVE_SHM); both eyes must be the test pattern
# bit-exactly, frames at the requested rate, the producer stops when the consumer closes; no recording (no autorecord). ----
$cons = Join-Path $bin 'live_share_test.exe'
foreach ($api in 'd3d11', 'd3d12') {
    $name = "live_$api"
    if (-not (Want $name)) { continue }
    if (-not (Test-Path $cons)) { Write-Host "`n=== ${name}: build first: $cons" -ForegroundColor Red; Record $name @('live_share_test.exe missing'); continue }
    $out = Join-Path $outRoot $name
    if (Test-Path $log) { Remove-Item $log -Force }
    $shm = "Local\OpenXR-EyeCapture-Live-Test-$PID-$api"
    $env:XR_EYE_CAPTURE_TEST_LIVE_SHM = $shm
    $env:XR_EYE_CAPTURE_AUTORECORD = $null
    $env:XR_EYE_CAPTURE_CONFIG = $rawCfg + ';live_share=1'
    Write-Host "`n=== $name" -ForegroundColor Cyan
    $so = Join-Path $b "$name.stdout.txt"
    $p = Start-Process -FilePath $exe -ArgumentList '--api', $api, '--game-dir', "`"$game`"", '--out-dir', "`"$out`"", '--layout', 'two', '--seconds', '14' -PassThru -NoNewWindow -RedirectStandardOutput $so
    $null = $p.Handle
    $co = & $cons --shm $shm --timeout 10 --raw --eye 1920x1080 2>&1 | Out-String
    $cExit = $LASTEXITCODE
    $done = $p.WaitForExit(60000)
    if (-not $done) { Stop-Process -Id $p.Id -Force }
    $env:XR_EYE_CAPTURE_AUTORECORD = '4'
    $env:XR_EYE_CAPTURE_TEST_LIVE_SHM = $null
    $co.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
    $go = if (Test-Path $so) { Get-Content $so -Raw } else { '' }
    $lt = if (Test-Path $log) { Get-Content $log -Raw -Encoding UTF8 } else { '' }
    ($lt -split "`r?`n") | Where-Object { $_ -match 'live share|capture AVAILABLE' } | Select-Object -First 12 | ForEach-Object { Write-Host "  $_" }
    $f = @()
    Need ($cExit -eq 0 -and $co -match 'LIVE_OK') 'consumer LIVE_OK (raw eyes = the pattern, bit-exact)' ([ref]$f)
    Need ($done -and $p.ExitCode -eq 0 -and $go -match 'openvr_e2e PASS') 'the game ran clean (openvr_e2e PASS)' ([ref]$f)
    Need ($lt -match 'live share: consumer found') 'live share: consumer found' ([ref]$f)
    Need ($lt -match 'live share: stopped \(consumer gone') 'live share: stopped (consumer gone ...)' ([ref]$f)
    Need ($lt -notmatch 'record: start') 'no recording (live only)' ([ref]$f)
    if ($api -eq 'd3d12') { Need ($go -match 'E2E_D3D12_DEBUG=(on|unavailable) errors 0') 'D3D12 debug layer errors' ([ref]$f) }
    Record $name $f
}

# ---- IVRCompositor_029 (OpenVR 2.15, e.g. No Man's Sky): Submit moved to slot 6 ----
foreach ($iface in 'cpp', 'fntable') {
    $name = "v029_$iface"
    if (-not (Want $name)) { continue }
    $r = RunCase $name @('--layout', 'two', '--iface', $iface, '--seconds', '4', '--version', '029') $rawCfg
    $f = @()
    Need ($r.code -eq 0) 'openvr_e2e failed' ([ref]$f)
    if ($iface -eq 'cpp') { Need ($r.log -match 'IVRCompositor_029 \(C\+\+ object .*\): vtable .* patched in place -- slot 2 WaitGetPoses, slot 6 Submit') 'vtable patch line (slot 6)' ([ref]$f) }
    else { Need ($r.log -match 'FnTable:IVRCompositor_029 \(FnTable .*\): copied \d+ entries') 'FnTable copy line' ([ref]$f) }
    Need ($r.log -notmatch 'IVRCompositor_029 is a version this proxy does not know') 'still unknown' ([ref]$f)
    Need ($r.log -match 'record: start .*_SBS\.mp4 \| SteamVR D3D11') 'record start line' ([ref]$f)
    Need ($r.log -match 'recording summary: ') 'recording summary (finalized)' ([ref]$f)
    Need ($null -ne $r.mp4) 'no MP4' ([ref]$f)
    Record $name $f
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
if (Want 'mirror_steamvr_dll') {
    # no DLL override: SteamVR's own openvr_api.dll is found through an openvrpaths.vrpath (XR_EYE_CAPTURE_TEST_OPENVRPATHS)
    # whose first runtime is missing and whose second has bin\win64\openvr_api.dll (here: the mock)
    $vrRoot = Join-Path $b 'vrpath'
    $rtOk = Join-Path $vrRoot 'SteamVR'
    New-Item -ItemType Directory -Force (Join-Path $rtOk 'bin\win64') | Out-Null
    Copy-Item $mockDll (Join-Path $rtOk 'bin\win64\openvr_api.dll')
    $vp = Join-Path $vrRoot 'openvrpaths.vrpath'
    $json = '{ "config" : [ "x" ], "runtime" : [ "' + ((Join-Path $vrRoot 'Gone') -replace '\\', '\\') + '", "' + ($rtOk -replace '\\', '\\') + '" ], "version" : 1 }'
    [System.IO.File]::WriteAllText($vp, $json, (New-Object System.Text.UTF8Encoding $false))
    $r = RunMirror 'mirror_steamvr_dll' $quickCfg @{ XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = ''; XR_EYE_CAPTURE_TEST_OPENVRPATHS = $vp; MOCK_OPENVR_QUIT_AFTER_MS = '6000' }
    $f = @()
    Need ($r.log -match [regex]::Escape("mirror: openvr_api.dll = $rtOk\bin\win64\openvr_api.dll")) 'loaded <runtime>\bin\win64\openvr_api.dll of the first EXISTING runtime' ([ref]$f)
    Need ($r.done -and $r.code -eq 0 -and $null -ne $r.mp4) 'recorded through it (exit 0, MP4)' ([ref]$f)
    Record 'mirror_steamvr_dll' $f
}
if (Want 'mirror_no_steamvr') {
    $r = RunMirror 'mirror_no_steamvr' $quickCfg @{ XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = ''; XR_EYE_CAPTURE_TEST_OPENVRPATHS = (Join-Path $b 'no_such.vrpath') } 15
    $f = @()
    Need ($r.done -and $r.code -eq 4) "no SteamVR: exit code 4 (got $($r.code))" ([ref]$f)
    Need ($r.log -match "mirror: SteamVR's openvr_api\.dll not found -- .*no_such\.vrpath not found \(SteamVR not installed, or never started\)\. Quick mode needs SteamVR installed") 'one clear log line' ([ref]$f)
    Record 'mirror_no_steamvr' $f
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
    # a game recorded through the proxy is the SteamVR scene app (MOCK_OPENVR_SCENE_PID): its proxy marker (named mutex) is seen
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
if (Want 'mirror_yields_to_openxr_game') {
    # an OpenXR game with the layer (layer_e2e + the BUILD layer as an explicit layer, the registered one disabled for it) is the
    # scene app: the layer's marker Local\OpenXR-EyeCapture-Layer-<pid> is seen (no process memory is read)
    $layerExe = Join-Path $bin 'layer_e2e.exe'
    $lp = Join-Path $b 'layer_manifest'
    New-Item -ItemType Directory -Force $lp | Out-Null
    $m = Get-Content (Join-Path $bin 'XR_APILAYER_JRDEVO_eye_capture.json') -Raw | ConvertFrom-Json
    $m.api_layer.library_path = (Join-Path $bin 'eye_capture.dll')
    $m | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $lp 'XR_APILAYER_JRDEVO_eye_capture.json') -Encoding ascii
    # DISABLE_XR_APILAYER_DIENERTECH_VECTORXR: keep a foreign implicit OpenXR layer (e.g. DienerTech VectorXR) out of this
    # layer_e2e game's loader chain so it does not break the mock runtime; set and cleared with the other per-child vars below.
    $lenv = @{ DISABLE_XR_APILAYER_JRDEVO_eye_capture = '1'; XR_API_LAYER_PATH = $lp; XR_ENABLE_API_LAYERS = 'XR_APILAYER_JRDEVO_eye_capture'; DISABLE_XR_APILAYER_DIENERTECH_VECTORXR = '1' }
    foreach ($k in $lenv.Keys) { [Environment]::SetEnvironmentVariable($k, $lenv[$k]) }
    $env:XR_EYE_CAPTURE_CONFIG = $rawCfg
    $xrP = Start-Process $layerExe -ArgumentList '--api', 'd3d11', '--layout', 'two', '--seconds', '12', '--record', '0', '--out-dir', "`"$(Join-Path $outRoot 'mirror_openxr_game_game')`"" -PassThru -WindowStyle Hidden
    foreach ($k in $lenv.Keys) { [Environment]::SetEnvironmentVariable($k, $null) }
    $marker = $false
    for ($i = 0; $i -lt 40 -and -not $marker; $i++) {
        Start-Sleep -Milliseconds 250
        $mx = $null
        $marker = [System.Threading.Mutex]::TryOpenExisting("Local\OpenXR-EyeCapture-Layer-$($xrP.Id)", [ref]$mx)
        if ($mx) { $mx.Dispose() }
    }
    $r = RunMirror 'mirror_yields_to_openxr_game' $quickCfg @{ MOCK_OPENVR_QUIT_AFTER_MS = '6000'; MOCK_OPENVR_SCENE_PID = "$($xrP.Id)" }
    if (-not $xrP.WaitForExit(20000)) { Stop-Process -Id $xrP.Id -Force }
    $f = @()
    Need $marker "the layer created Local\OpenXR-EyeCapture-Layer-$($xrP.Id)" ([ref]$f)
    Need ($r.log -match "mirror: idle -- the OpenXR layer in pid $($xrP.Id) records this game") 'idle while the scene app has the OpenXR layer' ([ref]$f)
    Need ($null -eq $r.mp4) 'no mirror MP4' ([ref]$f)
    Record 'mirror_yields_to_openxr_game' $f
}
if (Want 'mirror_scene_without_marker') {
    # the scene app is a process without either marker (this PowerShell): the mirror records
    $r = RunMirror 'mirror_scene_without_marker' $quickCfg @{ MOCK_OPENVR_QUIT_AFTER_MS = '7000'; MOCK_OPENVR_SCENE_PID = "$PID" }
    $f = @()
    Need ($r.log -notmatch 'mirror: idle --') 'not idle' ([ref]$f)
    Need ($null -ne $r.mp4 -and $r.log -match 'recording summary: ') 'the mirror recorded (MP4)' ([ref]$f)
    Record 'mirror_scene_without_marker' $f
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
if (Want 'mirror_tray_no_steamvr') {
    # the mirror finds no SteamVR openvr_api.dll (exit code 4): ONE start, one balloon "Quick mode needs SteamVR installed",
    # no restarts (not the 3-failures loop) until the SteamVR state changes
    $tt = Join-Path $b 'tray'
    if (-not (Test-Path (Join-Path $tt 'EyeCaptureTray.exe'))) {
        New-Item -ItemType Directory -Force $tt | Out-Null
        Copy-Item (Join-Path $bin 'EyeCaptureTray.exe'), $mirrorExe $tt
        Copy-Item (Join-Path $bin 'status_sim.exe') (Join-Path $tt 'fakevrserver.exe')
        [System.IO.File]::WriteAllText((Join-Path $tt 'config.ini'), "[capture]`r`naudio_enabled = 0`r`n[steamvr]`r`nsteamvr_mode = quick`r`n[debug]`r`nlog_level = info`r`n", (New-Object System.Text.UTF8Encoding $false))
    }
    if (Test-Path (Join-Path $tt 'logs\tray.log')) { Remove-Item (Join-Path $tt 'logs\tray.log') }
    $env:XR_EYE_CAPTURE_CONFIG = $quickCfg
    $env:XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = $null
    $env:XR_EYE_CAPTURE_TEST_OPENVRPATHS = Join-Path $b 'no_such.vrpath'
    $env:EYECAP_TRAY_TEST_SHM = "eyecap-mirror-nosvr-$PID"
    $env:EYECAP_TRAY_TEST_VRSERVER = 'fakevrserver.exe'
    $tray = Start-Process (Join-Path $tt 'EyeCaptureTray.exe') -PassThru
    $vr = Start-Process (Join-Path $tt 'fakevrserver.exe') -ArgumentList "Local\EyeCapture-FakeVr4-$PID", '60', 'x.mp4' -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 12
    $tl1 = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8
    Stop-Process -Id $vr.Id
    Start-Sleep -Seconds 3
    $tl = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8
    Stop-Process -Id $tray.Id -ErrorAction SilentlyContinue
    Remove-Item -Path "HKCU:\$testKey" -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Get-ChildItem 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue)) { Remove-Item 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue }
    foreach ($v in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_VRSERVER', 'XR_EYE_CAPTURE_TEST_OPENVRPATHS') { [Environment]::SetEnvironmentVariable($v, $null) }
    ($tl -split "`r?`n") | Where-Object { $_ -match 'quick mode' } | ForEach-Object { Write-Host "  $_" }
    $f = @()
    Need (([regex]::Matches($tl1, 'started .*EyeCaptureMirror\.exe')).Count -eq 1) 'one start only (no relaunch loop)' ([ref]$f)
    Need ($tl1 -match 'EyeCaptureMirror found no SteamVR openvr_api\.dll \(code 4\)' -and ([regex]::Matches($tl1, 'balloon \[SteamVR quick mode\] Quick mode needs SteamVR installed \(openvr_api\.dll not found in the SteamVR folder\)\.')).Count -eq 1) 'one balloon "Quick mode needs SteamVR installed"' ([ref]$f)
    Need ($tl1 -notmatch 'failed 3 times within a minute') 'not the 3-failures back-off' ([ref]$f)
    Need ($tl -match 'the SteamVR state changed -- EyeCaptureMirror may start again') 'unblocked when SteamVR changed' ([ref]$f)
    Record 'mirror_tray_no_steamvr' $f
}
if (Want 'mirror_tray_busy') {
    # another EyeCaptureMirror "runs" (this script holds its single-instance mutex): every start exits with code 3, which is
    # NOT a failure -- no back-off, no balloon, tried again after 10 s; once the mutex is gone the mirror starts and keeps running
    $tt = Join-Path $b 'tray'
    if (-not (Test-Path (Join-Path $tt 'EyeCaptureTray.exe'))) {
        New-Item -ItemType Directory -Force $tt | Out-Null
        Copy-Item (Join-Path $bin 'EyeCaptureTray.exe'), $mirrorExe $tt
        Copy-Item (Join-Path $bin 'status_sim.exe') (Join-Path $tt 'fakevrserver.exe')
        [System.IO.File]::WriteAllText((Join-Path $tt 'config.ini'), "[capture]`r`naudio_enabled = 0`r`n[steamvr]`r`nsteamvr_mode = quick`r`n[debug]`r`nlog_level = info`r`n", (New-Object System.Text.UTF8Encoding $false))
    }
    if (Test-Path (Join-Path $tt 'logs\tray.log')) { Remove-Item (Join-Path $tt 'logs\tray.log') }
    $busyMutex = New-Object System.Threading.Mutex($false, 'Local\OpenXR-EyeCapture-Mirror')
    $env:XR_EYE_CAPTURE_CONFIG = $quickCfg
    $env:XR_EYE_CAPTURE_OUTPUT_DIR = Join-Path $outRoot 'mirror_tray_busy'
    $env:XR_EYE_CAPTURE_MIRROR_OPENVR_DLL = $mockDll
    $env:EYECAP_TRAY_TEST_SHM = "eyecap-mirror-busy-$PID"
    $env:EYECAP_TRAY_TEST_VRSERVER = 'fakevrserver.exe'
    $tray = Start-Process (Join-Path $tt 'EyeCaptureTray.exe') -PassThru
    $vr = Start-Process (Join-Path $tt 'fakevrserver.exe') -ArgumentList "Local\EyeCapture-FakeVr3-$PID", '90', 'x.mp4' -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 16
    $tl1 = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8
    $busyMutex.Dispose()
    $running = $false
    for ($i = 0; $i -lt 30 -and -not $running; $i++) { Start-Sleep -Milliseconds 500; $running = [bool](Get-Process EyeCaptureMirror -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$tt*" }) }
    Start-Sleep -Seconds 3
    $stillRunning = [bool](Get-Process EyeCaptureMirror -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$tt*" })
    Stop-Process -Id $vr.Id
    $stopped = $false
    for ($i = 0; $i -lt 40 -and -not $stopped; $i++) { Start-Sleep -Milliseconds 500; $stopped = -not (Get-Process EyeCaptureMirror -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$tt*" }) }
    $tl = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8
    Stop-Process -Id $tray.Id -ErrorAction SilentlyContinue
    Remove-Item -Path "HKCU:\$testKey" -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Get-ChildItem 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue)) { Remove-Item 'HKCU:\Software\OpenXR-EyeCapture-Test' -ErrorAction SilentlyContinue }
    foreach ($v in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_VRSERVER', 'XR_EYE_CAPTURE_MIRROR_OPENVR_DLL', 'XR_EYE_CAPTURE_OUTPUT_DIR') { [Environment]::SetEnvironmentVariable($v, $null) }
    ($tl -split "`r?`n") | Where-Object { $_ -match 'quick mode' } | ForEach-Object { Write-Host "  $_" }
    $busy = ([regex]::Matches($tl1, 'EyeCaptureMirror exited \(code 3: another EyeCaptureMirror already runs\) -- tried again in 10 s')).Count
    $starts = ([regex]::Matches($tl1, 'started .*EyeCaptureMirror\.exe')).Count
    $f = @()
    Need ($busy -ge 1 -and $starts -ge 2 -and $starts -le 3 -and ($starts - $busy) -le 1) "exit code 3 = tried again after 10 s, not every 2 s ($busy busy exits, $starts starts in 16 s)" ([ref]$f)
    Need ($tl -notmatch 'failed 3 times within a minute' -and $tl -notmatch 'keeps failing') 'exit code 3 never counts toward the back-off (no block, no balloon)' ([ref]$f)
    Need ($running -and $stillRunning) 'once the other instance is gone the mirror starts and keeps running' ([ref]$f)
    Need $stopped 'stopped when "vrserver" exited' ([ref]$f)
    Record 'mirror_tray_busy' $f
}

foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
Write-Host "`n=== run_steamvr summary" -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-String | Write-Host
$bad = @($results | Where-Object { -not $_.ok })
if ($bad.Count) { Write-Host "run_steamvr: $($bad.Count) of $($results.Count) cases FAILED" -ForegroundColor Red; exit 1 }
Write-Host "run_steamvr: all $($results.Count) cases passed" -ForegroundColor Green
exit 0
