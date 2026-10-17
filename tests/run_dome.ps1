<#
    run_dome.ps1 -- end-to-end test of the VR180 / VR360 dome output ([dome] projection), no headset: real OpenXR loader ->
    INSTALLED layer -> tests/mock_runtime in jitter mode (tests/test_motion.h: +-2 deg yaw / pitch / roll jitter for the
    whole run, plus a deliberate 30 deg yaw turn to the LEFT from t = 4 s to t = 5 s; asymmetric fovs left -50/+40,
    right -40/+50, +-45 deg). layer_e2e --jitter renders a world-locked grid (background 40, a 90 line every 10 deg of
    yaw / pitch) and a white marker at the world direction -Z (at infinity) for the located pose, like a game.

    Four recordings (11.5 s each, XR_EYE_CAPTURE_CONFIG overrides):
        vr180         projection = vr180 (fill last_seen, fill_fade_s 8, window_feather_deg 1: the defaults)
        vr180_black   projection = vr180, fill = black
        vr180_fade1   projection = vr180, fill_fade_s = 1
        vr360         projection = vr360 (fill last_seen)
    Each video is decoded (ffmpeg, Y plane); the marker is located per frame in both eyes (weighted centroid of Y > 150
    inside lon +-20 / lat +-10 deg around the dome centre); video time -> motion time through the layer's "first video
    frame ... displayTime" line and layer_e2e's E2E_T0_NS (same mapping as run_jitter.ps1).
    The "probe region" is lon +33 / lat 0 deg of the left eye (the lat 0 grid line runs through it): inside the left
    eye's window before the turn (right edge = 40 - head yaw + D yaw >= ~36 deg with the +-2 deg jitter), outside after
    it (the head turned 30 deg LEFT, so the right edge moves to ~+10). Painted = max Y > 60 (grid line ~93, background ~50 in limited range), black = max Y < 30 (black = 16).
    Checks:
      (a) world lock: left-eye marker centroid over the jitter-only phase t in [1.0, 3.9] s: std-dev x and y < 0.3 px
          (vr180, vr360); its mean lon / lat = the dome frame's yaw from the log (lon = +D yaw: OpenXR yaw is positive
          to the LEFT, lon positive to the RIGHT; D = head yaw at the first recorded frame) within 2 deg, lat 0 +- 2 deg.
      (b) turn: mean marker position over t in [5.5, end] = mean over [1.0, 3.9] within 0.5 px (vr180, vr360); probe
          region painted at t ~ 5.5 s in vr180 (last_seen), black in vr180_black.
      (c) fade: vr180_fade1: probe region painted at t ~ 5.5 s and black on every frame with t >= 8.5 s.
      (d) stereo: right-eye marker centroid = left-eye centroid (same equirect pixel), right half (vr180) / bottom half
          (vr360): mean |dx|, |dy| <= 0.3 px and per-frame std-dev <= 0.15 px (each eye's centroid has ~0.05 px of its
          own raster noise -- the mock draws an aliased disk at a different sub-pixel phase per eye -- so the per-frame
          max over ~1000 frames is ~0.3 px; it is printed).
      (e) tags: ffprobe on the MP4 and on the MKV made with the tray's remux command: spherical (tiled) equirectangular
          + stereo 3D side by side (vr180) / top and bottom (vr360); the exact ffprobe lines are printed. Plus the
          <name>.json sidecar and the _VR180 / _VR360 file name.
    Sets XR_EYE_CAPTURE_REQUIRE_TRAY=0 and XR_EYE_CAPTURE_TRAY_IPC=0 (a running tray must not post-process the MP4s).
      powershell -ExecutionPolicy Bypass -File tests\run_dome.ps1 [-BuildLayer]
    -BuildLayer: test build\Release\eye_capture.dll instead of the installed (registered) copy, as in run_e2e.ps1.
