<#
    run_x86.ps1 -- 32-bit OpenXR games end to end (docs/X86_DESIGN.md), without a headset: the 32-bit layer
    build32\Release\eye_capture32.dll in the 32-bit tests\layer_e2e.cpp (build32\Release\layer_e2e.exe) through the REAL 32-bit
    OpenXR loader and the 32-bit tests\mock_runtime.cpp, with the 64-bit helper build\Release\EyeCaptureRecorder.exe making
    the recordings. GPU + hardware encoder (like run_e2e.ps1).
      powershell -ExecutionPolicy Bypass -File tests\run_x86.ps1 [-Only <regex>]
    Build first:  cmake -B build32 -A Win32 ... && cmake --build build32 --config Release   (and the normal x64 build)
    Setup (all under build\x86_e2e): the build32 layer is enabled as an EXPLICIT layer (XR_API_LAYER_PATH = a folder with its
    manifest, XR_ENABLE_API_LAYERS); a registered copy is disabled for these processes (DISABLE_XR_APILAYER_JRDEVO_eye_capture,
    and DISABLE_XR_APILAYER_DIENERTECH_VECTORXR for another vendor's layer); APPDATA = an empty scratch folder (no user
    config.ini); XR_EYE_CAPTURE_TEST_X86_SHM = a private block name (a real 32-bit game / helper / tray is never met);
    XR_EYE_CAPTURE_REQUIRE_TRAY=0 and XR_EYE_CAPTURE_TRAY_IPC=0 except where a case says otherwise. Nothing is registered.
    Cases:
      two / array / wide / msaa (D3D11, raw eyes): a valid MP4 made by the helper (verify_mp4.ps1: size, 90 fps, duration,
        st3d, audio, grey steps = gamma, left half = left eye: eye ids / no swap), frames dropped <= 10 %, the indicator quad
        (appended by the 32-bit layer) on at least the recorded frames, the helper exits when the game's session ends.
      head_smoothing: symmetric_fov 1 + head_smoothing 0.6: the helper's re-projection ran on every frame with a usable pose
        (the poses / fovs crossed the process boundary), the MP4 is still pixel-exact (the mock does not move).
      dome_vr180: projection = vr180 -> a _VR180.mp4 + sidecar, dome painted from the poses (0 frames without pose).
      hotkey: the control thread runs in the 32-bit game (private status block XR_EYE_CAPTURE_TEST_SHM, hotkey = F24, a key
        no program uses): F24 pressed twice (keybd_event) = start / stop, one MP4 of the time in between.
      tray_command: the same through the tray's command path (ECC_TOGGLE written into the private status block like
        EyeCaptureTray.exe does): state READY -> RECORDING -> READY, api "D3D11 (32-bit)", one MP4.
      no_helper: no EyeCaptureRecorder: the start fails after 8 s with a clear log line, the game's frame loop is unaffected.
      helper_killed: the helper is killed during a recording: the game runs on, the recording is reported lost.
      game_killed: the game is killed during a recording: the helper finalizes a valid MP4, exits 0 and hands "saved <file>"
        to the (private) tray status block.
      second_game: two 32-bit games at once: one is recorded, the other logs why it is not, both run normally.
      tray: a scratch test-mode tray (EYECAP_TRAY_TEST_SHM) with EyeCaptureRecorder.exe next to it starts the helper when the
        game's layer creates its block (logged), the recording is made, the helper exits with the game (code 0). The game
        publishes its status to the tray's private block (TRAY_IPC=1): the idle tray has no timer and wakes on that.
      address_space: the 32-bit game's used address space with the layer recording vs without the layer (informational,
        limit 64 MB).
      dormant (no tray, no override): no block, nothing recorded -- skipped while an EyeCaptureTray runs.
    Skips (exit 0 with a message) while amtrucks / eurotrucks2 / obs64 runs.
#>
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$bin32 = Join-Path $root 'build32\Release'
$exe = Join-Path $bin32 'layer_e2e.exe'
$helperExe = Join-Path $bin 'EyeCaptureRecorder.exe'
foreach ($f in @($exe, (Join-Path $bin32 'eye_capture32.dll'), (Join-Path $bin32 'mock_runtime.dll'), $helperExe, (Join-Path $bin 'EyeCaptureTray.exe'))) {
    if (-not (Test-Path $f)) { throw "build first: $f" }
}
$busy = Get-Process amtrucks, eurotrucks2, obs64 -ErrorAction SilentlyContinue
if ($busy) {
    Write-Host ("run_x86: skipped -- {0} is running (GPU tests would disturb it)" -f (($busy | ForEach-Object Name) -join ', ')) -ForegroundColor Yellow
    exit 0
}

