<#
    run_settings.ps1 -- tests for the tray app window (no GPU, no game, does not touch the installed tray or config.ini).
      powershell -ExecutionPolicy Bypass -File tests\run_settings.ps1
    1. settings_test hotkeys : every key name the window can produce parses back through ParseHotkey; Pad: / Joy: bindings,
                               binding lists and refused inputs (src/hotkey.h).
    2. settings_test save    : the Save code path on a scratch copy of config.ini (bad values refused, three keys changed in
                               place, a missing section appended) -- the before / after diff must show ONLY those lines.
    3. settings_test drive   : a scratch tray (copy of EyeCaptureTray.exe in build\tt2, EYECAP_TRAY_TEST_SHM, scratch APPDATA)
                               opens the REAL app window; "Capture..." F7, "Add..." + mouse X1 -> "F7, MouseX1",
                               "Capture..." F7 again -> "F7"; the test sets controls with SendMessage and clicks Save: a bad
                               value is refused (the tray logs what the message box would show), good values are written,
                               an untouched head_smoothing slider keeps its exact file text (0.57), the tooltip says
                               "settings saved" and is restored after 5 s; the "Convert" drop-down (post_when)
                               (4 items) loads game_exit and saves "Immediately" = now, which the tray picks up without a
                               restart; later the window is reopened, "When I say" (manual) is saved and shown on the next
                               reopen (tray logs post_when = manual). The "Convert a recording..." button and the
                               "Conversions" queue view (idle status, empty list, greyed buttons) exist; the window fits 1080p.
                               The "Include overlay layers" check loads record_overlays = 1 and saves 0 when unticked.
                               "Codec" has 3 items; AV1 makes the Default bitrate hint "about 36 GB per hour (AV1: 80
                               Mbit/s)" and saves codec = av1. "8K codec" (yt8k_codec) is greyed without 8K output like
                               "8K bitrate", loads hevc as "HEVC (default)", saves av1; both AV1 choices show on reopen.
                               The "Resolution" drop-down (max_width) loads 3000 as "Custom (3000 px)" and keeps it
                               on Save; the window is reopened three times: Custom kept -> pick 4K (3840), 4K shown ->
                               pick Native (0), Native shown -> Cancel.
                               "Output layout" sits in the Export group one row above "Eye crop" (found by ID); greyed for
                               VR180 / VR360. "Dome fill" / "Fade after" (+ labels) are enabled for VR180 / VR360 and greyed
                               for Flat. "Record directly in the YouTube layout (no re-encode)" (record_for_youtube) is
                               greyed for domes, loads 0, greys the 8K bitrate / codec (+ labels) when ticked, and is saved
                               as record_for_youtube 0->1 in place (the scratch config gets a "record_for_youtube = 0" line in
                               [capture] when config.ini has none yet).
                               Only the scratch tray is stopped.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$test = Join-Path $bin 'settings_test.exe'
