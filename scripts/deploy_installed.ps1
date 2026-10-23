<#
.SYNOPSIS
    DEV ONLY (not in the zip / installer): copies a fresh build over the INSTALLED copy, so a small change can be tried
    without running the setup again.

.DESCRIPTION
    1. Refuses while SteamVR, a VR game, OBS or one of our helpers runs (their files are in use).
    2. Closes the installed tray (EyeCaptureTray.exe --quit).
    3. Copies the program files from build\Release (+ eye_capture32.dll from build32\Release) into the install folder.
       The install folder is in Program Files, so this step runs elevated: ONE Windows admin prompt (UAC).
       Settings (%APPDATA%), logs (%LOCALAPPDATA%), recordings and config.ini are not touched.
    4. Copies the new SteamVR proxy (openvr_api.dll) into every SteamVR game enabled in the tray's "SteamVR games" window
       (folders that hold our proxy: eye_capture_proxy.ini + openvr_api_real.dll) -- no untick / tick needed.
    5. Starts the tray again (through explorer.exe, as a normal user).

    Build first (cmake --build build --config Release; for 32-bit games also build32). Nothing is registered: the HKLM
    layer registration already points at the install folder.

.PARAMETER Target
    The install folder (default C:\Program Files\VR EyeCapture; it must hold installed.txt).
.PARAMETER WhatIf
    Only print what would be copied.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\deploy_installed.ps1
#>
param(
    [string]$Target = "$env:ProgramFiles\VR EyeCapture",
    [switch]$WhatIf,
    [string]$CopyList = ''   # internal: the elevated child copies the pairs in this file
)
$ErrorActionPreference = 'Stop'

# ---- elevated child: copy the list, write the result next to it, exit ----
if ($CopyList) {
    $out = @()
    $bad = 0
    foreach ($line in Get-Content -LiteralPath $CopyList) {
        $p = $line -split '\|'
        try { Copy-Item -LiteralPath $p[0] -Destination $p[1] -Force; $out += "ok    $($p[1])" }
        catch { $out += "FAIL  $($p[1]): $($_.Exception.Message)"; $bad++ }
    }
    $out | Set-Content -LiteralPath "$CopyList.result" -Encoding UTF8
    exit $bad
}

function Fail($m) { Write-Host "deploy_installed: $m" -ForegroundColor Red; exit 1 }
$root = Split-Path $PSScriptRoot -Parent
$rel = Join-Path $root 'build\Release'
$rel32 = Join-Path $root 'build32\Release'
if (-not (Test-Path (Join-Path $Target 'installed.txt'))) { Fail "$Target is not an installed copy (no installed.txt)" }

# 1. nothing may hold the files
$busy = @(Get-Process -ErrorAction SilentlyContinue -Name vrserver, vrcompositor, vrmonitor, amtrucks, eurotrucks2, obs64, hlvr,
                     EyeCaptureMirror, EyeCaptureVrInput, EyeCaptureRecorder | Select-Object -ExpandProperty Name -Unique)
if ($busy.Count) { Fail "close these first (their files are in use): $($busy -join ', ')" }

