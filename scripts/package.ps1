<#
    package.ps1 -- build Release and make the installer (user decision 2026-10-18: installer only, no zip release -- an
    uninstall through Windows Settings > Apps always cleans up; a deleted zip folder left registry entries and game files).

    Produces dist\VR-EyeCapture-Setup-v<version>.exe (Inno Setup 6, installer\VREyeCapture.iss) from the stage folder
    dist\VR-EyeCapture-v<version>\ containing:
        eye_capture.dll, XR_APILAYER_JRDEVO_eye_capture.json, EyeCaptureTray.exe, config.ini,
        steamvr\openvr_api.dll (the SteamVR proxy -- never loose in the root: the tray's "SteamVR games" copies it into a
        game folder), EyeCaptureMirror.exe for the SteamVR quick mode (it loads SteamVR's own openvr_api.dll from the
        user's SteamVR install; no Valve binary is shipped),
        scripts\make_mkv.ps1, scripts\make_thumbnail.ps1 (+ scripts\thumbnail_overlay.png), README.md, THIRD_PARTY.md
        and installed.txt (the marker: the installed copy keeps the user's data in %APPDATA% / %LOCALAPPDATA% / Videos,
        config.h IsInstalledCopy).
    The installer registers the layer (elevated, once) and its uninstaller cleans everything up (EyeCaptureTray.exe
    --uninstall-cleanup). scripts\install.ps1 / uninstall.ps1 are developer scripts and stay out of it.
    ISCC.exe: %LOCALAPPDATA%\Programs\Inno Setup 6, Program Files (x86)\Inno Setup 6 or Program Files\Inno Setup 6.

    Also produces dist\VR-EyeCapture-OBS-v<version>.zip (a separate release asset, only for OBS users):
        obs-eyecapture.dll, README.md (obs\README.md), COPYING.md (obs\COPYING.md: the plugin is GPL-2.0-or-later),
        LICENSE.md (the PolyForm Noncommercial license of the rest of the project, which COPYING.md refers to)
    The OBS plugin is never in the installer.

    The version comes from project(... VERSION x.y.z) in CMakeLists.txt.
    Windows PowerShell 5.1 compatible.  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#>
$ErrorActionPreference = 'Stop'
function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'
$distDir = Join-Path $root 'dist'

$cml = Get-Content -Raw (Join-Path $root 'CMakeLists.txt')
$verMatch = [regex]::Match($cml, 'project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $verMatch.Success) { Fail 'could not read project(... VERSION x.y.z) from CMakeLists.txt' }
$version = $verMatch.Groups[1].Value
Write-Host "Packaging VR EyeCapture v$version" -ForegroundColor Cyan

$cmake = $null
$onPath = Get-Command cmake -ErrorAction SilentlyContinue
if ($onPath) { $cmake = $onPath.Source } else {
    foreach ($base in @("$env:ProgramFiles\Microsoft Visual Studio\2022", "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022")) {
        foreach ($ed in @('Community', 'Professional', 'Enterprise')) {
            $cand = Join-Path $base "$ed\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $cand) { $cmake = $cand; break }
        }
        if ($cmake) { break }
    }
}
if (-not $cmake) { Fail 'cmake not found on PATH or under Visual Studio 2022' }

& $cmake -B $buildDir -S $root -G 'Visual Studio 17 2022' -A x64 -DEYE_BUILD_OBS_PLUGIN=ON
if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed ($LASTEXITCODE)" }
& $cmake --build $buildDir --config Release --target eye_capture EyeCaptureTray openvr_proxy EyeCaptureMirror obs-eyecapture
if ($LASTEXITCODE -ne 0) { Fail "cmake build failed ($LASTEXITCODE)" }

$rel = Join-Path $buildDir 'Release'
$stageName = "VR-EyeCapture-v$version"
$stage = Join-Path $distDir $stageName
$setup = Join-Path $distDir "VR-EyeCapture-Setup-v$version.exe"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $setup) { Remove-Item -Force $setup }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'scripts'), (Join-Path $stage 'steamvr') | Out-Null

