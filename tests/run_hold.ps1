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
      E. post_when = manual       : the recording is queued and held ("converts when you say"); 4 s after the game process
                                    exited there is still no .mkv and no ffmpeg; a forced request (WM_APP+7 = tray menu
                                    "Convert last recording now") upgrades the held job and the .mkv is made.
      G. low disk space           : (after F) a new scratch tray with EYECAP_TEST_MIN_FREE_GB=100000 (src/disk.h) refuses
                                    the conversion: logged, balloon "Not enough disk space", no .mkv, MP4 kept.
      F. exit while paused        : a paused job, then the tray is closed gracefully (WM_CLOSE): the suspended ffmpeg is
                                    resumed and terminated (no fake ffmpeg left), the partial .mkv is deleted.
      H. Settings queue           : (after G) post_when = now, a fresh scratch tray with EYECAP_TRAY_TEST_PICK (the "picked"
                                    files, no dialog). settings_test queue drives the real window: "Add to queue..." three
                                    times for ONE MP4 with eye crop none / 4:3 / 16:9 and Keep original MP4 cleared (unsaved:
                                    config.ini keeps eye_crop = none, keep_mp4 = 1); the same format again is refused. The
                                    jobs wait ("waiting for you") although post_when = now: no ffmpeg after 4 s. queue.json
                                    lists them; the tray is closed and restarted: "restored 3 job(s)", still waiting. Start on
                                    the 16:9 row, then Start all: run order 16:9, none, 4:3; hq_SBS.mkv, hq_SBS (2).mkv and
                                    hq_SBS_4x3.mkv exist; the MP4 is kept while 2, then 1 more job needs it, deleted after the
                                    last (keep_mp4 = 0 in the snapshot); queue.json is empty again. H0 first: picking an
                                    .mkv and a _FLAT.mp4 is refused (logged, one balloon "Not a recording: ..."), both intact.
      I. killed mid-job           : (after H) MKV + flat, keep_mp4 0; the tray is KILLED (no Stop()) during the flat step:
                                    the job object kills the fake ffmpeg too; queue.json has "done_mkv" + the frozen format;
                                    a planted truncated _FLAT.mp4.part is deleted at the restart; Start all redoes only the
                                    flat step with the frozen format (config.ini changed meanwhile): no "(2).mkv", MP4 deleted.
      F also checks Stop()'s own "job: ffmpeg (pid <F's pid>) resumed (tray exiting)" line.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
