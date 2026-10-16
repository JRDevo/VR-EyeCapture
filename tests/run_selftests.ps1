<#
    run_selftests.ps1 -- run encoder_selftest over the API / layout / format / codec matrix and verify every file.
    Needs a Release build (build\Release\encoder_selftest.exe). Plays a 440 Hz tone for the first case only.
    symmetric_fov is on by default, so every case goes through the re-projection pass (the self-test's views are
    symmetric, so it is an identity mapping); the *_raw cases run with --no-symmetric (stage bypassed, the old path).
    The *_smooth cases run the head-smoothing re-projection with an identity pose and output_fov_scale 1 (exact identity
    mapping), so the usual pixel checks prove the stage leaves colours unchanged (sRGB data and the linear pre-pass).
    The last case is the in-game size (ATS on VDXR: 6120x6496 per eye -> 8192x4348 HEVC; FitStageOutput rounds the height to the nearest even, the old video-processor path truncated to 4346) with smoothing on, for the
    cost line ("head smoothing summary: ... GPU x ms"). It runs at 72 fps: at 90 fps this machine's encoder drops ~10 %
    of the 8K frames with or without smoothing (measured), which would fail the frame-count check for an unrelated reason.
    codec = av1 (hardware AV1 MFT; needs a GPU that encodes AV1, e.g. RTX 40 / 50): a 1080p case with the usual pixel checks,
    the in-game 8K size at 90 fps (the AV1 path asks the encoder for quality vs speed 25 above 1 Gpixel/s, so 8K keeps 90
    fps where HEVC drops ~10 %), and *_noav1: XR_EYE_CAPTURE_TEST_NO_AV1 = 1 makes the AV1 sink writer attempt fail in
    CreateWriter (after AddStream), so the real fallback runs -> the recording must fall back to HEVC (verified as hevc,
    and the log must say "AV1 unavailable -- fell back to HEVC"). On a machine without an AV1 encoder the plain av1 cases
    fall back to HEVC and fail the codec check, which is the point: they tell you AV1 is not available.
    The 8K (out=) cases also assert "re-projection rendered at 8192x4348 (... 2x2 bilinear taps ...)": the stage renders at
    the recorded size and the video processor does not rescale. *_raw_smooth_mw1280 = raw layout (--no-symmetric) + head
    smoothing (identity) + max_width 1280: the scaled raw-layout re-projection (3840x1080 -> 1280x360, 3x, 2x2 taps); the
    pixel checks run on the scaled frame (640x360 per eye) and the log must say "re-projection rendered at 1280x360".
    *_yt_* (record_for_youtube, --youtube <layout_3d> [--eye-crop]): the stage renders the YouTube layout straight into the
    encoded frame (PredictYtLayout; yt_short = XR_EYE_CAPTURE_TEST_YT_SHORT, the frame's short side, so the small cases stay
    fast): tb_half 1920x1080 short 540 -> 960x540 (eye 960x270), sbs_half -> 960x540 (eye 480x540), 1200x1600 eye_crop 4:3
    short 450 -> crop 1200x900 -> 600x452 (eye 600x226), the same with eye_crop 4:3pad -> padded to 1600x900 -> 800x452 (eye
    800x226, picture 600 wide at x 100, black bars checked), and the real size 1920x1080 tb_half -> 7680x4320 at 72 fps (short
    side not overridden; 4x upscale, 1 tap); *_yt_*_nosym = --no-symmetric (symmetric_fov = 0 is overridden for the
    recording, logged). verify_mp4.ps1 -Packing checks the frame size, st3d top-bottom (stereo_mode 1, ffprobe "top and
    bottom") / left-right and samples each eye in its packed rectangle; the log must have the "YouTube layout: ..." line,
    "re-projection rendered at <frame>" with the expected taps and the summary's "layout YouTube <packing>"; the sidecar
    <name>.json must be exactly the one line the tray reads.
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
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; raw = $true },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'unorm'; codec = 'hevc'; sound = $false; raw = $true },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '1.0' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'unorm'; codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '1.0' },
    @{ api = 'd3d12'; layout = 'array'; eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '1.0' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '1.0'; raw = $true; mw = 1280; pix = '640x360'; rendered = '1280x360'; taps = 2 },
    @{ api = 'd3d11'; layout = 'two';   eye = '6120x6496'; fmt = 'srgb';  codec = 'hevc'; sound = $false; smooth = '0.6'; fov = '0.9'; out = '8192x4348'; fps = 72; rendered = '8192x4348'; taps = 2 },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'av1';  sound = $false },
    @{ api = 'd3d12'; layout = 'array'; eye = '1920x1080'; fmt = 'unorm'; codec = 'av1';  sound = $false },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'av1';  sound = $false; noav1 = $true; expect = 'hevc' },
    @{ api = 'd3d11'; layout = 'two';   eye = '6120x6496'; fmt = 'srgb';  codec = 'av1';  sound = $false; smooth = '0.6'; fov = '0.9'; out = '8192x4348'; fps = 90; rendered = '8192x4348'; taps = 2 },
    # record_for_youtube: yt = layout_3d, crop = eye_crop (default 16:9), ytshort = XR_EYE_CAPTURE_TEST_YT_SHORT (0 = not set)
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; yt = 'tb_half';  ytshort = 540; out = '960x540'; rendered = '960x540'; taps = 2;
       ytlog = 'eye 1920x1080 -> crop 16:9 1920x1080 -> tb_half frame 960x540 (each eye 960x270; short side 540, long side <= 8192), st3d top-bottom' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; yt = 'sbs_half'; ytshort = 540; out = '960x540'; rendered = '960x540'; taps = 2;
       ytlog = 'eye 1920x1080 -> crop 16:9 1920x1080 -> sbs_half frame 960x540 (each eye 480x540; short side 540, long side <= 8192), st3d left-right' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1200x1600'; fmt = 'srgb';  codec = 'hevc'; sound = $false; yt = 'tb_half';  crop = '4:3'; ytshort = 450; out = '600x452'; rendered = '600x452'; taps = 2;
       ytlog = 'eye 1200x1600 -> crop 4:3 1200x900 -> tb_half frame 600x452 (each eye 600x226; short side 450, long side <= 8192), st3d top-bottom' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1200x1600'; fmt = 'srgb';  codec = 'hevc'; sound = $false; yt = 'tb_half';  crop = '4:3pad'; ytshort = 450; out = '800x452'; rendered = '800x452'; taps = 2;
       ytlog = 'eye 1200x1600 -> crop 4:3pad 1200x900 -> black bars left and right to 1600x900 (picture 600 wide at x 100 of each eye) -> tb_half frame 800x452 (each eye 800x226; short side 450, long side <= 8192), st3d top-bottom' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; yt = 'tb_half';  ytshort = 540; out = '960x540'; rendered = '960x540'; taps = 2; raw = $true;
       ytlog = 'eye 1920x1080 -> crop 16:9 1920x1080 -> tb_half frame 960x540 (each eye 960x270; short side 540, long side <= 8192), st3d top-bottom';
       ytextra = 'record_for_youtube: symmetric_fov = 0 overridden' },
    @{ api = 'd3d11'; layout = 'two';   eye = '1920x1080'; fmt = 'srgb';  codec = 'hevc'; sound = $false; yt = 'tb_half';  ytshort = 0; out = '7680x4320'; fps = 72; rendered = '7680x4320'; taps = 1;
       ytlog = 'eye 1920x1080 -> crop 16:9 1920x1080 -> tb_half frame 7680x4320 (each eye 7680x2160; short side 4320, long side <= 8192), st3d top-bottom' }
)
if ($Quick) { $cases = $cases[0..1] }

