<#
    package.ps1 -- build Release and make the distributable zip.

    Produces dist\OpenXR-EyeCapture-v<version>.zip containing:
        eye_capture.dll, XR_APILAYER_JRDEVO_eye_capture.json, EyeCaptureTray.exe, config.ini,
        scripts\make_mkv.ps1, README.md, THIRD_PARTY.md
    Users only unzip and run EyeCaptureTray.exe: it registers the layer itself (one UAC prompt; tray/install.cpp) and
    unregisters it from its tray menu. scripts\install.ps1 / uninstall.ps1 are developer scripts and stay out of the zip.

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
Write-Host "Packaging OpenXR-EyeCapture v$version" -ForegroundColor Cyan

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

& $cmake -B $buildDir -S $root -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed ($LASTEXITCODE)" }
& $cmake --build $buildDir --config Release --target eye_capture EyeCaptureTray
if ($LASTEXITCODE -ne 0) { Fail "cmake build failed ($LASTEXITCODE)" }

$rel = Join-Path $buildDir 'Release'
$stageName = "OpenXR-EyeCapture-v$version"
$stage = Join-Path $distDir $stageName
$zip = Join-Path $distDir "$stageName.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'scripts') | Out-Null

$files = @(
    @{ src = (Join-Path $rel 'eye_capture.dll'); name = 'eye_capture.dll' },
    @{ src = (Join-Path $rel 'XR_APILAYER_JRDEVO_eye_capture.json'); name = 'XR_APILAYER_JRDEVO_eye_capture.json' },
    @{ src = (Join-Path $rel 'EyeCaptureTray.exe'); name = 'EyeCaptureTray.exe' },
    @{ src = (Join-Path $root 'config.ini'); name = 'config.ini' },
    @{ src = (Join-Path $root 'README.md'); name = 'README.md' },
    @{ src = (Join-Path $root 'THIRD_PARTY.md'); name = 'THIRD_PARTY.md' },
    @{ src = (Join-Path $root 'LICENSE.md'); name = 'LICENSE.md' },
    @{ src = (Join-Path $root 'scripts\make_mkv.ps1'); name = 'scripts\make_mkv.ps1' }
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
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Write-Host "`nCreated $zip" -ForegroundColor Green
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try { foreach ($e in $archive.Entries) { "  {0,10}  {1}" -f $e.Length, $e.FullName } } finally { $archive.Dispose() }