#>
param([string]$FfmpegDir = '', [switch]$BuildLayer)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$exe = Join-Path $bin 'layer_e2e.exe'
if ($BuildLayer) {   # the same mechanism as run_e2e.ps1 -BuildLayer: nothing is copied or registered
    $env:DISABLE_XR_APILAYER_JRDEVO_eye_capture = '1'
    $lp = Join-Path $root 'build\e2e_layer'
    New-Item -ItemType Directory -Force $lp | Out-Null
    $m = Get-Content (Join-Path $bin 'XR_APILAYER_JRDEVO_eye_capture.json') -Raw | ConvertFrom-Json
    $m.api_layer.library_path = (Join-Path $bin 'eye_capture.dll')
    $m | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $lp 'XR_APILAYER_JRDEVO_eye_capture.json') -Encoding ascii
    $env:XR_API_LAYER_PATH = $lp
    $env:XR_ENABLE_API_LAYERS = 'XR_APILAYER_JRDEVO_eye_capture'
    Write-Host "-BuildLayer: testing $bin\eye_capture.dll (registered copy disabled for these processes)" -ForegroundColor Yellow
}
if (-not (Test-Path $exe)) { throw "build first: $exe" }
if (-not $FfmpegDir) {
    $cand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffprobe.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cand) { $FfmpegDir = $cand.DirectoryName } else {
        $onPath = Get-Command ffprobe -ErrorAction SilentlyContinue
        if ($onPath) { $FfmpegDir = Split-Path $onPath.Source } else { throw 'ffprobe not found (pass -FfmpegDir)' }
    }
}
$ffmpeg = Join-Path $FfmpegDir 'ffmpeg.exe'
$ffprobe = Join-Path $FfmpegDir 'ffprobe.exe'

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
public static class DomeAnalysis {
    // Per frame: left-eye marker centroid (x, y in eye pixels; NaN if none), right-eye centroid, max Y in the probe
    // region of the left eye. tb = top-bottom layout (vr360). The marker search window is [sx0, sx1) x [sy0, sy1) in eye
    // pixels; the probe region [px0, px1) x [py0, py1).
    public static double[][] Scan(string ffmpeg, string file, int W, int H, bool tb, int sx0, int sx1, int sy0, int sy1,
                                  int px0, int px1, int py0, int py1) {
        ProcessStartInfo psi = new ProcessStartInfo(ffmpeg,
            "-v error -i \"" + file + "\" -map 0:v:0 -fps_mode passthrough -vf extractplanes=y -f rawvideo -pix_fmt gray -");
        psi.UseShellExecute = false; psi.RedirectStandardOutput = true; psi.CreateNoWindow = true;
        Process p = Process.Start(psi);
        Stream s = p.StandardOutput.BaseStream;
        byte[] f = new byte[W * H];
        int eyeW = tb ? W : W / 2, eyeH = tb ? H / 2 : H;
        System.Collections.Generic.List<double[]> res = new System.Collections.Generic.List<double[]>();
        while (true) {
            int got = 0;
            while (got < f.Length) { int n = s.Read(f, got, f.Length - got); if (n <= 0) break; got += n; }
            if (got < f.Length) break;
            double[] r = new double[5];
            for (int e = 0; e < 2; e++) {
                int ox = tb ? 0 : e * eyeW, oy = tb ? e * eyeH : 0;
                double sw = 0, sx = 0, sy = 0;
                for (int y = sy0; y < sy1; y++) {
                    int row = (oy + y) * W + ox;
                    for (int x = sx0; x < sx1; x++) {
                        int v = f[row + x];
                        if (v > 150) { double w = v - 150; sw += w; sx += w * x; sy += w * y; }
                    }
                }
                r[2 * e] = sw > 0 ? sx / sw : double.NaN;
                r[2 * e + 1] = sw > 0 ? sy / sw : double.NaN;
            }
            int mx = 0;
            for (int y = py0; y < py1; y++) for (int x = px0; x < px1; x++) { int v = f[y * W + x]; if (v > mx) mx = v; }
            r[4] = mx;
            res.Add(r);
        }
        p.WaitForExit();
        return res.ToArray();
    }
}
'@

