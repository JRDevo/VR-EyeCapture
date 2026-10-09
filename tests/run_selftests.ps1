<#
    run_selftests.ps1 -- run encoder_selftest over the API / layout / format / codec matrix and verify every file.
    Needs a Release build (build\Release\encoder_selftest.exe). Plays a 440 Hz tone for the first case only.
    The *_smooth cases run the head-smoothing re-projection with an identity pose and output_fov_scale 1 (exact identity
    mapping), so the usual pixel checks prove the stage leaves colours unchanged (sRGB data and the linear pre-pass).
    The last case is the in-game size (ATS on VDXR: 6120x6496 per eye -> 8192x4346 HEVC) with smoothing on, for the
    cost line ("head smoothing summary: ... GPU x ms"). It runs at 72 fps: at 90 fps this machine's encoder drops ~10 %
    of the 8K frames with or without smoothing (measured), which would fail the frame-count check for an unrelated reason.
      powershell -ExecutionPolicy Bypass -File tests\run_selftests.ps1 [-Quick]
#>
param([switch]$Quick)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\Release\encoder_selftest.exe'
$outDir = Join-Path $root 'build\selftest_out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not (Test-Path $exe)) { throw "build first: $exe" }

$cases = @(
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $true },
    @{ api = 'd3d12'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d11'; layout = 'array'; eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d12'; layout = 'array'; eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d11'; layout = 'wide';  eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d12'; layout = 'wide';  eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d11'; layout = 'msaa';  eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d12'; layout = 'msaa';  eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'unorm'; codec = 'hevc'; sound = $false },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'h264'; sound = $false },
    @{ api = 'd3d11'; layout = 'two';   eye = '3072x3264'; fmt = 'srgb';  codec = 'hevc'; sound = $false },   # ETS2 on VDXR (CAPTURE_FINDINGS)
    @{ api = 'd3d12'; layout = 'two';   eye = '3072x3264'; fmt = 'srgb';  codec = 'hevc'; sound = $false },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '1.0' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'unorm'; codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '1.0' },
    @{ api = 'd3d12'; layout = 'array'; eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '1.0' },
    @{ api = 'd3d11'; layout = 'two';   eye = '6120x6496'; fmt = 'srgb';  codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '0.9'; out = '8192x4346'; fps = 72 }
)
if ($Quick) { $cases = $cases[0..1] }

$results = @()
foreach ($c in $cases) {
    $name = "{0}_{1}_{2}_{3}_{4}" -f $c.api, $c.layout, $c.eye, $c.fmt, $c.codec
    if ($c.smooth) { $name += '_smooth' }
    $out = Join-Path $outDir "$name.mp4"
    $targs = @('--api', $c.api, '--layout', $c.layout, '--eye', $c.eye, '--format', $c.fmt, '--codec', $c.codec, '--out', $out)
    if (-not $c.sound) { $targs += '--no-tone' }
    if ($c.smooth) { $targs += @('--head-smoothing', $c.smooth, '--fov-scale', $c.fov) }
    if ($c.fps) { $targs += @('--fps', "$($c.fps)") }
    Write-Host "`n=== $name" -ForegroundColor Cyan
    & $exe @targs | Select-String -Pattern 'recording summary|head smoothing summary|SubmitFrame cost|=== encoder_selftest|ERROR|WARN' | ForEach-Object { Write-Host "  $($_.Line.Substring([math]::Min(33, $_.Line.Length)))" }
    $runOk = ($LASTEXITCODE -eq 0)
    $wh = $c.eye.Split('x')
    $vargs = @{ File = $out; EyeW = [int]$wh[0]; EyeH = [int]$wh[1]; Codec = $c.codec; Format = $c.fmt; ExpectAudio = $true; ExpectSound = [bool]$c.sound }
    if ($c.fps) { $vargs.Fps = [double]$c.fps }
    if ($c.out) { $o = $c.out.Split('x'); $vargs.OutW = [int]$o[0]; $vargs.OutH = [int]$o[1]; $vargs.SkipPixels = $true }   # scaled: no pixel checks
    $verOk = $false
    if ($runOk) { & (Join-Path $PSScriptRoot 'verify_mp4.ps1') @vargs; $verOk = ($LASTEXITCODE -eq 0) }
    $results += [pscustomobject]@{ case = $name; run = $runOk; verify = $verOk }
}
Write-Host ''
$results | Format-Table -AutoSize
if ($results | Where-Object { -not ($_.run -and $_.verify) }) { exit 1 }
exit 0
