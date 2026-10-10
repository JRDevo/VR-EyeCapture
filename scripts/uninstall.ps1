<#
    uninstall.ps1 -- unregister the OpenXR-EyeCapture implicit API layer (HKLM and HKCU). Files and recordings are left
    in place. Elevates itself (UAC) when an HKLM entry exists. DEVELOPER SCRIPT, not in the release zip: users pick
    "Unregister from VR games..." in the tray menu (or run EyeCaptureTray.exe --unregister from an admin prompt).
      powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1
#>
param([switch]$Elevated)
$ErrorActionPreference = 'Stop'
$jsonName = 'XR_APILAYER_JRDEVO_eye_capture.json'
$lm = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
$cu = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

function Ours($path) {
    if (-not (Test-Path $path)) { return @() }
    return @((Get-Item $path).GetValueNames() | Where-Object { [System.IO.Path]::GetFileName($_) -ieq $jsonName })
}

foreach ($n in (Ours $cu)) { Remove-ItemProperty -Path $cu -Name $n; Write-Host "removed HKCU: $n" }

$lmNames = Ours $lm
if ($lmNames.Count -gt 0) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        Write-Host 'Requesting administrator rights (UAC) to edit HKLM...'
        $p = Start-Process powershell -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated') -Wait -PassThru
        exit $p.ExitCode
    }
    foreach ($n in $lmNames) { Remove-ItemProperty -Path $lm -Name $n; Write-Host "removed HKLM: $n" }
}
Write-Host ''
& reg query 'HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
if ((Ours $lm).Count -eq 0 -and (Ours $cu).Count -eq 0) { Write-Host 'OpenXR-EyeCapture layer unregistered.' -ForegroundColor Green; exit 0 }
Write-Host 'some registrations remain' -ForegroundColor Red
exit 1