$cases = @(
    @{ name = 'vr180';       cfg = 'projection=vr180';                  proj = 'vr180' },
    @{ name = 'vr180_black'; cfg = 'projection=vr180;fill=black';       proj = 'vr180' },
    @{ name = 'vr180_fade1'; cfg = 'projection=vr180;fill_fade_s=1';    proj = 'vr180' },
    @{ name = 'vr360';       cfg = 'projection=vr360';                  proj = 'vr360' }
)
$prevTray = $env:XR_EYE_CAPTURE_REQUIRE_TRAY; $prevCfg = $env:XR_EYE_CAPTURE_CONFIG; $prevIpc = $env:XR_EYE_CAPTURE_TRAY_IPC
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
$env:XR_EYE_CAPTURE_TRAY_IPC = '0'   # a running tray must not post-process (and delete) the test recordings
$data = @{}
$files = @{}
$fails = New-Object System.Collections.Generic.List[string]
function Check($ok, $what) {
    if ($ok) { Write-Host "  ok   $what" -ForegroundColor Green } else { Write-Host "  FAIL $what" -ForegroundColor Red; $fails.Add($what) }
}
$probeLon = 33.0; $probeLat = 0.0
foreach ($c in $cases) {
    $out = Join-Path $root "build\e2e_out\dome_$($c.name)"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    $env:XR_EYE_CAPTURE_CONFIG = $c.cfg
    Write-Host "`n=== dome / $($c.name): $($c.cfg)" -ForegroundColor Cyan
    $o = & $exe --jitter --seconds 12 --record 11.5 --out-dir $out 2>&1 | Out-String
    $mOut = [regex]::Match($o, 'E2E_OUTPUT=(.+)'); $mT0 = [regex]::Match($o, 'E2E_T0_NS=(\d+)')
    if ($LASTEXITCODE -ne 0 -or -not $mOut.Success -or -not $mT0.Success) { Write-Host $o; Check $false "$($c.name): e2e run"; continue }
    $file = $mOut.Groups[1].Value.Trim()
    $files[$c.name] = $file
    $log = Get-Content (Join-Path $out 'eye_capture.log') -Raw -Encoding UTF8
    foreach ($pat in @('recorder: dome vr\d+: [^\r\n]+', 'recorder: dome pass[^\r\n]+', 'dome frame D fixed[^\r\n]+', 're-projection summary[^\r\n]+', 'st3d\+sv3d: [^|\r\n]+')) {
        foreach ($mm in [regex]::Matches($log, $pat)) { Write-Host "  [$($c.name)] $($mm.Value)" }
    }
    $mFirst = [regex]::Match($log, 'first video frame \(t = 0 in the file\): displayTime (\d+) ns')
    $mYaw = [regex]::Match($log, 'dome frame D fixed[^\r\n]*?yaw ([+-]?[\d.]+) deg')
    if (-not $mFirst.Success -or -not $mYaw.Success) { Check $false "$($c.name): first-frame displayTime / D yaw in the log"; continue }
    $offset = ([double]$mFirst.Groups[1].Value - [double]$mT0.Groups[1].Value) / 1e9
    $probe = & $ffprobe -v error -print_format json -show_streams $file | Out-String | ConvertFrom-Json
    $v = $probe.streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
    $W = [int]$v.width; $H = [int]$v.height
    $tb = $c.proj -eq 'vr360'
    $eyeW = if ($tb) { $W } else { [int]($W / 2) }; $eyeH = if ($tb) { [int]($H / 2) } else { $H }
    $span = if ($tb) { 360.0 } else { 180.0 }
    $pxPerDeg = $eyeW / $span
    # marker search window: lon +-20, lat +-10 deg around the centre; probe region lon 36 +- 0.5, lat 0 +- 0.8 deg
    $sx0 = [int]($eyeW / 2 - 20 * $pxPerDeg); $sx1 = [int]($eyeW / 2 + 20 * $pxPerDeg)
    $sy0 = [int]($eyeH / 2 - 10 * $pxPerDeg); $sy1 = [int]($eyeH / 2 + 10 * $pxPerDeg)
    $pcx = ($probeLon / $span + 0.5) * $eyeW - 0.5; $pcy = (0.5 - $probeLat / 180.0) * $eyeH - 0.5
    $px0 = [int][math]::Floor($pcx - 0.5 * $pxPerDeg); $px1 = [int][math]::Ceiling($pcx + 0.5 * $pxPerDeg) + 1
    $py0 = [int][math]::Floor($pcy - 0.8 * $pxPerDeg); $py1 = [int][math]::Ceiling($pcy + 0.8 * $pxPerDeg) + 1
    $pts = & $ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 $file | Where-Object { $_ -match '\d' } |
        ForEach-Object { [double]($_.Trim().TrimEnd(',')) } | Sort-Object
    $scan = [DomeAnalysis]::Scan($ffmpeg, $file, $W, $H, $tb, $sx0, $sx1, $sy0, $sy1, $px0, $px1, $py0, $py1)
    $n = [math]::Min($scan.Length, $pts.Count)
    Write-Host ("  {0} frames {1}x{2} ({3}x{4} per eye, {5:N2} px/deg), motion t of frame 0 = {6:N3} s, D yaw {7} deg; probe region (lon {8}, lat {9}) = x [{10},{11}) y [{12},{13}) of the left eye" -f
        $n, $W, $H, $eyeW, $eyeH, $pxPerDeg, $offset, $mYaw.Groups[1].Value, $probeLon, $probeLat, $px0, $px1, $py0, $py1)
    $rows = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $n; $i++) {
        $r = $scan[$i]
        $rows.Add([pscustomobject]@{ t = $pts[$i] + $offset; xl = $r[0]; yl = $r[1]; xr = $r[2]; yr = $r[3]; probe = $r[4] })
    }
    $data[$c.name] = @{ rows = $rows; eyeW = $eyeW; eyeH = $eyeH; span = $span; yaw = [double]$mYaw.Groups[1].Value; file = $file; tb = $tb }
}
if ($null -ne $prevTray) { $env:XR_EYE_CAPTURE_REQUIRE_TRAY = $prevTray } else { Remove-Item Env:\XR_EYE_CAPTURE_REQUIRE_TRAY -ErrorAction SilentlyContinue }
if ($null -ne $prevCfg) { $env:XR_EYE_CAPTURE_CONFIG = $prevCfg } else { Remove-Item Env:\XR_EYE_CAPTURE_CONFIG -ErrorAction SilentlyContinue }
if ($null -ne $prevIpc) { $env:XR_EYE_CAPTURE_TRAY_IPC = $prevIpc } else { Remove-Item Env:\XR_EYE_CAPTURE_TRAY_IPC -ErrorAction SilentlyContinue }