$b = Join-Path $root 'build\x86_e2e'
if (Test-Path $b) { Remove-Item -Recurse -Force $b }
$lp = Join-Path $b 'layer'; $home2 = Join-Path $b 'home'; $appdata = Join-Path $b 'appdata'; $outRoot = Join-Path $b 'out'
New-Item -ItemType Directory -Force $lp, $home2, $appdata, $outRoot | Out-Null
$m = Get-Content (Join-Path $bin32 'XR_APILAYER_JRDEVO_eye_capture32.json') -Raw | ConvertFrom-Json
$m.api_layer.library_path = (Join-Path $bin32 'eye_capture32.dll')
$m | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $lp 'XR_APILAYER_JRDEVO_eye_capture32.json') -Encoding ascii
$layerLog = Join-Path $bin32 'logs\eye_capture.log'
$helperLog = Join-Path $home2 'logs\eye_capture.log'

$envNames = 'APPDATA', 'DISABLE_XR_APILAYER_JRDEVO_eye_capture', 'DISABLE_XR_APILAYER_DIENERTECH_VECTORXR', 'XR_API_LAYER_PATH',
            'XR_ENABLE_API_LAYERS', 'XR_EYE_CAPTURE_REQUIRE_TRAY', 'XR_EYE_CAPTURE_TRAY_IPC', 'XR_EYE_CAPTURE_TEST_X86_SHM',
            'XR_EYE_CAPTURE_CONFIG', 'XR_EYE_CAPTURE_TEST_SHM', 'EYECAP_TRAY_TEST_SHM', 'XR_EYE_CAPTURE_OUTPUT_DIR', 'XR_EYE_CAPTURE_AUTORECORD'