$files = @(
    @{ src = (Join-Path $rel 'eye_capture.dll'); name = 'eye_capture.dll' },
    @{ src = (Join-Path $rel 'XR_APILAYER_JRDEVO_eye_capture.json'); name = 'XR_APILAYER_JRDEVO_eye_capture.json' },
    @{ src = (Join-Path $rel 'EyeCaptureTray.exe'); name = 'EyeCaptureTray.exe' },
    @{ src = (Join-Path $rel 'openvr_api.dll'); name = 'steamvr\openvr_api.dll' },
    @{ src = (Join-Path $rel 'EyeCaptureMirror.exe'); name = 'EyeCaptureMirror.exe' },
    @{ src = (Join-Path $root 'config.ini'); name = 'config.ini' },
    @{ src = (Join-Path $root 'README.md'); name = 'README.md' },
    @{ src = (Join-Path $root 'THIRD_PARTY.md'); name = 'THIRD_PARTY.md' },
    @{ src = (Join-Path $root 'LICENSE.md'); name = 'LICENSE.md' },
    @{ src = (Join-Path $root 'scripts\make_mkv.ps1'); name = 'scripts\make_mkv.ps1' },
    @{ src = (Join-Path $root 'scripts\make_thumbnail.ps1'); name = 'scripts\make_thumbnail.ps1' },
    @{ src = (Join-Path $root 'docs\images\thumbnail_overlay.png'); name = 'scripts\thumbnail_overlay.png' }
)
foreach ($f in $files) {
    if (-not (Test-Path $f.src)) { Fail "missing: $($f.src)" }
    Copy-Item -LiteralPath $f.src -Destination (Join-Path $stage $f.name) -Force
}
# config.ini: ship the committed defaults, never the developer's local edits (hotkey, post_* choices)
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    $committed = & $git.Source -C $root show HEAD:config.ini
    if ($LASTEXITCODE -ne 0) { Fail 'git show HEAD:config.ini failed' }
    [System.IO.File]::WriteAllText((Join-Path $stage 'config.ini'), (($committed -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding $false))
    Write-Host 'config.ini: committed version (HEAD), not the working copy'
} else { Write-Host 'WARNING: git not found -- config.ini copied from the working tree' -ForegroundColor Yellow }
# the marker of an installed copy (config.h kInstalledMarker)
[System.IO.File]::WriteAllText((Join-Path $stage 'installed.txt'),
    "Installed by the VR EyeCapture setup. Your settings are in %APPDATA%\VR EyeCapture, logs in %LOCALAPPDATA%\VR EyeCapture\logs,`r`nrecordings in Videos\VR EyeCapture (unless you chose another folder). Uninstall: Windows Settings > Apps > VR EyeCapture.`r`n",
    (New-Object System.Text.UTF8Encoding $false))
Get-ChildItem -Recurse -File $stage | ForEach-Object { "  {0,10}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1) }
$iscc = $null
foreach ($cand in @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe")) {
    if (Test-Path $cand) { $iscc = $cand; break }
}
if (-not $iscc) { Fail 'ISCC.exe (Inno Setup 6) not found' }
& $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutDir=$distDir" (Join-Path $root 'installer\VREyeCapture.iss')
if ($LASTEXITCODE -ne 0) { Fail "ISCC failed ($LASTEXITCODE)" }
Write-Host "`nCreated $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)" -ForegroundColor Green

# --- separate OBS plugin zip (own release asset; not in the installer) ---
$obsName = "VR-EyeCapture-OBS-v$version"
$obsStage = Join-Path $distDir $obsName
$obsZip = Join-Path $distDir "$obsName.zip"
if (Test-Path $obsStage) { Remove-Item -Recurse -Force $obsStage }
if (Test-Path $obsZip) { Remove-Item -Force $obsZip }
New-Item -ItemType Directory -Force -Path $obsStage | Out-Null
$obsFiles = @(
    @{ src = (Join-Path $rel 'obs-eyecapture.dll'); name = 'obs-eyecapture.dll' },
    @{ src = (Join-Path $root 'obs\README.md'); name = 'README.md' },
    @{ src = (Join-Path $root 'obs\COPYING.md'); name = 'COPYING.md' },   # GPL-2.0-or-later notice of the plugin
    @{ src = (Join-Path $root 'LICENSE.md'); name = 'LICENSE.md' }
)
foreach ($f in $obsFiles) {
    if (-not (Test-Path $f.src)) { Fail "missing: $($f.src)" }
    Copy-Item -LiteralPath $f.src -Destination (Join-Path $obsStage $f.name) -Force
}
Compress-Archive -Path (Join-Path $obsStage '*') -DestinationPath $obsZip -Force
Write-Host "`nCreated $obsZip" -ForegroundColor Green
$archive = [System.IO.Compression.ZipFile]::OpenRead($obsZip)
try { foreach ($e in $archive.Entries) { "  {0,10}  {1}" -f $e.Length, $e.FullName } } finally { $archive.Dispose() }