foreach ($x in 'EyeCaptureTray.exe', 'status_sim.exe', 'fake_ffmpeg.exe', 'settings_test.exe') { if (-not (Test-Path (Join-Path $bin $x))) { throw "build first: $x" } }
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
    [void](WaitFor { (LogText) -match 'MKV step ok in [\d.]+ s [^\r\n]*-> hd_SBS\.mkv' } 5)
    $m = [regex]::Matches((LogText), 'MKV step ok in ([\d.]+) s [^\r\n]*-> hd_SBS\.mkv')
    $secs = if ($m.Count) { [double]$m[$m.Count - 1].Groups[1].Value } else { 0 }
    Write-Host "  last MKV step took $secs s (fake ffmpeg works 2.5 s)"
    Step 'D: the job took longer than 2.5 s + the paused time (> 5 s)' ($secs -gt 5)

    # ---- E: manual -- never converted on its own; converted after a forced request (WM_APP+7 = "Convert last recording now")
    Write-Host "`n=== E: post_when = manual (game exits; nothing converts until a forced request)" -ForegroundColor Cyan
    WriteConfig 'manual'
    Step 'E: tray re-read post_when = manual' ((LogText) -match 'tray: post_when = manual')
    $hm = NewRec 'hm_SBS.mp4'
    $s = StartSim @($shm, '0', '--outdir', $c, $hm)
    $s | Wait-Process -Timeout 30
    $queued = WaitFor { (LogText) -match [regex]::Escape("queued $hm") } 10
    Start-Sleep -Seconds 4   # the game has exited: game_exit / session_end would have started it by now
    Step 'E: queued and held (converts when you say), the game process has exited' ($queued -and $s.HasExited -and ((LogText) -match 'queued while held -- converts when you say'))
    Step 'E: no .mkv and no ffmpeg 4 s after the game exited' (-not (Test-Path (Mkv $hm)) -and (FakeProcs).Count -eq 0)
    $w = [HoldTest.W]::TrayWindow([uint32]$p.Id)
    [void][HoldTest.W]::PostMessageW($w, 0x8007, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_APP+7 = Convert last recording now
    $ok = WaitFor { Test-Path (Mkv $hm) } 15
    Step 'E: forced request: logged, the held job was upgraded and the .mkv made' ($ok -and ((LogText) -match 'convert now requested for .*hm_SBS\.mp4 \(WM_APP\+7\)') -and ((LogText) -match 'hm_SBS\.mp4 was waiting for the hold -- forced, runs now'))
    WriteConfig 'session_end'

    # ---- F: tray exits while a job is paused
    Write-Host "`n=== F: tray closed while a job is paused" -ForegroundColor Cyan
    $e = NewRec 'he_SBS.mp4'
    $s = StartSim @($shm, '0', '--outdir', $c, $e)
    $started = WaitFor { (FakeProcs).Count -gt 0 } 15
    $s2 = StartSim @($shm, '20')
    $susp = WaitFor { ([regex]::Matches((LogText), 'SUSPENDED \(hold\)')).Count -ge 2 } 5
    Step 'F: job started and paused' ($started -and $susp)
    $sm = [regex]::Matches((LogText), 'job: ffmpeg \(pid (\d+)\) SUSPENDED \(hold\)')
    $ffpidF = if ($sm.Count) { $sm[$sm.Count - 1].Groups[1].Value } else { 'none' }   # case F's ffmpeg (D's came first)
    $w = [HoldTest.W]::TrayWindow([uint32]$p.Id)
    Step 'F: found the scratch tray window' ($w -ne [IntPtr]::Zero)
    [void][HoldTest.W]::PostMessageW($w, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE -> WM_DESTROY -> Processor::Stop
    $gone = WaitFor { $p.HasExited } 10
    Step 'F: tray exited' $gone
    Step 'F: no fake ffmpeg left running' ((FakeProcs).Count -eq 0)
    # Stop() itself must resume F's ffmpeg (its exact line, F's pid; D's "resumed (hold cleared)" does not count), then cancel
    Step "F: Stop() resumed F's ffmpeg (pid $ffpidF) before the cancel, no .mkv / .mkv.part left" (((LogText) -match "job: ffmpeg \(pid $ffpidF\) resumed \(tray exiting\)[\s\S]*job: cancelled \(tray exiting\)") -and
        -not (Test-Path (Mkv $e)) -and -not (Test-Path ((Mkv $e) + '.part')))
    $qjF = Join-Path $t 'queue.json'
    Step 'F: the interrupted job is kept in queue.json' ((Test-Path $qjF) -and ((Get-Content $qjF -Raw) -match 'he_SBS\.mp4'))
    Stop-Process -Id $s2.Id -ErrorAction SilentlyContinue

    # ---- G: low-space guard (src/disk.h): a new scratch tray whose threshold is raised to 100000 GB refuses the conversion
    Write-Host "`n=== G: low disk space -> conversion not started (EYECAP_TEST_MIN_FREE_GB=100000)" -ForegroundColor Cyan
    WriteConfig 'now'
    $env:EYECAP_TRAY_TEST_SHM = $shm; $env:FAKE_FFMPEG_MS = '2500'; $env:APPDATA = Join-Path $t 'appdata'; $env:EYECAP_TEST_MIN_FREE_GB = '100000'
    $p = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'FAKE_FFMPEG_MS', 'EYECAP_TEST_MIN_FREE_GB') { Remove-Item "Env:\$n" }
    $env:APPDATA = $oldAppdata
    Start-Sleep -Seconds 2
    $g = NewRec 'hg_SBS.mp4'
    $s = StartSim @($shm, '0', '--outdir', $c, $g)
    $refused = WaitFor { (LogText) -match 'job: NOT STARTED hg_SBS\.mp4: low disk space' } 15
    Step 'G: conversion refused and logged (low disk space, MP4 kept)' $refused
    Step 'G: the job interrupted in F came back from queue.json, waiting for you' ((LogText) -match 'queue: restored he_SBS\.mp4 \(settings of config\.ini at its start\) -- waits for you')
    Step 'G: balloon "Not enough disk space", no .mkv, no ffmpeg, MP4 still there' (((LogText) -match 'balloon \[Not enough disk space\] hg_SBS\.mp4 was not converted') -and -not (Test-Path (Mkv $g)) -and (FakeProcs).Count -eq 0 -and (Test-Path $g))
    $s | Wait-Process -Timeout 30

    # ---- H: the Settings window's queue: "Add to queue..." jobs wait (even with post_when = now), each with its own format
    # snapshot taken from the window (unsaved values); the queue survives a tray restart (queue.json); Start runs the selected
    # job next, Start all the rest in order; one MP4 converted with three eye crops gives three files; keep_mp4 = 0 deletes the
    # MP4 only after the last of its jobs.
    Write-Host "`n=== H: Add to queue x3 (eye crop none / 4:3 / 16:9, keep MP4 off, unsaved), restart, Start + Start all" -ForegroundColor Cyan
    $w = [HoldTest.W]::TrayWindow([uint32]$p.Id)
    if ($w -ne [IntPtr]::Zero) { [void][HoldTest.W]::PostMessageW($w, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }   # G's tray: WM_CLOSE
    [void](WaitFor { $p.HasExited } 10)
    Remove-Item (Join-Path $t 'queue.json') -ErrorAction SilentlyContinue   # F's interrupted job (restored by G's tray): not part of H
    WriteConfig 'now'
    $hq = NewRec 'hq_SBS.mp4'
    $pick = Join-Path $t 'pick.txt'
    Set-Content $pick -Value $hq -Encoding utf8
    $qtest = Join-Path $bin 'settings_test.exe'
    function StartTray {
        $env:EYECAP_TRAY_TEST_SHM = $shm; $env:FAKE_FFMPEG_MS = '6000'; $env:APPDATA = Join-Path $t 'appdata'; $env:EYECAP_TRAY_TEST_PICK = $pick
        $tp = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
        foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'FAKE_FFMPEG_MS', 'EYECAP_TRAY_TEST_PICK') { Remove-Item "Env:\$n" }
        $env:APPDATA = $oldAppdata
        Start-Sleep -Seconds 2
        return $tp
    }
    function Queue([string[]]$qa) {
        $out = & $qtest queue $p.Id @qa 2>&1 | Out-String
        $script:qExit = $LASTEXITCODE
        Write-Host ($out.TrimEnd() -split "`r?`n" | ForEach-Object { "    $_" } | Out-String).TrimEnd()
        return $out
    }
    function Items($out) { return @(($out -split "`r?`n") | Where-Object { $_ -like 'ITEM: *' }) }
    $p = StartTray
    # H0: picking a converted file (an .mkv, a _FLAT.mp4) is refused: it would be re-encoded and, with keep_mp4 = 0, deleted
    $bad = @((NewRec 'hq_old.mkv'), (NewRec 'hq_old_FLAT.mp4'))
    Set-Content $pick -Value $bad -Encoding utf8
    $o = Queue @('add', '3', '0')
    $l = LogText
    Step 'H0: picked .mkv / _FLAT.mp4 refused (logged, one balloon "Not a recording: ..."), nothing queued, both files intact' (
        @(Items $o | Where-Object { $_ -match 'hq_old' }).Count -eq 0 -and ($l -match 'hq_old\.mkv is not a recording') -and ($l -match 'hq_old_FLAT\.mp4 is not a recording') -and
        ($l -match 'balloon \[OpenXR-EyeCapture\] Not a recording: hq_old\.mkv, hq_old_FLAT\.mp4') -and
        @($bad | Where-Object { (Test-Path $_) -and (Get-Item $_).Length -eq 200000 }).Count -eq 2)
    Set-Content $pick -Value $hq -Encoding utf8
    $o = Queue @('add', '3', '0'); $okA = $qExit -eq 0
    $o = Queue @('add', '1', '0'); $okA = $okA -and $qExit -eq 0
    $o = Queue @('add', '0', '0'); $okA = $okA -and $qExit -eq 0
    $o = Queue @('add', '3', '0')   # the same file with the same format again: not added twice
    $it = Items $o
    Step 'H: three jobs listed for one MP4, each "waiting for you" with its own format (none / 4:3 / 16:9)' ($okA -and $it.Count -eq 3 -and
        $it[0] -match 'hq_SBS\.mp4  -  MKV, none, sbs_full  -  waiting for you' -and $it[1] -match 'hq_SBS\.mp4  -  MKV, 4:3, sbs_full  -  waiting for you' -and
        $it[2] -match 'hq_SBS\.mp4  -  MKV, 16:9, sbs_full  -  waiting for you')
    Step 'H: the same file + same format again is refused (already waiting)' ((LogText) -match 'queue: hq_SBS\.mp4 \(MKV, none, sbs_full\) is already waiting')
    Start-Sleep -Seconds 4
    Step 'H: post_when = now, but nothing runs: no ffmpeg, no .mkv after 4 s' ((FakeProcs).Count -eq 0 -and -not (Test-Path (Mkv $hq)) -and
        ((LogText) -match 'queue: hq_SBS\.mp4 added \(MKV, 4:3, sbs_full\) -- waits for you'))
    $iniNow = Get-Content $ini -Raw
    Step 'H: the window values were NOT saved (config.ini still eye_crop = none, keep_mp4 = 1)' (($iniNow -match '(?m)^eye_crop = none\s*$') -and ($iniNow -match '(?m)^keep_mp4 = 1\s*$'))
    $qj = Join-Path $t 'queue.json'
    $qText = if (Test-Path $qj) { Get-Content $qj -Raw } else { '' }
    Step 'H: queue.json lists the three jobs with their eye crops' (([regex]::Matches($qText, 'hq_SBS\.mp4')).Count -eq 3 -and $qText -match '"eye_crop":"4:3"' -and $qText -match '"eye_crop":"16:9"' -and $qText -match '"keep_mp4":0')
    # restart: the jobs come back, still waiting for you
    $w = [HoldTest.W]::TrayWindow([uint32]$p.Id)
    [void][HoldTest.W]::PostMessageW($w, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Step 'H: tray closed' (WaitFor { $p.HasExited } 10)
    $p = StartTray
    Step 'H: restarted tray restored 3 jobs from queue.json' ((LogText) -match 'queue: restored 3 job\(s\) from queue\.json')
    $o = Queue @('list')
    $it = Items $o
    Step 'H: after the restart the three jobs wait for you, same formats, nothing runs' ($it.Count -eq 3 -and @($it | Where-Object { $_ -match 'waiting for you' }).Count -eq 3 -and
        $it[1] -match 'MKV, 4:3, sbs_full' -and (FakeProcs).Count -eq 0)
    # Start on row 2 (16:9): it runs first; then Start all: none, then 4:3
    $o = Queue @('start', '2')
    Step 'H: Start on the 16:9 row' ($qExit -eq 0)
    $o = Queue @('startall')
    $it = Items $o
    Step 'H: Start all: the other two are queued in order (none, then 4:3)' ($qExit -eq 0 -and $it.Count -eq 2 -and $it[0] -match 'none.*next' -and $it[1] -match '4:3.*queued \(started by you\)')
    $done = WaitFor { ([regex]::Matches((LogText), 'job: done hq_SBS')).Count -ge 3 } 60
    $outs = @('hq_SBS.mkv', 'hq_SBS (2).mkv', 'hq_SBS_4x3.mkv') | ForEach-Object { Join-Path $c $_ }
    Step 'H: all three jobs done; hq_SBS.mkv (16:9), hq_SBS (2).mkv (none), hq_SBS_4x3.mkv (4:3) exist' ($done -and @($outs | Where-Object { Test-Path $_ }).Count -eq 3)
    $starts = [regex]::Matches((LogText), 'job: start hq_SBS\.mp4 \([^)]*\) format MKV, ([^,]+), sbs_full') | ForEach-Object { $_.Groups[1].Value }
    Write-Host "  job order: $($starts -join ', ')"
    Step 'H: run order 16:9 (Start), none, 4:3 (Start all, oldest first)' (($starts -join ',') -eq '16:9,none,4:3')
    $l = LogText
    Step 'H: keep_mp4 = 0: the MP4 stayed after jobs 1 and 2 (2, then 1 more job needed it) and was deleted after the last' (
        ($l -match 'keep_mp4 = 0, but 2 more queued job\(s\) need hq_SBS\.mp4') -and ($l -match 'keep_mp4 = 0, but 1 more queued job\(s\) need hq_SBS\.mp4') -and
        ($l -match 'job: deleted hq_SBS\.mp4 \(keep_mp4 = 0\)') -and -not (Test-Path $hq))
    $qText = if (Test-Path $qj) { Get-Content $qj -Raw } else { '' }
    Step 'H: queue.json is empty again' ($qText -notmatch 'hq_SBS')

    # ---- I: the tray is KILLED during the 2nd step of an MKV + flat job (like a logoff / crash: no Stop()). The job object
    # kills the fake ffmpeg with it; queue.json has the finished MKV step ("done_mkv") and the frozen format; a truncated
    # <name>_FLAT.mp4.part is left (planted here: the fake writes its output only at the end). After a restart the .part is
    # deleted, and Start all makes only the flat step (no "hi_SBS (2).mkv"), with the frozen format although config.ini
    # changed meanwhile (post_flat 0, keep_mp4 1 there; the snapshot says flat + keep_mp4 0: the MP4 is deleted).
    Write-Host "`n=== I: tray killed during the flat step of MKV + flat; restart: stale .part deleted, MKV step not redone" -ForegroundColor Cyan
    $iniI = $baseIni -replace '(?m)^post_mkv = \S+', 'post_mkv = 1' -replace '(?m)^post_flat = \S+', 'post_flat = 1' `
                     -replace '(?m)^post_yt8k = \S+', 'post_yt8k = 0' -replace '(?m)^keep_mp4 = \S+', 'keep_mp4 = 0' `
                     -replace '(?m)^layout_3d = \S+', 'layout_3d = sbs_full' -replace '(?m)^eye_crop = \S+', 'eye_crop = none' -replace '(?m)^post_when = \S+', 'post_when = now' `
                     -replace '(?m)^ffmpeg_path =.*$', "ffmpeg_path = $fake" -replace '(?m)^output_dir =.*$', "output_dir = $c" `
                     -replace '(?m)^log_level = \S+', 'log_level = info'
    $iniI | Set-Content $ini -NoNewline -Encoding utf8
    Start-Sleep -Milliseconds 1500
    $hi = NewRec 'hi_SBS.mp4'
    $hiMkv = Join-Path $c 'hi_SBS.mkv'; $hiFlat = Join-Path $c 'hi_SBS_FLAT.mp4'; $hiPart = "$hiFlat.part"
    $s = StartSim @($shm, '0', '--outdir', $c, $hi)
    $inFlat = WaitFor { (LogText) -match 'job: flat cmd \(libx264\): .*hi_SBS_FLAT\.mp4\.part' } 30
    [void](WaitFor { (FakeProcs).Count -gt 0 } 5)
    $qText = if (Test-Path $qj) { Get-Content $qj -Raw } else { '' }
    Step 'I: MKV step done (final hi_SBS.mkv, no .part), the flat step runs, queue.json has done_mkv + the frozen format' ($inFlat -and (Test-Path $hiMkv) -and
        -not (Test-Path "$hiMkv.part") -and (FakeProcs).Count -eq 1 -and $qText -match '"path":"[^"]*hi_SBS\.mp4","own_format":1,"done_mkv":"hi_SBS\.mkv"' -and
        $qText -match '"post_flat":1' -and $qText -match '"keep_mp4":0')
    $mkvSize = (Get-Item $hiMkv).Length
    Stop-Process -Id $p.Id   # TerminateProcess: no Stop(), no WM_DESTROY (a crash / kill)
    [void](WaitFor { $p.HasExited } 10)
    Step 'I: tray killed -> the job object killed its fake ffmpeg (no orphan)' (WaitFor { (FakeProcs).Count -eq 0 } 5)
    $s | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue
    [IO.File]::WriteAllBytes($hiPart, (New-Object byte[] 1000))   # what a killed ffmpeg leaves: a truncated output under the .part name
    WriteConfig 'now'   # post_flat 0, keep_mp4 1 now: the restored job must use its frozen snapshot
    $p = StartTray
    $l = LogText
    Step 'I: restart: stale hi_SBS_FLAT.mp4.part deleted; the job restored with "already made: hi_SBS.mkv", waiting for you' (-not (Test-Path $hiPart) -and
        ($l -match 'job: deleted stale partial output hi_SBS_FLAT\.mp4\.part \(left by an interrupted conversion; queue\.json restored\)') -and
        ($l -match 'queue: restored hi_SBS\.mp4 \(MKV \+ 2D, none, sbs_full; already made: hi_SBS\.mkv\) -- waits for you'))
    $o = Queue @('startall')
    $done = WaitFor { (LogText) -match 'job: done hi_SBS\.mkv' } 60
    $l = LogText
    $seg = [regex]::Match($l, 'job: start hi_SBS\.mp4[^\r\n]*own settings[\s\S]*?job: done hi_SBS\.mkv').Value
    Step 'I: restored job: "MKV step already done", no MKV command, only the flat step ran' ($done -and $seg -and
        ($seg -match 'job: MKV step already done before the restart: hi_SBS\.mkv') -and ($seg -notmatch 'job: MKV cmd') -and ($seg -match 'job: flat step ok in [\d.]+ s \(\d+ bytes\) -> hi_SBS_FLAT\.mp4'))
    Step 'I: hi_SBS.mkv untouched, no "hi_SBS (2).mkv", hi_SBS_FLAT.mp4 made, no .part left' ((Get-Item $hiMkv).Length -eq $mkvSize -and
        -not (Test-Path (Join-Path $c 'hi_SBS (2).mkv')) -and (Test-Path $hiFlat) -and -not (Test-Path $hiPart) -and @(Get-ChildItem $c -Filter '*.part').Count -eq 0)
    Step 'I: snapshot keep_mp4 = 0 applied (MP4 deleted), balloon names both files' (-not (Test-Path $hi) -and ($seg -match 'job: deleted hi_SBS\.mp4 \(keep_mp4 = 0\)') -and
        ($l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube: hi_SBS\.mkv\s+Flat 2D: hi_SBS_FLAT\.mp4'))
    $qText = if (Test-Path $qj) { Get-Content $qj -Raw } else { '' }
    Step 'I: queue.json is empty again' ($qText -notmatch 'hi_SBS')
} finally {
    $env:APPDATA = $oldAppdata
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
    FakeProcs | Stop-Process -ErrorAction SilentlyContinue
}
Write-Host '--- tray.log (hold / job lines):'
(LogText) -split "`r?`n" | Where-Object { $_ -match 'hold|post_when|SUSPENDED|resumed|producer|has exited|queued|job: (done|cancelled|ffmpeg)|waiting' } | ForEach-Object { Write-Host "  $_" }
if ($fail) { exit 1 } else { exit 0 }