$results = @()
foreach ($c in $cases) {
    $name = "{0}_{1}_{2}_{3}_{4}" -f $c.api, $c.layout, $c.eye, $c.fmt, $c.codec
    if ($c.smooth) { $name += '_smooth' }
    if ($c.raw -and -not $c.yt) { $name += '_raw' }
    if ($c.noav1) { $name += '_noav1' }
    if ($c.yt) { $name += "_yt_$($c.yt)"; if ($c.crop) { $name += '_' + $c.crop.Replace(':', 'x') }; if ($c.ytshort) { $name += "_s$($c.ytshort)" }; if ($c.raw) { $name += '_nosym' } }
    if ($c.mw) { $name += "_mw$($c.mw)" }
    if ($c.fps) { $name += "_$($c.fps)fps" }
    $out = Join-Path $outDir "$name.mp4"
    $targs = @('--api', $c.api, '--layout', $c.layout, '--eye', $c.eye, '--format', $c.fmt, '--codec', $c.codec, '--out', $out)
    if (-not $c.sound) { $targs += '--no-tone' }
    if ($c.smooth) { $targs += @('--head-smoothing', $c.smooth, '--fov-scale', $c.fov) }
    if ($c.fps) { $targs += @('--fps', "$($c.fps)") }
    if ($c.raw) { $targs += '--no-symmetric' }
    if ($c.mw) { $targs += @('--max-width', "$($c.mw)") }
    if ($c.yt) { $targs += @('--youtube', $c.yt); if ($c.crop) { $targs += @('--eye-crop', $c.crop) } }
    Write-Host "`n=== $name" -ForegroundColor Cyan
    if ($c.noav1) { $env:XR_EYE_CAPTURE_TEST_NO_AV1 = '1' }
    if ($c.ytshort) { $env:XR_EYE_CAPTURE_TEST_YT_SHORT = "$($c.ytshort)" }
    $sidecar = [System.IO.Path]::ChangeExtension($out, '.json')
    Remove-Item $sidecar -ErrorAction SilentlyContinue
    $log = @(& $exe @targs)
    $runOk = ($LASTEXITCODE -eq 0)
    Remove-Item Env:\XR_EYE_CAPTURE_TEST_NO_AV1 -ErrorAction SilentlyContinue
    Remove-Item Env:\XR_EYE_CAPTURE_TEST_YT_SHORT -ErrorAction SilentlyContinue
    $log | Select-String -Pattern 'recording summary|re-projection summary|re-projection rendered|symmetric fov|YouTube layout|record_for_youtube|TEST_YT_SHORT|sidecar|SubmitFrame cost|quality vs speed|fell back|=== encoder_selftest|ERROR|WARN' | ForEach-Object { Write-Host "  $($_.Line.Substring([math]::Min(33, $_.Line.Length)))" }
    # log-line checks (each must match, else the case fails)
    $logText = $log -join "`n"
    $want = @()
    if ($c.noav1) { $want += 'AV1 unavailable -- fell back to HEVC' }
    if ($c.rendered) {
        $rx = 're-projection rendered at ' + $c.rendered + ' \('
        if ($c.taps) { $rx += '[^\r\n]*' + $c.taps + 'x' + $c.taps + ' bilinear taps per output pixel\) -- the video processor does not scale' }
        $want += $rx
    }
    if ($c.yt) {
        $want += 'recorder: YouTube layout: ' + [regex]::Escape($c.ytlog)
        $want += 'recording summary: OK [^\r\n]*\| layout YouTube ' + $c.yt + ' \(eye_crop ' + [regex]::Escape($(if ($c.crop) { $c.crop } else { '16:9' })) + ', st3d ' + $(if ($c.yt.StartsWith('tb_')) { 'top-bottom' } else { 'left-right' }) + '\)'
        if ($c.ytshort) { $want += 'XR_EYE_CAPTURE_TEST_YT_SHORT = ' + $c.ytshort + ': YouTube frame short side' }
        if ($c.ytextra) { $want += [regex]::Escape($c.ytextra) }
    }
    $logOk = $true
    foreach ($rx in $want) {
        $m = [regex]::Match($logText, $rx)
        if ($m.Success) { Write-Host "  ok   log: $rx" -ForegroundColor Green }
        else { Write-Host "  FAIL log line missing: $rx" -ForegroundColor Red; $logOk = $false }
    }
    $wh = $(if ($c.pix) { $c.pix } else { $c.eye }).Split('x')   # pix = per-eye size of the encoded frame when it is scaled
    $vargs = @{ File = $out; EyeW = [int]$wh[0]; EyeH = [int]$wh[1]; Codec = $(if ($c.expect) { $c.expect } else { $c.codec }); Format = $c.fmt; ExpectAudio = $true; ExpectSound = [bool]$c.sound }
    if ($c.fps) { $vargs.Fps = [double]$c.fps }
    if ($c.out) { $o = $c.out.Split('x'); $vargs.OutW = [int]$o[0]; $vargs.OutH = [int]$o[1]; if (-not $c.yt) { $vargs.SkipPixels = $true } }   # scaled: no pixel checks
    if ($c.yt) { $vargs.Packing = $c.yt; $vargs.EyeCrop = $(if ($c.crop) { $c.crop } else { '16:9' }) }   # packed eyes: the pixel checks know where they are
    $verOk = $false
    if ($runOk) { & (Join-Path $PSScriptRoot 'verify_mp4.ps1') @vargs; $verOk = ($LASTEXITCODE -eq 0) }
    if ($c.yt) {   # the sidecar the tray reads: exactly one line
        $o = $c.out.Split('x')
        $wantJson = '{"projection":"flat","layout":"' + $(if ($c.yt.StartsWith('tb_')) { 'top_bottom' } else { 'left_right' }) + '","packing":"' + $c.yt +
                    '","eye_crop":"' + $(if ($c.crop) { $c.crop } else { '16:9' }) + '","eyes":2,"width":' + $o[0] + ',"height":' + $o[1] + ',"youtube":1}'
        $gotJson = if (Test-Path $sidecar) { ([System.IO.File]::ReadAllText($sidecar)).TrimEnd() } else { '(missing)' }
        if ($gotJson -ceq $wantJson) { Write-Host "  ok   sidecar $gotJson" -ForegroundColor Green }
        else { Write-Host "  FAIL sidecar $gotJson (want $wantJson)" -ForegroundColor Red; $verOk = $false }
    }
    $results += [pscustomobject]@{ case = $name; run = $runOk; verify = $verOk; log = $logOk }
}
Write-Host ''
$results | Format-Table -AutoSize
if ($results | Where-Object { -not ($_.run -and $_.verify -and $_.log) }) { exit 1 }
exit 0