function Stats($rows, $t0, $t1) {
    $j = @($rows | Where-Object { $_.t -ge $t0 -and $_.t -le $t1 -and -not [double]::IsNaN($_.xl) })
    $mx = ($j | Measure-Object xl -Average).Average; $my = ($j | Measure-Object yl -Average).Average
    $vx = 0; $vy = 0; foreach ($r in $j) { $vx += ($r.xl - $mx) * ($r.xl - $mx); $vy += ($r.yl - $my) * ($r.yl - $my) }
    $cnt = [math]::Max(1, $j.Count)
    $miss = @($rows | Where-Object { $_.t -ge $t0 -and $_.t -le $t1 -and [double]::IsNaN($_.xl) }).Count
    return @{ mx = $mx; my = $my; sx = [math]::Sqrt($vx / $cnt); sy = [math]::Sqrt($vy / $cnt); n = $j.Count; miss = $miss }
}

Write-Host "`n=== checks" -ForegroundColor Cyan
# (a) world lock + (b) marker after the turn
foreach ($name in @('vr180', 'vr360')) {
    if (-not $data.ContainsKey($name)) { continue }
    $d = $data[$name]
    $a = Stats $d.rows 1.0 3.9
    $lon = (($a.mx + 0.5) / $d.eyeW - 0.5) * $d.span; $lat = (0.5 - ($a.my + 0.5) / $d.eyeH) * 180
    Check ($a.sx -lt 0.3 -and $a.sy -lt 0.3 -and $a.n -gt 200 -and $a.miss -eq 0) ("(a) {0}: world lock, left-eye marker std-dev over t 1.0..3.9 s: x {1:N3} px, y {2:N3} px ({3} frames, marker missing in {4})" -f $name, $a.sx, $a.sy, $a.n, $a.miss)
    Check ([math]::Abs($lon - $d.yaw) -le 2 -and [math]::Abs($lat) -le 2) ("(a) {0}: marker sits at lon {1:N3} deg, lat {2:N3} deg (mean pixel {3:N2}, {4:N2}); D yaw {5:N2} deg -> expected lon {5:N2}, lat 0 (diff {6:N3} deg)" -f $name, $lon, $lat, $a.mx, $a.my, $d.yaw, ($lon - $d.yaw))
    $tEnd = ($d.rows | Measure-Object t -Maximum).Maximum
    $b = Stats $d.rows 5.5 $tEnd
    Check ([math]::Abs($b.mx - $a.mx) -le 0.5 -and [math]::Abs($b.my - $a.my) -le 0.5 -and $b.miss -eq 0) ("(b) {0}: marker after the 30 deg turn (t 5.5..{1:N2} s, {2} frames) at ({3:N3}, {4:N3}) vs before ({5:N3}, {6:N3}): dx {7:N3} px, dy {8:N3} px; std-dev after {9:N3} / {10:N3} px" -f $name, $tEnd, $b.n, $b.mx, $b.my, $a.mx, $a.my, ($b.mx - $a.mx), ($b.my - $a.my), $b.sx, $b.sy)
}
# (b) probe region: last_seen keeps it, black does not
function ProbeAt($rows, $t0, $t1) { return @($rows | Where-Object { $_.t -ge $t0 -and $_.t -le $t1 } | ForEach-Object { $_.probe }) }
foreach ($name in @('vr180', 'vr180_black', 'vr180_fade1')) {
    if (-not $data.ContainsKey($name)) { continue }
    $rows = $data[$name].rows
    $before = ProbeAt $rows 1.0 3.9
    $minB = ($before | Measure-Object -Minimum).Minimum
    Check ($minB -gt 60) ("{0}: probe region (lon +33, lat 0) live before the turn: min of max-Y over t 1.0..3.9 s = {1}" -f $name, $minB)
    $at = ProbeAt $rows 5.45 5.65
    $mn = ($at | Measure-Object -Minimum).Minimum; $mxv = ($at | Measure-Object -Maximum).Maximum
    if ($name -eq 'vr180_black') {
        Check ($at.Count -gt 0 -and $mxv -lt 30) ("(b) {0}: probe region outside the window after the turn is BLACK at t 5.45..5.65 s: max-Y {1}..{2} over {3} frames (< 30)" -f $name, $mn, $mxv, $at.Count)
    } else {
        Check ($at.Count -gt 0 -and $mn -gt 60) ("({0}) {1}: probe region outside the window after the turn is still PAINTED at t 5.45..5.65 s: max-Y {2}..{3} over {4} frames (> 60)" -f $(if ($name -eq 'vr180') { 'b' } else { 'c' }), $name, $mn, $mxv, $at.Count)
    }
}
# (c) fade
if ($data.ContainsKey('vr180_fade1')) {
    $rows = $data['vr180_fade1'].rows
    $late = @($rows | Where-Object { $_.t -ge 8.5 })
    $worst = ($late | Measure-Object probe -Maximum).Maximum
    $lastPainted = ($rows | Where-Object { $_.probe -gt 60 } | Measure-Object t -Maximum).Maximum
    $firstBlack = ($rows | Where-Object { $_.t -gt 4.0 -and $_.probe -lt 30 } | Measure-Object t -Minimum).Minimum
    Check ($late.Count -gt 100 -and $worst -lt 30) ("(c) vr180_fade1: probe region BLACK on every frame with t >= 8.5 s: max-Y {0} over {1} frames (< 30); last painted (> 60) at t {2:N2} s, first black (< 30) at t {3:N2} s" -f $worst, $late.Count, $lastPainted, $firstBlack)
    if ($data.ContainsKey('vr180')) {
        $l2 = @($data['vr180'].rows | Where-Object { $_.t -ge 8.5 -and $_.t -le 9.5 })
        Write-Host ("  info vr180 (fill_fade_s 8): probe region at t 8.5..9.5 s max-Y min {0} (still painted, fades only from ~12 s)" -f ($l2 | Measure-Object probe -Minimum).Minimum)
    }
}
# (d) stereo: both eyes put the far marker on the same equirect pixel
foreach ($name in @('vr180', 'vr180_black', 'vr180_fade1', 'vr360')) {
    if (-not $data.ContainsKey($name)) { continue }
    $both = @($data[$name].rows | Where-Object { -not [double]::IsNaN($_.xl) -and -not [double]::IsNaN($_.xr) })
    $dx = @($both | ForEach-Object { $_.xr - $_.xl }); $dy = @($both | ForEach-Object { $_.yr - $_.yl })
    $mdx = ($dx | ForEach-Object { [math]::Abs($_) } | Measure-Object -Maximum).Maximum
    $mdy = ($dy | ForEach-Object { [math]::Abs($_) } | Measure-Object -Maximum).Maximum
    $avx = ($dx | Measure-Object -Average).Average; $avy = ($dy | Measure-Object -Average).Average
    $sdx = 0; foreach ($v in $dx) { $sdx += ($v - $avx) * ($v - $avx) }; $sdx = [math]::Sqrt($sdx / [math]::Max(1, $dx.Count))
    $sdy = 0; foreach ($v in $dy) { $sdy += ($v - $avy) * ($v - $avy) }; $sdy = [math]::Sqrt($sdy / [math]::Max(1, $dy.Count))
    $total = $data[$name].rows.Count
    # The per-frame difference carries each eye's own raster noise (the mock draws an aliased disk at a different
    # sub-pixel phase per eye, ~0.05 px std per eye): the alignment is the mean, the spread must stay at noise level.
    Check ($both.Count -eq $total -and [math]::Abs($avx) -le 0.3 -and [math]::Abs($avy) -le 0.3 -and $sdx -le 0.15 -and $sdy -le 0.15) ("(d) {0}: right-eye marker - left-eye marker ({1}): mean dx {2:N3} / dy {3:N3} px (<= 0.3), per-frame std-dev {4:N3} / {5:N3} px (<= 0.15), per-frame max |dx| {6:N3}, max |dy| {7:N3} px, over {8} of {9} frames" -f $name, $(if ($data[$name].tb) { 'bottom - top' } else { 'right half - left half' }), $avx, $avy, $sdx, $sdy, $mdx, $mdy, $both.Count, $total)
}
# (e) tags: MP4 boxes, the tray's MKV remux, sidecar, file name
foreach ($name in @('vr180', 'vr360')) {
    if (-not $files.ContainsKey($name)) { continue }
    $file = $files[$name]
    $tag = if ($name -eq 'vr180') { 'VR180' } else { 'VR360' }
    $stereoTag = if ($name -eq 'vr180') { 'left_right' } else { 'top_bottom' }
    $wantStereo = if ($name -eq 'vr180') { 'side by side' } else { 'top and bottom' }
    $wantProj = if ($name -eq 'vr180') { 'tiled equirectangular' } else { 'equirectangular' }
    Check ([System.IO.Path]::GetFileName($file) -match "_$tag\.mp4$") ("(e) {0}: file name {1}" -f $name, [System.IO.Path]::GetFileName($file))
    $json = [System.IO.Path]::ChangeExtension($file, '.json')
    $jtxt = if (Test-Path $json) { (Get-Content $json -Raw).Trim() } else { '' }
    Check ($jtxt -match "`"projection`":`"$name`"" -and $jtxt -match "`"layout`":`"$stereoTag`"") ("(e) {0}: sidecar {1}: {2}" -f $name, [System.IO.Path]::GetFileName($json), $jtxt)
    $mkv = [System.IO.Path]::ChangeExtension($file, '.mkv')
    & $ffmpeg -hide_banner -loglevel error -y -i $file -map 0 -c copy -metadata:s:v:0 "stereo_mode=$stereoTag" $mkv   # = the tray's MKV command
    foreach ($f in @($file, $mkv)) {
        $lines = @(& $ffprobe -v error -select_streams v:0 -show_entries 'stream_side_data:stream_tags=stereo_mode' -of default=nw=1 $f)
        Write-Host ("  ffprobe -v error -select_streams v:0 -show_entries stream_side_data {0}" -f [System.IO.Path]::GetFileName($f))
        $lines | ForEach-Object { Write-Host "      $_" }
        $txt = $lines -join "`n"
        $projOk = $txt -match "(?m)^projection=$([regex]::Escape($wantProj))$"
        $stOk = $txt -match "(?m)^type=$([regex]::Escape($wantStereo))$"
        $bOk = if ($name -eq 'vr180') { ($txt -match '(?m)^bound_left=[1-9]') -and ($txt -match '(?m)^bound_right=[1-9]') -and ($txt -match '(?m)^bound_top=0$') } else { $true }
        Check ($projOk -and $stOk -and $bOk) ("(e) {0}: {1}: Spherical Mapping '{2}'{3} + Stereo 3D '{4}'" -f $name, [System.IO.Path]::GetExtension($f), $wantProj, $(if ($name -eq 'vr180') { ' with left / right bounds' } else { '' }), $wantStereo)
    }
    # the Matroska Projection master element (ID 0x7670) with ProjectionType (0x7671) in the MKV header
    $head = [System.IO.File]::ReadAllBytes($mkv)
    $lim = [math]::Min($head.Length, 1048576)
    $at = -1
    for ($i = 0; $i -lt $lim - 4; $i++) { if ($head[$i] -eq 0x76 -and $head[$i + 1] -eq 0x70) { for ($k = $i + 2; $k -lt [math]::Min($i + 16, $lim - 2); $k++) { if ($head[$k] -eq 0x76 -and $head[$k + 1] -eq 0x71) { $at = $i; $pt = $head[$k + 3]; break } }; if ($at -ge 0) { break } } }
    Check ($at -ge 0 -and $pt -eq 1) ("(e) {0}: MKV has the Matroska Projection element at byte {1}, ProjectionType {2} (1 = equirectangular)" -f $name, $at, $pt)
}
if ($fails.Count -eq 0) { Write-Host "`nPASS dome" -ForegroundColor Green; exit 0 }
Write-Host "`nFAIL dome ($($fails.Count) checks)" -ForegroundColor Red
$fails | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
exit 1