if (-not (Test-Path $test)) { throw "build first: $test" }
$fail = 0
function Step($name, $ok) { Write-Host ("{0}: {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' }); if (-not $ok) { $script:fail++ } }

# ---- 1. hotkey names
& $test hotkeys; Step 'hotkeys' ($LASTEXITCODE -eq 0)
& $test layout; Step 'layout (pp::PredictLayout sizes)' ($LASTEXITCODE -eq 0)

# ---- 2. Save on a scratch file
$s = Join-Path $root 'build\settings_scratch'
if (Test-Path $s) { Remove-Item -Recurse -Force $s }
New-Item -ItemType Directory $s | Out-Null
# the live config.ini may have any projection: the scratch copies always start at vr180 so the expected diff is fixed
$scratchIni = (Get-Content (Join-Path $root 'config.ini') -Raw) -replace '(?m)^projection = \S+', 'projection = vr180' -replace '(?m)^hotkey = [^\r\n]*', 'hotkey = ScrollLock' -replace '(?m)^record_overlays = \S+', 'record_overlays = 1' -replace '(?m)^bitrate_mbps = \S+', 'bitrate_mbps = 0'
$scratchIni | Set-Content (Join-Path $s 'config.ini') -NoNewline -Encoding utf8; $scratchIni | Set-Content (Join-Path $s 'before.ini') -NoNewline -Encoding utf8
& $test save (Join-Path $s 'config.ini'); Step 'save' ($LASTEXITCODE -eq 0)
$b = Get-Content (Join-Path $s 'before.ini'); $a = Get-Content (Join-Path $s 'config.ini')
$d = @(Compare-Object $b $a)
Write-Host 'diff (before => after):'; $d | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
$expect = @('hotkey = ScrollLock', 'bitrate_mbps = 0', 'projection = vr180', 'hotkey = Ctrl+Alt+R', 'bitrate_mbps = 20', 'projection = vr360')
Step 'save diff is exactly the 3 changed lines' ($d.Count -eq 6 -and -not (Compare-Object ($d | ForEach-Object { $_.InputObject } | Sort-Object) ($expect | Sort-Object)))

# ---- 3. drive the real window of a scratch tray
$t = Join-Path $root 'build\tt2'
Get-Process EyeCaptureTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" } | Stop-Process
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
New-Item -ItemType Directory (Join-Path $t 'appdata\VR EyeCapture') | Out-Null
Copy-Item (Join-Path $bin 'EyeCaptureTray.exe') $t
$ini = Join-Path $t 'appdata\VR EyeCapture\config.ini'
$trayIni = Get-Content (Join-Path $root 'config.ini') -Raw
# record_for_youtube must be a line of the file (so Save rewrites it in place: one changed line): 0, added to [capture] if missing
if ($trayIni -match '(?m)^record_for_youtube\s*=') { $trayIni = $trayIni -replace '(?m)^record_for_youtube\s*=[^\r\n]*', 'record_for_youtube = 0' }
else { $trayIni = $trayIni -replace '(?m)^\[capture\][ \t]*(\r?\n)', "[capture]`$1record_for_youtube = 0`$1" }
$trayIni -replace '(?m)^head_smoothing = [\d.]+', 'head_smoothing = 0.57' -replace '(?m)^projection = \S+', 'projection = vr180' -replace '(?m)^hotkey = [^\r\n]*', 'hotkey = ScrollLock' -replace '(?m)^post_mkv = \S+', 'post_mkv = 1' -replace '(?m)^post_yt8k = \S+', 'post_yt8k = 0' -replace '(?m)^post_when = \S+', 'post_when = game_exit' -replace '(?m)^record_overlays = \S+', 'record_overlays = 1' -replace '(?m)^max_width = \S+', 'max_width = 3000' -replace '(?m)^eye_crop = \S+', 'eye_crop = 16:9' -replace '(?m)^bitrate_mbps = \S+', 'bitrate_mbps = 0' -replace '(?m)^yt8k_bitrate_mbps = \S+', 'yt8k_bitrate_mbps = 150' -replace '(?m)^codec = \S+', 'codec = hevc' -replace '(?m)^yt8k_codec = \S+', 'yt8k_codec = hevc' | Set-Content $ini -NoNewline -Encoding utf8
Copy-Item $ini (Join-Path $t 'before.ini')
$oldAppdata = $env:APPDATA
try {
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-settings-test'; $env:APPDATA = Join-Path $t 'appdata'
    $p = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
    Remove-Item Env:\EYECAP_TRAY_TEST_SHM
    Start-Sleep -Seconds 2
    & $test drive $p.Id noslider; Step 'drive' ($LASTEXITCODE -eq 0)
    Start-Sleep -Seconds 6
    $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
    Write-Host '--- tray.log (settings lines):'
    $log -split "`r?`n" | Where-Object { $_ -match 'settings|tooltip' -or $_ -match '^\s*line \d+' } | ForEach-Object { Write-Host "  $_" }
    Step 'bad value went through the reporter (not a real message box)' ($log -match "settings message box \[Not Saved\][\s\S]*bad value '7' for output_fov_scale")
    Step 'tray.log lists the changed keys' ($log -match 'settings saved to .*changed hotkey ScrollLock->F7, codec hevc->av1, bitrate_mbps 0->250, swap_eyes 0->1, record_overlays 1->0, record_for_youtube 0->1, output_fov_scale 0\.9->0\.85, projection vr180->flat, layout_3d tb_half->sbs_half, post_yt8k 0->1, yt8k_bitrate_mbps 150->200, yt8k_codec hevc->av1, post_when game_exit->now, eye_crop 16:9->4:3, start_with_windows 0->1')
    Step 'tray.log: Resolution saved 3000 -> 3840 (4K), then 3840 -> 0 (Native)' (($log -match 'settings saved to .*changed max_width 3000->3840\s*(\r?\n|$)') -and ($log -match 'settings saved to .*changed max_width 3840->0\s*(\r?\n|$)'))
    Step 'tooltip says settings saved' ($log -match "tooltip now: settings saved \S+ applies to the next recording")
    Step 'tooltip restored after 5 s' ($log -match 'tooltip restored: VR EyeCapture: no VR game running')
    $log -split "`r?`n" | Where-Object { $_ -match 'text does not fit|text fit check' } | ForEach-Object { Write-Host "  $_" }
    Step 'App window text fit check ran and nothing is cut (labels, checks, buttons, drop-down items; GetTextExtentPoint32)' (($log -match 'settings: text fit check: \d{2,} controls, 0 cut') -and ($log -notmatch 'settings: text fit check: \d+ controls, [1-9]') -and ($log -notmatch 'text does not fit'))
} finally {
    $env:APPDATA = $oldAppdata
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
}
$b = Get-Content (Join-Path $t 'before.ini'); $a = Get-Content $ini
$d = @(Compare-Object $b $a)
Write-Host 'diff of the scratch tray config (before => after):'; $d | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
Step 'window diff is exactly the 15 changed lines (slider untouched: head_smoothing 0.57 kept; max_width 3000 -> 0)' ($d.Count -eq 30)
$aText = Get-Content $ini -Raw
Step 'Record for YouTube ticked: saved file has record_for_youtube = 1 (once, in place)' (($aText -match '(?m)^record_for_youtube = 1\s*$') -and ([regex]::Matches($aText, '(?m)^record_for_youtube').Count -eq 1))
Step 'combo "3D MKV and 8K 3D MKV": saved file has post_mkv = 1 and post_yt8k = 1' (($aText -match '(?m)^post_yt8k = 1\s*$') -and ($aText -match '(?m)^post_mkv = 1\s*$'))
Step 'layout combo "Side by side, half width": saved file has layout_3d = sbs_half' ($aText -match '(?m)^layout_3d = sbs_half\s*$')
Step 'bitrate combo 250 Mbit/s: saved file has bitrate_mbps = 250' ($aText -match '(?m)^bitrate_mbps = 250\s*$')
Step '8K bitrate combo 200 Mbit/s: saved file has yt8k_bitrate_mbps = 200' ($aText -match '(?m)^yt8k_bitrate_mbps = 200\s*$')
Step '8K codec combo AV1: saved file has yt8k_codec = av1 (once, in place)' (($aText -match '(?m)^yt8k_codec = av1\s*$') -and ([regex]::Matches($aText, '(?m)^yt8k_codec').Count -eq 1))
Step 'Codec combo AV1: saved file has codec = av1' ($aText -match '(?m)^codec = av1\s*$')
Step 'convert combo "When I say" (last save): saved file has post_when = manual' ($aText -match '(?m)^post_when = manual\s*$')
Step 'tray.log: post_when saved now -> manual' ($log -match 'settings saved to .*changed post_when now->manual\s*(\r?\n|$)')
Step 'tray re-read post_when = manual after the save' ($log -match 'tray: post_when = manual')
Step 'crop combo "4:3": saved file has eye_crop = 4:3' ($aText -match '(?m)^eye_crop = 4:3\s*$')
Step 'Include overlay layers unticked: saved file has record_overlays = 0' ($aText -match '(?m)^record_overlays = 0\s*$')
Step 'Resolution Native: saved file has max_width = 0 (once, in place)' (($aText -match '(?m)^max_width = 0\s*$') -and ([regex]::Matches($aText, '(?m)^max_width').Count -eq 1))
Step 'tray re-read post_when = now after the save' ($log -match 'tray: post_when = now')
if ($fail) { exit 1 } else { exit 0 }