$saved = @{}
foreach ($n in $envNames) { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
$env:APPDATA = $appdata
$env:DISABLE_XR_APILAYER_JRDEVO_eye_capture = '1'
$env:DISABLE_XR_APILAYER_DIENERTECH_VECTORXR = '1'
$env:XR_API_LAYER_PATH = $lp
$env:XR_ENABLE_API_LAYERS = 'XR_APILAYER_JRDEVO_eye_capture'
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
$env:XR_EYE_CAPTURE_TRAY_IPC = '0'
$env:XR_EYE_CAPTURE_TEST_X86_SHM = "Local\EyeCapX86Test-$PID"
$flatCfg = 'projection=flat;symmetric_fov=0;head_smoothing=0;record_for_youtube=0;max_width=0;max_fps=0;codec=hevc;swap_eyes=0;gamma=auto'
$env:XR_EYE_CAPTURE_CONFIG = $flatCfg

$results = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
function Want($name) { return (-not $Only) -or ($name -match $Only) }
function Need($ok, $what, [ref]$fails) { if (-not $ok) { $fails.Value += $what; Write-Host "  FAIL $what" -ForegroundColor Red } else { Write-Host "  ok   $what" -ForegroundColor Green } }
function Record($name, $fails) {
    $ok = $fails.Count -eq 0
    Write-Host ("  -> {0}" -f $(if ($ok) { 'ok' } else { 'FAIL: ' + ($fails -join '; ') })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
    $results.Add([pscustomobject]@{ case = $name; ok = $ok; why = ($fails -join '; ') })
}
function LogPos($p) { if (Test-Path $p) { return (Get-Item $p).Length } else { return 0 } }
function LogSince($p, $pos) {
    if (-not (Test-Path $p)) { return '' }
    $fs = [System.IO.File]::Open($p, 'Open', 'Read', 'ReadWrite')
    try {
        if ($pos -gt $fs.Length) { $pos = 0 }   # rotated
        $null = $fs.Seek($pos, 'Begin')
        return (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd()
    } finally { $fs.Dispose() }
}
function StartHelper() { return Start-Process $helperExe -ArgumentList '--home', "`"$home2`"" -PassThru -WindowStyle Hidden }
function WaitExit($p, $sec) { if ($null -eq $p) { return $true }; if (-not $p.WaitForExit($sec * 1000)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; return $false }; return $true }
# a game started in the background (stdout to a file)
function StartGame($name, [string[]]$gameArgs) {
    $out = Join-Path $outRoot $name
    New-Item -ItemType Directory -Force $out | Out-Null
    $so = Join-Path $b "$name.stdout.txt"
    $p = Start-Process $exe -ArgumentList ($gameArgs + @('--out-dir', "`"$out`"")) -PassThru -WindowStyle Hidden -RedirectStandardOutput $so -RedirectStandardError (Join-Path $b "$name.stderr.txt")
    return @{ p = $p; so = $so; out = $out }
}
function GameText($g) { if (Test-Path $g.so) { return [string](Get-Content $g.so -Raw) } else { return '' } }
function RunGame($name, [string[]]$gameArgs) {
    $out = Join-Path $outRoot $name
    $o = & $exe @gameArgs --out-dir $out 2>&1 | Out-String
    $code = $LASTEXITCODE
    $o.Trim().Split("`n") | Where-Object { $_ -match 'E2E_|mock runtime\] (session|quad)|FAIL' } | ForEach-Object { Write-Host "  $($_.TrimEnd())" }
    $mm = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
    return @{ out = $o; code = $code; dir = $out; mp4 = $(if ($mm.Success) { $mm.Groups[1].Value.Trim() } else { $null }) }
}
function QuadInfo($o) {
    $q = [regex]::Match($o, 'quad layers: valid on (\d+) frames, invalid (\d+)')
    $f = [regex]::Match($o, 'session destroyed: (\d+) frames')
    return @{ ok = $q.Success; valid = [int]$q.Groups[1].Value; bad = [int]$q.Groups[2].Value; total = [int]$f.Groups[1].Value }
}
function Verify($mp4, [string[]]$extra) {
    $vo = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $mp4 @extra 2>&1 | Out-String
    $vo.Trim().Split("`n") | ForEach-Object { Write-Host "    $($_.TrimEnd())" }
    return ($LASTEXITCODE -eq 0)
}
function Written($logTxt) {
    $w = [regex]::Match([string]$logTxt, 'frames written (\d+), dropped (\d+)')
    if (-not $w.Success) { return @{ ok = $false; n = 0; d = 0 } }
    return @{ ok = $true; n = [int]$w.Groups[1].Value; d = [int]$w.Groups[2].Value }
}

Add-Type -Namespace EyeCapX86Test -Name Kbd -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
'@
function PressF24() {
    [EyeCapX86Test.Kbd]::keybd_event(0x87, 0, 0, [UIntPtr]::Zero)   # VK_F24 down
    Start-Sleep -Milliseconds 200                                     # the control thread polls every 20 ms
    [EyeCapX86Test.Kbd]::keybd_event(0x87, 0, 2, [UIntPtr]::Zero)   # KEYEVENTF_KEYUP
}
# The tray's side of the status block (src/shared_status.h EyeCapShared; offsets pinned by static_asserts there)
function ShmRead($name) {
    try { $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting($name) } catch { return $null }
    $a = $mmf.CreateViewAccessor(0, 2832)
    $chars = New-Object char[] 160
    [void]$a.ReadArray(2496, $chars, 0, 160)
    $api = New-Object char[] 16
    [void]$a.ReadArray(192, $api, 0, 16)
    $r = @{ owner = $a.ReadUInt32(12); state = $a.ReadUInt32(16); message = (-join $chars).Split([char]0)[0]; api = (-join $api).Split([char]0)[0] }
    $a.Dispose(); $mmf.Dispose()
    return $r
}
function ShmToggle($name) {   # = EyeCapSendCommand(shm, ECC_TOGGLE)
    $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting($name)
    $a = $mmf.CreateViewAccessor(0, 2832)
    $a.Write(2820, [uint32]$a.ReadUInt32(12))   # cmdTargetPid = ownerPid
    $a.Write(2824, [uint32]1)                   # ECC_TOGGLE
    $a.Write(2816, [int32]($a.ReadInt32(2816) + 1))
    $a.Dispose(); $mmf.Dispose()
    $ev = [System.Threading.EventWaitHandle]::OpenExisting("$name-Command"); [void]$ev.Set(); $ev.Dispose()
}

# ---- layouts (raw eyes) ----
foreach ($layout in 'two', 'array', 'wide', 'msaa') {
    if (-not (Want $layout)) { continue }
    Write-Host "`n=== $layout (D3D11, 32-bit game, raw eyes)" -ForegroundColor Cyan
    $lp0 = LogPos $layerLog; $hp0 = LogPos $helperLog
    $h = StartHelper
    $r = RunGame $layout @('--api', 'd3d11', '--layout', $layout, '--seconds', '6', '--record', '4')
    $hOk = WaitExit $h 15
    $lt = LogSince $layerLog $lp0; $ht = LogSince $helperLog $hp0
    $f = @()
    Need ($r.code -eq 0 -and $r.mp4) 'game ran, an MP4 appeared' ([ref]$f)
    $w = Written (Get-Content (Join-Path $r.dir 'eye_capture.log') -Raw -ErrorAction SilentlyContinue)
    $fps = if ($w.ok -and ($w.n + $w.d) -gt 0) { 90.0 * $w.n / ($w.n + $w.d) } else { 90.0 }
    Need ($w.ok -and $w.n -gt 0 -and $w.d -le 0.10 * ($w.n + $w.d)) "frames written $($w.n), dropped $($w.d) (limit 10 %)" ([ref]$f)
    if ($r.mp4) { Need (Verify $r.mp4 @('-EyeW', '1920', '-EyeH', '1080', '-Fps', "$fps", '-Seconds', '3.7', '-DurationTolerance', '0.35', '-ExpectAudio')) 'verify_mp4 (eye ids, gamma, st3d, audio)' ([ref]$f) }
    $q = QuadInfo $r.out
    Need ($q.ok -and $q.bad -eq 0 -and $q.valid -ge $w.n -and $q.valid -lt $q.total -and $lt -match 'indicator: quad layer appended AFTER the capture') "indicator quad valid on $($q.valid) of $($q.total) frames (recorded $($w.n))" ([ref]$f)
    Need ($lt -match 'loaded into .*layer_e2e\.exe' -and $lt -match '32-bit: recordings are made by EyeCaptureRecorder\.exe') 'the 32-bit layer loaded (banner)' ([ref]$f)
    Need ($lt -match 'recorder: RECORDING -> .* \(32-bit game, encoded by EyeCaptureRecorder\.exe pid \d+\)') 'layer: RECORDING through the helper' ([ref]$f)
    Need ($ht -match 'recording summary: OK' -and $ht -match 'helper: recording #\d+: (\d+) frames taken from the game') 'helper: recording summary OK' ([ref]$f)
    Need ($hOk -and $h.ExitCode -eq 0 -and $ht -match "helper: finishing -- the game's VR session ended") "helper exited 0 after the session ended (exit $($h.ExitCode))" ([ref]$f)
    $va = [regex]::Match($r.out, 'E2E_ADDRESS_SPACE max used ([\d.]+) MB')
    if ($va.Success -and $layout -eq 'two') { $script:vaRec = [double]$va.Groups[1].Value }
    Record $layout $f
}

# ---- head smoothing + symmetric fov: poses and fovs reach the helper's re-projection ----
if (Want 'head_smoothing') {
    Write-Host "`n=== head_smoothing (symmetric_fov 1, head_smoothing 0.6)" -ForegroundColor Cyan
    $env:XR_EYE_CAPTURE_CONFIG = 'projection=flat;symmetric_fov=1;head_smoothing=0.6;record_for_youtube=0;max_width=0;max_fps=0;codec=hevc;swap_eyes=0;gamma=auto'
    $hp0 = LogPos $helperLog
    $h = StartHelper
    $r = RunGame 'head_smoothing' @('--api', 'd3d11', '--layout', 'two', '--seconds', '6', '--record', '4')
    $hOk = WaitExit $h 15
    $env:XR_EYE_CAPTURE_CONFIG = $flatCfg
    $ht = LogSince $helperLog $hp0
    $f = @()
    Need ($r.code -eq 0 -and $r.mp4) 'an MP4 appeared' ([ref]$f)
    $s = [regex]::Match($ht, 're-projection summary \(head smoothing on, symmetric fov on\): (\d+) frames .*no usable pose in (\d+) frames')
    Need ($s.Success -and [int]$s.Groups[1].Value -gt 100 -and [int]$s.Groups[2].Value -eq 0) "helper: re-projection on $($s.Groups[1].Value) frames, $($s.Groups[2].Value) without a usable pose" ([ref]$f)
    if ($r.mp4) { Need (Verify $r.mp4 @('-EyeW', '1920', '-EyeH', '1080', '-Fps', '90', '-Seconds', '3.7', '-DurationTolerance', '0.35')) 'verify_mp4' ([ref]$f) }
    Need ($hOk -and $h.ExitCode -eq 0) 'helper exited 0' ([ref]$f)
    Record 'head_smoothing' $f
}

# ---- VR180 dome ----
if (Want 'dome_vr180') {
    Write-Host "`n=== dome_vr180 (projection = vr180)" -ForegroundColor Cyan
    $env:XR_EYE_CAPTURE_CONFIG = 'projection=vr180;record_for_youtube=0;max_width=0;max_fps=0;codec=hevc;swap_eyes=0;gamma=auto'
    $hp0 = LogPos $helperLog
    $h = StartHelper
    $r = RunGame 'dome_vr180' @('--api', 'd3d11', '--layout', 'two', '--seconds', '6', '--record', '4')
    $hOk = WaitExit $h 15
    $env:XR_EYE_CAPTURE_CONFIG = $flatCfg
    $ht = LogSince $helperLog $hp0
    $f = @()
    Need ($r.code -eq 0 -and $r.mp4 -and $r.mp4 -match '_VR180\.mp4$') 'a _VR180.mp4 appeared' ([ref]$f)
    Need ($r.mp4 -and (Test-Path ($r.mp4 -replace '\.mp4$', '.json'))) 'its sidecar json' ([ref]$f)
    $s = [regex]::Match($ht, 're-projection summary \(dome vr180[^)]*\): (\d+) frames \| dome: (\d+) frames painted, (\d+) frames without pose')
    Need ($s.Success -and [int]$s.Groups[2].Value -gt 100 -and [int]$s.Groups[3].Value -eq 0) "helper: dome painted $($s.Groups[2].Value) frames, $($s.Groups[3].Value) without pose" ([ref]$f)
    Need ($ht -match 'recording summary: OK') 'recording summary OK' ([ref]$f)
    Need ($hOk -and $h.ExitCode -eq 0) 'helper exited 0' ([ref]$f)
    Record 'dome_vr180' $f
}

# ---- the hotkey (control thread in the 32-bit game) ----
if (Want 'hotkey') {
    Write-Host "`n=== hotkey (F24 twice: start / stop)" -ForegroundColor Cyan
    $shm = "Local\EyeCapX86Status-$PID-hk"
    $env:XR_EYE_CAPTURE_TRAY_IPC = '1'
    $env:XR_EYE_CAPTURE_TEST_SHM = $shm
    $env:XR_EYE_CAPTURE_CONFIG = $flatCfg + ';hotkey=F24;beeps=0'
    $lp0 = LogPos $layerLog; $hp0 = LogPos $helperLog
    $h = StartHelper
    $g = StartGame 'hotkey' @('--api', 'd3d11', '--layout', 'two', '--seconds', '14', '--record', '0', '--no-output')
    Start-Sleep -Seconds 3
    PressF24
    Start-Sleep -Seconds 6
    PressF24
    $gOk = WaitExit $g.p 30
    $hOk = WaitExit $h 15
    $env:XR_EYE_CAPTURE_TRAY_IPC = '0'; $env:XR_EYE_CAPTURE_TEST_SHM = $null; $env:XR_EYE_CAPTURE_CONFIG = $flatCfg
    $lt = LogSince $layerLog $lp0; $ht = LogSince $helperLog $hp0
    $go = GameText $g
    $f = @()
    Need ($gOk -and $go -match 'E2E_LOOP_OK frames=1260') 'the game loop completed (1260 frames)' ([ref]$f)
    Need (([regex]::Matches($lt, 'control: hotkey F24 pressed')).Count -eq 2) 'layer: hotkey F24 pressed twice' ([ref]$f)
    Need ($lt -match 'control: start requested' -and $lt -match 'control: stop requested') 'layer: start / stop requested' ([ref]$f)
    $mp4 = @(Get-ChildItem $g.out -Filter *_SBS.mp4 -ErrorAction SilentlyContinue)
    Need ($mp4.Count -eq 1) "one MP4 ($($mp4.Count))" ([ref]$f)
    if ($mp4.Count -eq 1) { Need (Verify $mp4[0].FullName @('-EyeW', '1920', '-EyeH', '1080', '-Fps', '90', '-Seconds', '5.5', '-DurationTolerance', '0.8', '-ExpectAudio')) 'verify_mp4 (~5.5 s between the presses)' ([ref]$f) }
    Need ($hOk -and $h.ExitCode -eq 0 -and $ht -match 'recording summary: OK') 'helper: summary OK, exit 0' ([ref]$f)
    Record 'hotkey' $f
}

# ---- the tray's command path (status block) ----
if (Want 'tray_command') {
    Write-Host "`n=== tray_command (ECC_TOGGLE through the status block, state / api published by the 32-bit game)" -ForegroundColor Cyan
    $shm = "Local\EyeCapX86Status-$PID-cmd"
    $env:XR_EYE_CAPTURE_TRAY_IPC = '1'
    $env:XR_EYE_CAPTURE_TEST_SHM = $shm
    $env:XR_EYE_CAPTURE_CONFIG = $flatCfg + ';beeps=0'
    $hp0 = LogPos $helperLog
    $h = StartHelper
    $g = StartGame 'tray_command' @('--api', 'd3d11', '--layout', 'two', '--seconds', '14', '--record', '0', '--no-output')
    Start-Sleep -Seconds 3
    $s0 = ShmRead $shm
    ShmToggle $shm
    Start-Sleep -Seconds 3
    $s1 = ShmRead $shm
    Start-Sleep -Seconds 2
    ShmToggle $shm
    Start-Sleep -Seconds 3
    $s2 = ShmRead $shm
    $gOk = WaitExit $g.p 30
    $hOk = WaitExit $h 15
    $env:XR_EYE_CAPTURE_TRAY_IPC = '0'; $env:XR_EYE_CAPTURE_TEST_SHM = $null; $env:XR_EYE_CAPTURE_CONFIG = $flatCfg
    $ht = LogSince $helperLog $hp0
    $f = @()
    Need ($null -ne $s0 -and $s0.owner -eq $g.p.Id -and $s0.state -eq 1 -and $s0.api -eq 'D3D11 (32-bit)') "before: owner = the game, state READY, api '$($s0.api)'" ([ref]$f)
    Need ($null -ne $s1 -and $s1.state -eq 3) "after the first toggle: RECORDING (state $($s1.state))" ([ref]$f)
    Need ($null -ne $s2 -and $s2.state -eq 1 -and $s2.message -match '^saved .*_SBS\.mp4') "after the second toggle: READY, message '$($s2.message)'" ([ref]$f)
    $mp4 = @(Get-ChildItem $g.out -Filter *_SBS.mp4 -ErrorAction SilentlyContinue)
    Need ($gOk -and $mp4.Count -eq 1) "game loop completed, one MP4 ($($mp4.Count))" ([ref]$f)
    if ($mp4.Count -eq 1) { Need (Verify $mp4[0].FullName @('-EyeW', '1920', '-EyeH', '1080', '-Fps', '90', '-Seconds', '4.6', '-DurationTolerance', '1.0')) 'verify_mp4' ([ref]$f) }
    Need ($hOk -and $h.ExitCode -eq 0 -and $ht -match 'recording summary: OK') 'helper: summary OK, exit 0' ([ref]$f)
    Record 'tray_command' $f
}

# ---- no helper: the start fails cleanly, the game is unaffected ----
if (Want 'no_helper') {
    Write-Host "`n=== no_helper (no EyeCaptureRecorder)" -ForegroundColor Cyan
    $lp0 = LogPos $layerLog
    $r = RunGame 'no_helper' @('--api', 'd3d11', '--layout', 'two', '--seconds', '12', '--record', '10', '--no-output')   # the start waits 8 s for a helper
    $lt = LogSince $layerLog $lp0
    $ef = [regex]::Match($r.out, 'E2E_ENDFRAME max ([\d.]+) ms')
    $f = @()
    Need ($r.code -eq 0 -and $r.out -match 'E2E_LOOP_OK frames=1080' -and $r.out -notmatch 'an MP4 appeared') 'game loop completed (1080 frames), no MP4' ([ref]$f)
    Need ($lt -match 'not started: EyeCaptureRecorder\.exe \(the 64-bit recording helper for 32-bit games\) is not running') 'layer: clear "not running" line' ([ref]$f)
    Need ($ef.Success -and [double]$ef.Groups[1].Value -lt 30) "xrEndFrame max $($ef.Groups[1].Value) ms (the wait is not on the game thread)" ([ref]$f)
    Record 'no_helper' $f
}

# ---- the helper is killed during a recording ----
if (Want 'helper_killed') {
    Write-Host "`n=== helper_killed (Stop-Process -Force during a recording)" -ForegroundColor Cyan
    $lp0 = LogPos $layerLog
    $h = StartHelper
    $g = StartGame 'helper_killed' @('--api', 'd3d11', '--layout', 'two', '--seconds', '10', '--record', '8', '--no-output')
    Start-Sleep -Seconds 4
    $wasRunning = -not $h.HasExited
    Stop-Process -Id $h.Id -Force -ErrorAction SilentlyContinue
    $gOk = WaitExit $g.p 30
    $lt = LogSince $layerLog $lp0
    $go = GameText $g
    $ef = [regex]::Match($go, 'E2E_ENDFRAME max ([\d.]+) ms')
    $f = @()
    Need $wasRunning 'the helper was running when it was killed' ([ref]$f)
    Need ($gOk -and $go -match 'E2E_LOOP_OK frames=900') 'the game ran on to the end (900 frames)' ([ref]$f)
    Need ($lt -match 'recording lost: EyeCaptureRecorder\.exe \(pid \d+\) stopped during the recording') 'layer: "recording lost" line' ([ref]$f)
    Need ($ef.Success -and [double]$ef.Groups[1].Value -lt 30) "xrEndFrame max $($ef.Groups[1].Value) ms" ([ref]$f)
    Record 'helper_killed' $f
}

# ---- the game is killed during a recording: the helper finalizes ----
if (Want 'game_killed') {
    Write-Host "`n=== game_killed (Stop-Process -Force on the game during a recording)" -ForegroundColor Cyan
    $shm = "Local\EyeCapX86Status-$PID-kill"
    $env:XR_EYE_CAPTURE_TRAY_IPC = '1'
    $env:XR_EYE_CAPTURE_TEST_SHM = $shm
    $env:XR_EYE_CAPTURE_CONFIG = $flatCfg + ';beeps=0'
    $hp0 = LogPos $helperLog
    # the status block outlives the game only while someone holds it: this script keeps it open, like the tray
    $keep = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateOrOpen($shm, 2832)
    $h = StartHelper
    $g = StartGame 'game_killed' @('--api', 'd3d11', '--layout', 'two', '--seconds', '30', '--record', '25', '--no-output')
    Start-Sleep -Seconds 6
    Stop-Process -Id $g.p.Id -Force
    $hOk = WaitExit $h 40
    $st = ShmRead $shm
    $keep.Dispose()
    $env:XR_EYE_CAPTURE_TRAY_IPC = '0'; $env:XR_EYE_CAPTURE_TEST_SHM = $null; $env:XR_EYE_CAPTURE_CONFIG = $flatCfg
    $ht = LogSince $helperLog $hp0
    $f = @()
    Need ($hOk -and $h.ExitCode -eq 0) "helper exited 0 (exit $($h.ExitCode))" ([ref]$f)
    Need ($ht -match 'helper: finishing -- the game exited' -and $ht -match 'helper: finalizing the recording' -and $ht -match 'recording summary: OK') 'helper: game exited -> finalized, summary OK' ([ref]$f)
    $mp4 = @(Get-ChildItem (Join-Path $outRoot 'game_killed') -Filter *_SBS.mp4 -ErrorAction SilentlyContinue)
    Need ($mp4.Count -eq 1) "one MP4 ($($mp4.Count))" ([ref]$f)
    if ($mp4.Count -eq 1) { Need (Verify $mp4[0].FullName @('-EyeW', '1920', '-EyeH', '1080', '-Fps', '90', '-Seconds', '5.0', '-DurationTolerance', '1.5', '-ExpectAudio')) 'verify_mp4 (a valid, finalized file)' ([ref]$f) }
    Need ($null -ne $st -and $st.message -match '^saved .*_SBS\.mp4' -and $ht -match 'helper: published the final status to the tray') "tray status block: '$($st.message)'" ([ref]$f)
    Record 'game_killed' $f
}

# ---- two 32-bit games at once ----
if (Want 'second_game') {
    Write-Host "`n=== second_game (two 32-bit games at once)" -ForegroundColor Cyan
    $lp0 = LogPos $layerLog
    $h = StartHelper
    $g1 = StartGame 'second_game_1' @('--api', 'd3d11', '--layout', 'two', '--seconds', '8', '--record', '4', '--no-output')
    Start-Sleep -Milliseconds 1500
    $g2 = StartGame 'second_game_2' @('--api', 'd3d11', '--layout', 'two', '--seconds', '6', '--record', '3', '--no-output')
    $ok1 = WaitExit $g1.p 30; $ok2 = WaitExit $g2.p 30
    $hOk = WaitExit $h 15
    $lt = LogSince $layerLog $lp0
    $m1 = @(Get-ChildItem $g1.out -Filter *.mp4 -ErrorAction SilentlyContinue); $m2 = @(Get-ChildItem $g2.out -Filter *.mp4 -ErrorAction SilentlyContinue)
    $f = @()
    Need ($ok1 -and $ok2 -and (GameText $g1) -match 'E2E_LOOP_OK' -and (GameText $g2) -match 'E2E_LOOP_OK') 'both games ran to the end' ([ref]$f)
    Need ($m1.Count -eq 1 -and $m2.Count -eq 0) "the first game recorded ($($m1.Count) MP4), the second not ($($m2.Count))" ([ref]$f)
    Need ($lt -match "another 32-bit game \(pid $($g1.p.Id)\) is already recorded" -and $lt -match 'not started: another 32-bit game') 'the second one logs why' ([ref]$f)
    Need ($hOk -and $h.ExitCode -eq 0) 'helper exited 0' ([ref]$f)
    Record 'second_game' $f
}

# ---- the tray starts the helper ----
if (Want 'tray') {
    Write-Host "`n=== tray (a test-mode tray starts EyeCaptureRecorder when the 32-bit game's block appears)" -ForegroundColor Cyan
    $tt = Join-Path $b 'tray'
    New-Item -ItemType Directory -Force $tt | Out-Null
    Copy-Item (Join-Path $bin 'EyeCaptureTray.exe'), $helperExe $tt
    [System.IO.File]::WriteAllText((Join-Path $tt 'config.ini'), "[capture]`r`naudio_enabled = 0`r`n[debug]`r`nlog_level = info`r`n", (New-Object System.Text.UTF8Encoding $false))
    $env:EYECAP_TRAY_TEST_SHM = "eyecap-x86-tray-$PID"
    $tray = Start-Process (Join-Path $tt 'EyeCaptureTray.exe') -PassThru
    Start-Sleep -Seconds 2
    # like a real game: the layer publishes its status to the tray's block -- an idle tray has no timer and wakes on that
    $env:XR_EYE_CAPTURE_TRAY_IPC = '1'; $env:XR_EYE_CAPTURE_TEST_SHM = $env:EYECAP_TRAY_TEST_SHM
    $r = RunGame 'tray' @('--api', 'd3d11', '--layout', 'two', '--seconds', '10', '--record', '4')
    $env:XR_EYE_CAPTURE_TRAY_IPC = '0'; $env:XR_EYE_CAPTURE_TEST_SHM = $null
    $hExit = $false
    for ($i = 0; $i -lt 30 -and -not $hExit; $i++) { Start-Sleep -Milliseconds 500; $hExit = -not (Get-Process EyeCaptureRecorder -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$tt*" }) }
    Start-Sleep -Seconds 3   # the tray's next 2 s tick reaps the helper
    Stop-Process -Id $tray.Id -ErrorAction SilentlyContinue
    $env:EYECAP_TRAY_TEST_SHM = $null
    $tl = Get-Content (Join-Path $tt 'logs\tray.log') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
    $hl = Get-Content (Join-Path $tt 'logs\eye_capture.log') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
    ([string]$tl -split "`r?`n") | Where-Object { $_ -match '32-bit games' } | ForEach-Object { Write-Host "  $_" }
    $f = @()
    Need ([string]$tl -match 'tray: 32-bit games: layer_e2e \(pid \d+\) is a 32-bit OpenXR game' -and [string]$tl -match 'tray: 32-bit games: started .*EyeCaptureRecorder\.exe \(pid \d+\) for layer_e2e') 'the tray started EyeCaptureRecorder for the game (logged)' ([ref]$f)
    Need ($r.code -eq 0 -and $r.mp4) 'the recording was made' ([ref]$f)
    Need ($hExit -and [string]$tl -match 'EyeCaptureRecorder \(pid \d+\) exited \(code 0\)') 'the helper exited with the game (code 0, reaped by the tray)' ([ref]$f)
    Need ([string]$hl -match 'helper: serving layer_e2e') 'helper log in the tray folder' ([ref]$f)
    Record 'tray' $f
}

# ---- used address space of the 32-bit game (layer + recording vs no layer) ----
if (Want 'address_space') {
    Write-Host "`n=== address_space (32-bit game: with the layer recording vs without the layer)" -ForegroundColor Cyan
    $saveEnable = $env:XR_ENABLE_API_LAYERS
    $env:XR_ENABLE_API_LAYERS = $null
    $r0 = RunGame 'address_space_nolayer' @('--api', 'd3d11', '--layout', 'two', '--seconds', '4', '--record', '0', '--no-output')
    $env:XR_ENABLE_API_LAYERS = $saveEnable
    if (-not $script:vaRec) {
        $h = StartHelper
        $r1 = RunGame 'address_space_layer' @('--api', 'd3d11', '--layout', 'two', '--seconds', '6', '--record', '4')
        WaitExit $h 15 | Out-Null
        $va1 = [regex]::Match($r1.out, 'E2E_ADDRESS_SPACE max used ([\d.]+) MB')
        if ($va1.Success) { $script:vaRec = [double]$va1.Groups[1].Value }
    }
    $va0 = [regex]::Match($r0.out, 'E2E_ADDRESS_SPACE max used ([\d.]+) MB of ([\d.]+) MB \((\d+)-bit')
    $f = @()
    Need ($va0.Success -and $va0.Groups[3].Value -eq '32' -and $script:vaRec) 'both runs reported their address space (32-bit process)' ([ref]$f)
    if ($va0.Success -and $script:vaRec) {
        $d = $script:vaRec - [double]$va0.Groups[1].Value
        $line = "32-bit game address space: {0:N1} MB without the layer, {1:N1} MB with the layer recording (+{2:N1} MB) of {3} MB" -f [double]$va0.Groups[1].Value, $script:vaRec, $d, $va0.Groups[2].Value
        Write-Host "  $line"
        $notes.Add($line)
        Need ($d -lt 64) "the layer adds less than 64 MB of address space (+$([math]::Round($d, 1)) MB)" ([ref]$f)
    }
    Record 'address_space' $f
}

# ---- dormant: no tray, no override -> no block, nothing recorded ----
if (Want 'dormant') {
    Write-Host "`n=== dormant (no tray, no override)" -ForegroundColor Cyan
    $trayUp = $true
    try { $mx = [System.Threading.Mutex]::OpenExisting('Local\OpenXR-EyeCapture-TrayAlive'); $mx.Dispose() } catch { $trayUp = $false }
    if ($trayUp) {
        Write-Host '  SKIP: an EyeCaptureTray is running (it holds Local\OpenXR-EyeCapture-TrayAlive)' -ForegroundColor Yellow
        $notes.Add('dormant: skipped (an EyeCaptureTray was running)')
    } else {
        $env:XR_EYE_CAPTURE_REQUIRE_TRAY = $null
        $lp0 = LogPos $layerLog
        $r = RunGame 'dormant' @('--api', 'd3d11', '--layout', 'two', '--seconds', '3', '--record', '2', '--expect-dormant')
        $env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
        $lt = LogSince $layerLog $lp0
        $q = QuadInfo $r.out
        $f = @()
        Need ($r.code -eq 0 -and $r.out -match 'E2E_DORMANT_OK') 'loop completed, nothing recorded' ([ref]$f)
        Need ($lt -match 'layer dormant for this session' -and $lt -notmatch '32-bit recorder block' -and $q.valid -eq 0) 'dormant: no block, no indicator' ([ref]$f)
        Record 'dormant' $f
    }
}

foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
Write-Host "`n=== run_x86 summary" -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
foreach ($n in $notes) { Write-Host "note: $n" }
$bad = @($results | Where-Object { -not $_.ok })
if ($bad.Count) { Write-Host "run_x86: $($bad.Count) of $($results.Count) cases FAILED" -ForegroundColor Red; exit 1 }
Write-Host "run_x86: all $($results.Count) cases passed" -ForegroundColor Green
exit 0
