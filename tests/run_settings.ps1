<#
    run_settings.ps1 -- tests for the tray's Settings window (no GPU, no game, does not touch the installed tray or config.ini).
      powershell -ExecutionPolicy Bypass -File tests\run_settings.ps1
    1. settings_test hotkeys : every key name the window can produce parses back through ParseHotkey.
    2. settings_test save    : the Save code path on a scratch copy of config.ini (bad values refused, three keys changed in
                               place, a missing section appended) -- the before / after diff must show ONLY those lines.
    3. settings_test drive   : a scratch tray (copy of EyeCaptureTray.exe in build\tt2, EYECAP_TRAY_TEST_SHM, scratch APPDATA)
                               opens the REAL Settings window; the test sets controls with SendMessage and clicks Save: a bad
                               value is refused (the tray logs what the message box would show), good values are written,
                               an untouched head_smoothing slider keeps its exact file text (0.57), the tooltip says
                               "settings saved" and is restored after 5 s. Only the scratch tray is stopped.
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

# ---- 2. Save on a scratch file
$s = Join-Path $root 'build\settings_scratch'
if (Test-Path $s) { Remove-Item -Recurse -Force $s }
New-Item -ItemType Directory $s | Out-Null
# the live config.ini may have any projection: the scratch copies always start at vr180 so the expected diff is fixed
$scratchIni = (Get-Content (Join-Path $root 'config.ini') -Raw) -replace '(?m)^projection = \S+', 'projection = vr180'
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
New-Item -ItemType Directory (Join-Path $t 'appdata\OpenXR-EyeCapture') | Out-Null
Copy-Item (Join-Path $bin 'EyeCaptureTray.exe') $t
$ini = Join-Path $t 'appdata\OpenXR-EyeCapture\config.ini'
(Get-Content (Join-Path $root 'config.ini') -Raw) -replace '(?m)^head_smoothing = [\d.]+', 'head_smoothing = 0.57' -replace '(?m)^projection = \S+', 'projection = vr180' | Set-Content $ini -NoNewline -Encoding utf8
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
    Step 'bad value went through the reporter (not a real message box)' ($log -match "settings message box \[Settings -- not saved\][\s\S]*bad value '7' for output_fov_scale")
    Step 'tray.log lists the changed keys' ($log -match 'settings saved to .*changed hotkey ScrollLock->F7, bitrate_mbps 0->20, swap_eyes 0->1, output_fov_scale 0\.9->0\.85, projection vr180->flat')
    Step 'tooltip says settings saved' ($log -match "tooltip now: settings saved \S+ applies to the next recording")
    Step 'tooltip restored after 5 s' ($log -match 'tooltip restored: OpenXR-EyeCapture: no OpenXR game running')
} finally {
    $env:APPDATA = $oldAppdata
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
}
$b = Get-Content (Join-Path $t 'before.ini'); $a = Get-Content $ini
$d = @(Compare-Object $b $a)
Write-Host 'diff of the scratch tray config (before => after):'; $d | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
Step 'window diff is exactly the 5 changed lines (slider untouched: head_smoothing 0.57 kept)' ($d.Count -eq 10)
if ($fail) { exit 1 } else { exit 0 }