# what to copy (the program files of scripts\package.ps1; not config.ini / docs)
$pairs = @()
foreach ($f in @(@{ s = "$rel\eye_capture.dll"; d = 'eye_capture.dll' },
                 @{ s = "$rel\XR_APILAYER_JRDEVO_eye_capture.json"; d = 'XR_APILAYER_JRDEVO_eye_capture.json' },
                 @{ s = "$rel\EyeCaptureTray.exe"; d = 'EyeCaptureTray.exe' },
                 @{ s = "$rel\EyeCaptureMirror.exe"; d = 'EyeCaptureMirror.exe' },
                 @{ s = "$rel\EyeCaptureVrInput.exe"; d = 'EyeCaptureVrInput.exe' },
                 @{ s = "$rel\EyeCaptureRecorder.exe"; d = 'EyeCaptureRecorder.exe' },
                 @{ s = "$rel\openvr_api.dll"; d = 'steamvr\openvr_api.dll' },
                 @{ s = "$rel32\eye_capture32.dll"; d = 'eye_capture32.dll' },
                 @{ s = "$rel32\XR_APILAYER_JRDEVO_eye_capture32.json"; d = 'XR_APILAYER_JRDEVO_eye_capture32.json' })) {
    if (-not (Test-Path $f.s)) {
        if ($f.s -like "$rel32*") { Write-Host "  (skipped, not built: $($f.s))" -ForegroundColor Yellow; continue }
        Fail "missing $($f.s) -- build first"
    }
    $pairs += "$($f.s)|$(Join-Path $Target $f.d)"
}
# SteamVR games with our proxy (the tray's list; the files in the game folder are the truth)
$games = @()
$list = Join-Path $env:LOCALAPPDATA 'VR EyeCapture\steamvr_games.ini'
if (Test-Path $list) {
    $inGame = $false
    foreach ($l in Get-Content -LiteralPath $list) {
        if ($l -match '^\s*\[(\w+)\]') { $inGame = $Matches[1] -eq 'game'; continue }
        if ($inGame -and $l -match '^\s*dll\s*=\s*(.+?)\s*$') {
            $d = $Matches[1]
            if ((Test-Path (Join-Path $d 'eye_capture_proxy.ini')) -and (Test-Path (Join-Path $d 'openvr_api_real.dll'))) { $games += $d }
        }
    }
}

Write-Host "deploy_installed: $($pairs.Count) files -> $Target; SteamVR proxy -> $($games.Count) game folder(s)"
foreach ($p in $pairs) { Write-Host "  $(($p -split '\|')[1])" }
foreach ($g in $games) { Write-Host "  $g\openvr_api.dll" }
if ($WhatIf) { Write-Host '(WhatIf: nothing changed)'; exit 0 }

# 2. close the installed tray
$tray = Join-Path $Target 'EyeCaptureTray.exe'
& $tray --quit | Out-Null
for ($i = 0; $i -lt 50 -and (Get-Process EyeCaptureTray -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 200 }
if (Get-Process EyeCaptureTray -ErrorAction SilentlyContinue) { Fail 'the tray did not close (EyeCaptureTray still runs)' }

# 3. elevated copy into Program Files (one UAC prompt)
$tmp = Join-Path $env:TEMP "vrec_deploy_$PID.txt"
$pairs | Set-Content -LiteralPath $tmp -Encoding UTF8
Write-Host 'copying into the install folder (Windows asks for admin rights once)...'
try {
    $child = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-CopyList', "`"$tmp`"")
} catch {
    Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$tray`""   # the old tray again, unchanged
    Fail "the admin prompt was declined -- nothing copied into $Target (the tray is starting again, unchanged)"
}
$okAll = $child.ExitCode -eq 0
if (Test-Path "$tmp.result") { Get-Content "$tmp.result" | ForEach-Object { Write-Host "  $_" } }
Remove-Item -LiteralPath $tmp, "$tmp.result" -ErrorAction SilentlyContinue

# 4. the proxy into the enabled SteamVR games (user-writable game folders: no admin needed)
foreach ($g in $games) {
    try { Copy-Item -LiteralPath "$rel\openvr_api.dll" -Destination (Join-Path $g 'openvr_api.dll') -Force; Write-Host "  ok    $g\openvr_api.dll" }
    catch { Write-Host "  FAIL  $g\openvr_api.dll: $($_.Exception.Message) (is the game running?)" -ForegroundColor Red; $okAll = $false }
}

# 5. the tray again, as the normal user
Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$tray`""
if ($okAll) { Write-Host 'deploy_installed: done -- the tray is starting again' -ForegroundColor Green; exit 0 }
Write-Host 'deploy_installed: some files were NOT replaced (see FAIL above)' -ForegroundColor Red
exit 1
