<#
    run_hold.ps1 -- the tray's post_when hold (tray/tray.cpp UpdateHold, pp::Processor::SetHold), end to end.
      powershell -ExecutionPolicy Bypass -File tests\run_hold.ps1
    No GPU, no encoder, no real video: ffmpeg_path points at build\Release\fake_ffmpeg.exe, which "works" for
    FAKE_FFMPEG_MS (2500 ms) in 50 ms ticks and then copies the input to the .mkv (layout_3d = sbs_full = remux path, so
    the tray only checks the size). The "recordings" are files of random bytes. A scratch tray (copy of EyeCaptureTray.exe
    in build\tt5, EYECAP_TRAY_TEST_SHM, scratch APPDATA, output_dir = the scratch folder) is driven by status_sim.exe.
    Does not touch the installed tray, config.ini or the real status block. Only the scratch tray is stopped.
      A. post_when = now          : converted while the "game" (status_sim) is still live.
      B. post_when = game_exit    : the session ends (block released) but the process lingers 6 s: NOT converted while
                                    it lives; converted after it exits; hold ON / OFF logged.
      C. post_when = session_end  : same status_sim: NOT converted while the session is live, converted after the block is
                                    released while the process still lives.
      D. pause                    : session_end, a job is running (fake ffmpeg 2.5 s of ticks) when a new session goes live
                                    for ~6 s: the ffmpeg is SUSPENDED (its threads are suspended), resumed when that session
                                    ends; the job takes > 2.5 s + the suspended time and completes.
      E. exit while paused        : a paused job, then the tray is closed gracefully (WM_CLOSE): the suspended ffmpeg is
                                    resumed and terminated (no fake ffmpeg left), the partial .mkv is deleted.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
