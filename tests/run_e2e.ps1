<#
    run_e2e.ps1 -- end-to-end layer test over every swapchain layout and both D3D APIs, through the real OpenXR loader,
    the INSTALLED layer (run scripts\install.ps1 first) and tests/mock_runtime (no headset). Verifies every MP4.
      powershell -ExecutionPolicy Bypass -File tests\run_e2e.ps1
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$exe = Join-Path $bin 'layer_e2e.exe'
if (-not (Test-Path $exe)) { throw "build first: $exe" }
$results = @()
foreach ($api in @('d3d11', 'd3d12')) {
    foreach ($layout in @('two', 'array', 'wide', 'msaa')) {
        $out = Join-Path $root "build\e2e_out\${api}_$layout"
        if (Test-Path $out) { Remove-Item -Recurse -Force $out }
        Write-Host "`n=== $api / $layout" -ForegroundColor Cyan
        $o = & $exe --api $api --layout $layout --seconds 6 --record 4 --out-dir $out 2>&1 | Out-String
        $o.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
        $m = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
        $runOk = ($LASTEXITCODE -eq 0) -and $m.Success
        $verOk = $false
        if ($runOk) {
            # autorecord counts from the first frame; encoder start-up takes ~0.2-0.4 s of it
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $m.Groups[1].Value.Trim() -EyeW 1920 -EyeH 1080 -Seconds 3.7 -DurationTolerance 0.3 -ExpectAudio
            $verOk = ($LASTEXITCODE -eq 0)
        }
        $results += [pscustomobject]@{ api = $api; layout = $layout; run = $runOk; verify = $verOk }
    }
}
Write-Host ''
$results | Format-Table -AutoSize
Write-Host 'Layer log lines for these runs:'
Get-Content (Join-Path $root 'logs\eye_capture.log') -Tail 400 | Select-String 'frame layout \(|recording summary' | Select-Object -Last 16 | ForEach-Object { '  ' + $_.Line.Substring([math]::Min(37, $_.Line.Length)) }
if ($results | Where-Object { -not ($_.run -and $_.verify) }) { exit 1 }
exit 0