foreach ($x in 'EyeCaptureTray.exe', 'status_sim.exe', 'fake_ffmpeg.exe') { if (-not (Test-Path (Join-Path $bin $x))) { throw "build first: $x" } }
$fail = 0
function Step($name, $ok) { Write-Host ("{0}: {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' }); if (-not $ok) { $script:fail++ } }

$t = Join-Path $root 'build\tt5'
$c = Join-Path $t 'c'   # short path: status_sim's "saved <path>" message must stay under 160 characters
Get-Process EyeCaptureTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" } | Stop-Process
Get-Process fake_ffmpeg -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" } | Stop-Process
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
New-Item -ItemType Directory (Join-Path $t 'appdata\OpenXR-EyeCapture'), $c, (Join-Path $t 'ff') | Out-Null
Copy-Item (Join-Path $bin 'EyeCaptureTray.exe') $t
$fake = Join-Path $t 'ff\fake_ffmpeg.exe'
Copy-Item (Join-Path $bin 'fake_ffmpeg.exe') $fake
$sim = Join-Path $bin 'status_sim.exe'
$shm = 'eyecap-hold-test'
$ini = Join-Path $t 'appdata\OpenXR-EyeCapture\config.ini'
$baseIni = Get-Content (Join-Path $root 'config.ini') -Raw
function WriteConfig($when) {
    $s = $baseIni -replace '(?m)^post_mkv = \S+', 'post_mkv = 1' -replace '(?m)^post_flat = \S+', 'post_flat = 0' `
                  -replace '(?m)^post_yt8k = \S+', 'post_yt8k = 0' -replace '(?m)^keep_mp4 = \S+', 'keep_mp4 = 1' `
                  -replace '(?m)^layout_3d = \S+', 'layout_3d = sbs_full' -replace '(?m)^eye_crop = \S+', 'eye_crop = none' -replace '(?m)^post_when = \S+', "post_when = $when" `
                  -replace '(?m)^ffmpeg_path =.*$', "ffmpeg_path = $fake" -replace '(?m)^output_dir =.*$', "output_dir = $c" `
                  -replace '(?m)^log_level = \S+', 'log_level = info'
    if ($s -notmatch '(?m)^post_when = ') { throw 'config.ini has no post_when line to replace' }
    $s | Set-Content $ini -NoNewline -Encoding utf8
    Start-Sleep -Milliseconds 1500   # the tray re-reads config.ini within 500 ms of a change
}
function NewRec($name) {
    $p = Join-Path $c $name
    $bytes = New-Object byte[] 200000; (New-Object Random 7).NextBytes($bytes); [IO.File]::WriteAllBytes($p, $bytes)
    return $p
}
$log = Join-Path $t 'logs\tray.log'
function LogText { if (Test-Path $log) { return (Get-Content $log -Raw -Encoding UTF8) } else { return '' } }
function Mkv($mp4) { return [IO.Path]::ChangeExtension($mp4, '.mkv') }
function WaitFor($cond, $sec) { $until = (Get-Date).AddSeconds($sec); while (-not (& $cond) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 200 }; return [bool](& $cond) }
function StartSim([string[]]$simArgs) { return Start-Process $sim -ArgumentList $simArgs -PassThru -WindowStyle Hidden }
function FakeProcs { return @(Get-Process fake_ffmpeg -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" }) }
function AllThreadsSuspended($p) {
    $p.Refresh()
    $th = @($p.Threads)
    return $th.Count -gt 0 -and @($th | Where-Object { -not ($_.ThreadState -eq 'Wait' -and $_.WaitReason -eq 'Suspended') }).Count -eq 0
}

Add-Type -Namespace HoldTest -Name W -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
public static IntPtr TrayWindow(uint pid) {
    IntPtr w = IntPtr.Zero;
    while ((w = FindWindowExW(new IntPtr(-3), w, "OpenXREyeCaptureTray", null)) != IntPtr.Zero) {
        uint p; GetWindowThreadProcessId(w, out p);
        if (p == pid) return w;
    }
    return IntPtr.Zero;
}
'@

$oldAppdata = $env:APPDATA
$p = $null
try {
    $env:EYECAP_TRAY_TEST_SHM = $shm; $env:FAKE_FFMPEG_MS = '2500'; $env:APPDATA = Join-Path $t 'appdata'
    WriteConfig 'now'
    $p = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'FAKE_FFMPEG_MS') { Remove-Item "Env:\$n" }
    $env:APPDATA = $oldAppdata
    Start-Sleep -Seconds 2
    Step 'tray logged post_when = now at start' ((LogText) -match 'tray: post_when = now')

    # ---- A: now -- converted while the game is still live
    Write-Host "`n=== A: post_when = now" -ForegroundColor Cyan
    $a = NewRec 'ha_SBS.mp4'
    $s = StartSim @($shm, '12', '--outdir', $c, $a)
    $ok = WaitFor { Test-Path (Mkv $a) } 15
    Step 'A: .mkv made while status_sim is still live' ($ok -and -not $s.HasExited)
    $s | Wait-Process -Timeout 30

    # ---- B: game_exit -- session released, process lingers 6 s
    Write-Host "`n=== B: post_when = game_exit (session ends, the process lingers 6 s)" -ForegroundColor Cyan
    WriteConfig 'game_exit'
    Step 'B: tray re-read post_when = game_exit' ((LogText) -match 'tray: post_when = game_exit')
    $b = NewRec 'hb_SBS.mp4'
    $s = StartSim @($shm, '0', '--linger', '6', '--outdir', $c, $b)
    Start-Sleep -Seconds 7   # ready 1.5 + rec 1.2 + stopping 0.8 + saved 1.5 = 5 s, then released; lingers until ~11 s
    Step 'B: no .mkv while the game process lives (session already released)' (-not (Test-Path (Mkv $b)) -and -not $s.HasExited)
    Step 'B: hold ON logged (converts when the game closes)' ((LogText) -match 'queue: hold ON -- conversions wait, converts when the game closes')
    $s | Wait-Process -Timeout 30
    $ok = WaitFor { Test-Path (Mkv $b) } 15
    Step 'B: .mkv made after the game process exited' $ok
    Step 'B: hold OFF logged' ((LogText) -match 'queue: hold OFF')
    Step 'B: producer exit logged' ((LogText) -match 'tray: pid \d+ \(made a queued recording\) has exited')

    # ---- C: session_end -- converted after the block is released while the process still lives
    Write-Host "`n=== C: post_when = session_end (same status_sim)" -ForegroundColor Cyan
    WriteConfig 'session_end'
    $cc = NewRec 'hc_SBS.mp4'
    $s = StartSim @($shm, '0', '--linger', '8', '--outdir', $c, $cc)
    $saved = WaitFor { (LogText) -match [regex]::Escape("queued $cc") } 15
    Start-Sleep -Milliseconds 800   # status_sim stays live ~1.5 s after "saved"
    Step 'C: queued, but no .mkv while the session is live' ($saved -and -not (Test-Path (Mkv $cc)))
    $ok = WaitFor { Test-Path (Mkv $cc) } 12
    Step 'C: .mkv made after the session ended, while the process still lives' ($ok -and -not $s.HasExited)
    $s | Wait-Process -Timeout 30

    # ---- D: a running job is paused when a new session goes live
    Write-Host "`n=== D: session_end, a new session goes live while ffmpeg runs -> suspended, resumed" -ForegroundColor Cyan
    $d = NewRec 'hd_SBS.mp4'
    $s = StartSim @($shm, '0', '--outdir', $c, $d)
    $started = WaitFor { (FakeProcs).Count -gt 0 } 15
    Step 'D: the job started after the session ended (fake ffmpeg running)' $started
    $s2 = StartSim @($shm, '5')   # a new "game" goes live, no recordings, ~6.5 s
    $susp = WaitFor { (LogText) -match 'job: ffmpeg \(pid \d+\) SUSPENDED \(hold\)' } 5
    Step 'D: ffmpeg SUSPENDED logged' $susp
    $fp = FakeProcs
    Step 'D: every thread of the fake ffmpeg is suspended' ($fp.Count -eq 1 -and (AllThreadsSuspended $fp[0]))
    Step 'D: no .mkv while paused' (-not (Test-Path (Mkv $d)))
    $s2 | Wait-Process -Timeout 30
    $ok = WaitFor { Test-Path (Mkv $d) } 15
    Step 'D: resumed logged and the .mkv made after the second session ended' ($ok -and ((LogText) -match 'job: ffmpeg \(pid \d+\) resumed'))
    $m = [regex]::Matches((LogText), 'MKV step ok in ([\d.]+) s')
    $secs = if ($m.Count) { [double]$m[$m.Count - 1].Groups[1].Value } else { 0 }
    Write-Host "  last MKV step took $secs s (fake ffmpeg works 2.5 s)"
    Step 'D: the job took longer than 2.5 s + the paused time (> 5 s)' ($secs -gt 5)

    # ---- E: tray exits while a job is paused
    Write-Host "`n=== E: tray closed while a job is paused" -ForegroundColor Cyan
    $e = NewRec 'he_SBS.mp4'
    $s = StartSim @($shm, '0', '--outdir', $c, $e)
    $started = WaitFor { (FakeProcs).Count -gt 0 } 15
    $s2 = StartSim @($shm, '20')
    $susp = WaitFor { ([regex]::Matches((LogText), 'SUSPENDED \(hold\)')).Count -ge 2 } 5
    Step 'E: job started and paused' ($started -and $susp)
    $w = [HoldTest.W]::TrayWindow([uint32]$p.Id)
    Step 'E: found the scratch tray window' ($w -ne [IntPtr]::Zero)
    [void][HoldTest.W]::PostMessageW($w, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE -> WM_DESTROY -> Processor::Stop
    $gone = WaitFor { $p.HasExited } 10
    Step 'E: tray exited' $gone
    Step 'E: no fake ffmpeg left running' ((FakeProcs).Count -eq 0)
    Step 'E: resumed before the cancel, partial .mkv deleted' (((LogText) -match 'resumed[\s\S]*job: cancelled \(tray exiting\)') -and -not (Test-Path (Mkv $e)))
    Stop-Process -Id $s2.Id -ErrorAction SilentlyContinue
} finally {
    $env:APPDATA = $oldAppdata
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
    FakeProcs | Stop-Process -ErrorAction SilentlyContinue
}
Write-Host '--- tray.log (hold / job lines):'
(LogText) -split "`r?`n" | Where-Object { $_ -match 'hold|post_when|SUSPENDED|resumed|producer|has exited|queued|job: (done|cancelled|ffmpeg)|waiting' } | ForEach-Object { Write-Host "  $_" }
if ($fail) { exit 1 } else { exit 0 }
