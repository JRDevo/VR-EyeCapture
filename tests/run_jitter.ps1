<#
    run_jitter.ps1 -- end-to-end test of head-motion smoothing and symmetric_fov, no headset: real OpenXR loader -> INSTALLED layer ->
    tests/mock_runtime in jitter mode (head motion of tests/test_motion.h: +-2 deg yaw/pitch/roll jitter at 3.3/4.1/4.7 Hz
    for the whole run, plus a deliberate 30 deg yaw turn from t = 4 s to t = 5 s; asymmetric per-eye fov like a real
    headset: left -50/+40, right -40/+50, +-45 deg). layer_e2e --jitter renders a world-locked grid with a white marker
    (a direction, i.e. at infinity) for the located pose every frame, like a game.

    Six recordings:    raw      head_smoothing = 0  (symmetric_fov = 1, the default: output +-40 x +-45 deg per eye)
                       rawasym  head_smoothing = 0, symmetric_fov = 0      (stage bypassed: the raw eye images)
                       smooth   head_smoothing = 0.6, output_fov_scale = 0.9    (edge_guard = 1, the default)
                       noguard  same with edge_guard = 0                         (pure filter + follow)
                       hudraw   as raw, plus layer_e2e --hud: a green quad in a VIEW space (head-locked, like the DLAA
                                injector's panel) and a magenta quad in the LOCAL space (world-locked)
                       hudsmooth as smooth, plus --hud
                       small    as raw with max_width = 1280: the re-projection renders straight at 1280 wide (2 x 2
                                bilinear taps per output pixel), the video processor does not scale
    Each video is decoded (ffmpeg, Y plane), the marker is located in each eye per frame (weighted centroid of
    Y > 150) and converted to camera angles with the left eye's output fov (symmetric x scale, or the raw left fov). Video time -> motion time through the
    layer's "first video frame ... displayTime" log line and layer_e2e's E2E_T0_NS.
    Checks:
      raw     marker = the analytic expectation for the mock pose (RMS < 0.15 deg)   -> the test scene is right
              (rawasym: same with the raw left-eye fov)
      stereo  raw (symmetric): the far marker is at the same x in both halves (max |xL - xR| <= 1 px);
              rawasym: it is not (documents the bug symmetric_fov fixes)
      ratio   std-dev of the marker position, jitter-only phase t in [1.0, 3.9] s: raw / smooth >= 5
      settle  smooth and noguard: after the turn ends (t = 5 s) the marker (0.1 s moving average) stays within 1 deg
              of its final rest position from t <= 5.5 s on; final rest position within 0.5 deg of 30 deg / 0 deg
      edges   raw, rawasym, smooth: no black pixels (Y < 30) in a 3 px border of either eye, in the jitter phase AND over
              the whole recording (turn and catch-up included); smooth: the layer's summary reports 0 uncovered frames
              and the edge guard acted (> 0 frames). noguard (edge_guard = 0) is info only: black edges are expected.
      small   the MP4 is 1280 wide (even height, the 2328x1440 aspect), the layer logs "re-projection rendered at 1280x..."
              and its summary line reports that size; the marker follows the mock pose (RMS < 0.3 deg: one output pixel is
              about 0.13 deg here), same x in both eyes (<= 1 px), no black border pixels.
      hud     (RGB decode; per eye: green / magenta / white centroids) hudraw and hudsmooth: the VIEW-space quad is in
              every frame of both eyes and its centroid stays within 1 px of its median over the WHOLE recording (jitter,
              turn, catch-up) -- with smoothing it follows the smoothed camera (OverlayHeadLock), a steady HUD; the
              world-locked quad keeps its angular distance to the world marker (std-dev <= 0.05 deg, jitter phase, both
              eyes), i.e. it moves with the world exactly like the rendered scene.
    The recording cases set XR_EYE_CAPTURE_REQUIRE_TRAY=0 and XR_EYE_CAPTURE_CONFIG (test-only overrides; projection=flat
    pins the flat path whatever config.ini says).
      powershell -ExecutionPolicy Bypass -File tests\run_jitter.ps1 [-BuildLayer] [-OnlyHud]
      -BuildLayer: test build\Release\eye_capture.dll instead of the installed (registered) copy, as in run_e2e.ps1.
      -OnlyHud: run only the hud cases.
#>
param([string]$FfmpegDir = '', [switch]$BuildLayer, [switch]$OnlyHud)
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
public static class JitterAnalysis {
    // Per frame: marker centroid (x, y in left-eye pixels, NaN if none) and black border pixel count (both eyes).
    public static double[][] Scan(string ffmpeg, string file, int W, int H, int eyeW) {
        ProcessStartInfo psi = new ProcessStartInfo(ffmpeg,
            "-v error -i \"" + file + "\" -map 0:v:0 -fps_mode passthrough -vf extractplanes=y -f rawvideo -pix_fmt gray -");
        psi.UseShellExecute = false; psi.RedirectStandardOutput = true; psi.CreateNoWindow = true;
        Process p = Process.Start(psi);
        Stream s = p.StandardOutput.BaseStream;
        byte[] f = new byte[W * H];
        System.Collections.Generic.List<double[]> res = new System.Collections.Generic.List<double[]>();
        while (true) {
            int got = 0;
            while (got < f.Length) { int n = s.Read(f, got, f.Length - got); if (n <= 0) break; got += n; }
            if (got < f.Length) break;
            double sw = 0, sx = 0, sy = 0, rw = 0, rx = 0;
            for (int y = 0; y < H; y++) {
                int row = y * W;
                for (int x = 0; x < eyeW; x++) {
                    int v = f[row + x];
                    if (v > 150) { double w = v - 150; sw += w; sx += w * x; sy += w * y; }
                    int v2 = f[row + eyeW + x];
                    if (v2 > 150) { double w = v2 - 150; rw += w; rx += w * x; }
                }
            }
            int black = 0;
            int[] edgesX = new int[] { 0, eyeW - 3, eyeW, W - 3 };
            for (int y = 0; y < H; y++) {
                int row = y * W;
                bool edgeRow = y < 3 || y >= H - 3;
                if (edgeRow) { for (int x = 0; x < W; x++) if (f[row + x] < 30) black++; }
                else foreach (int x0 in edgesX) for (int x = x0; x < x0 + 3; x++) if (f[row + x] < 30) black++;
            }
            res.Add(new double[] { sw > 0 ? sx / sw : double.NaN, sw > 0 ? sy / sw : double.NaN, black, rw > 0 ? rx / rw : double.NaN });
        }
        p.WaitForExit();
        return res.ToArray();
    }
    // hud cases (RGB): per frame, per eye e (x relative to the eye): [6e + 0..1] green (VIEW quad) centroid, [6e + 2..3]
    // magenta (world quad) centroid, [6e + 4..5] white (marker) centroid; NaN when absent. Weighted by the distance
    // past the colour threshold, so edge pixels of the 4:2:0 chroma count less.
    public static double[][] ScanHud(string ffmpeg, string file, int W, int H, int eyeW) {
        ProcessStartInfo psi = new ProcessStartInfo(ffmpeg,
            "-v error -i \"" + file + "\" -map 0:v:0 -fps_mode passthrough " +
            "-vf scale=in_color_matrix=bt709:in_range=limited:out_range=full,format=rgb24 -f rawvideo -pix_fmt rgb24 -");
        psi.UseShellExecute = false; psi.RedirectStandardOutput = true; psi.CreateNoWindow = true;
        Process p = Process.Start(psi);
        Stream s = p.StandardOutput.BaseStream;
        byte[] f = new byte[W * H * 3];
        System.Collections.Generic.List<double[]> res = new System.Collections.Generic.List<double[]>();
        while (true) {
            int got = 0;
            while (got < f.Length) { int n = s.Read(f, got, f.Length - got); if (n <= 0) break; got += n; }
            if (got < f.Length) break;
            double[] r = new double[12];
            for (int e = 0; e < 2; e++) {
                double[] sw = new double[3], sx = new double[3], sy = new double[3];
                for (int y = 0; y < H; y++) {
                    int o = (y * W + e * eyeW) * 3;
                    for (int x = 0; x < eyeW; x++, o += 3) {
                        int R = f[o], G = f[o + 1], B = f[o + 2];
                        int k = -1; double w = 0;
                        if (G > 150 && R < 100 && B < 100) { k = 0; w = G - 150; }
                        else if (R > 150 && B > 150 && G < 100) { k = 1; w = Math.Min(R, B) - 150; }
                        else if (R > 200 && G > 200 && B > 200) { k = 2; w = Math.Min(R, Math.Min(G, B)) - 200; }
                        if (k < 0) continue;
                        sw[k] += w; sx[k] += w * x; sy[k] += w * y;
                    }
                }
                for (int k = 0; k < 3; k++) {
                    r[6 * e + 2 * k] = sw[k] > 0 ? sx[k] / sw[k] : double.NaN;
                    r[6 * e + 2 * k + 1] = sw[k] > 0 ? sy[k] / sw[k] : double.NaN;
                }
            }
            res.Add(r);
        }
        p.WaitForExit();
        return res.ToArray();
    }
    // tests/test_motion.h: the marker (world -Z) as seen by the head camera at motion time t -> (atan x_tan, atan y_tan) deg
    public static double[] Expected(double t) {
        double D = Math.PI / 180;
        double yaw = 2 * Math.Sin(2 * Math.PI * 3.3 * t), pitch = 2 * Math.Sin(2 * Math.PI * 4.1 * t + 1.0), roll = 2 * Math.Sin(2 * Math.PI * 4.7 * t + 2.0);
        if (t >= 5.0) yaw += 30; else if (t > 4.0) yaw += 30 * (0.5 - 0.5 * Math.Cos(Math.PI * (t - 4.0)));
        // camera-from-world = (Ry Rx Rz)^-1 = Rz^-1 Rx^-1 Ry^-1 applied to (0,0,-1)
        double a = -yaw * D, b = -pitch * D, c = -roll * D;
        double x = -Math.Sin(a), y = 0, z = -Math.Cos(a);                    // Ry(a) * (0,0,-1)
        double y2 = y * Math.Cos(b) - z * Math.Sin(b), z2 = y * Math.Sin(b) + z * Math.Cos(b);   // Rx(b)
        y = y2; z = z2;
        double x3 = x * Math.Cos(c) - y * Math.Sin(c), y3 = x * Math.Sin(c) + y * Math.Cos(c);   // Rz(c)
        x = x3; y = y3;
        return new double[] { Math.Atan(x / -z) / D, Math.Atan(y / -z) / D };
    }
}
'@

$D = [math]::PI / 180
$rawTans = @([math]::Tan(-50 * $D), [math]::Tan(40 * $D), [math]::Tan(45 * $D), [math]::Tan(-45 * $D))   # left eye, raw
$symTans = @(-[math]::Tan(40 * $D), [math]::Tan(40 * $D), [math]::Tan(45 * $D), -[math]::Tan(45 * $D))   # common symmetric
$cases = @(
    @{ name = 'raw';     cfg = 'projection=flat;head_smoothing=0';                                     k = 1.0; tans = $symTans },
    @{ name = 'rawasym'; cfg = 'projection=flat;head_smoothing=0;symmetric_fov=0';                     k = 1.0; tans = $rawTans },
    @{ name = 'smooth';  cfg = 'projection=flat;head_smoothing=0.6;output_fov_scale=0.9';              k = 0.9; tans = $symTans },
    @{ name = 'noguard'; cfg = 'projection=flat;head_smoothing=0.6;output_fov_scale=0.9;edge_guard=0'; k = 0.9; tans = $symTans },
    @{ name = 'hudraw';    cfg = 'projection=flat;head_smoothing=0';                        k = 1.0; tans = $symTans; hud = $true },
    @{ name = 'hudsmooth'; cfg = 'projection=flat;head_smoothing=0.6;output_fov_scale=0.9'; k = 0.9; tans = $symTans; hud = $true },
    @{ name = 'small';   cfg = 'projection=flat;head_smoothing=0;max_width=1280';                      k = 1.0; tans = $symTans }
)
if ($OnlyHud) { $cases = @($cases | Where-Object { $_.hud }) }
$hudData = @{}
$dims = @{}
$logs = @{}
$prevTray = $env:XR_EYE_CAPTURE_REQUIRE_TRAY; $prevCfg = $env:XR_EYE_CAPTURE_CONFIG; $prevIpc = $env:XR_EYE_CAPTURE_TRAY_IPC
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
$env:XR_EYE_CAPTURE_TRAY_IPC = '0'   # a running tray must not post-process (and delete) the test recordings
$data = @{}
$summ = @{}
$fails = New-Object System.Collections.Generic.List[string]
function Check($ok, $what) {
    if ($ok) { Write-Host "  ok   $what" -ForegroundColor Green } else { Write-Host "  FAIL $what" -ForegroundColor Red; $fails.Add($what) }
}
foreach ($c in $cases) {
    $out = Join-Path $root "build\e2e_out\jitter_$($c.name)"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    $env:XR_EYE_CAPTURE_CONFIG = $c.cfg
    Write-Host "`n=== jitter / $($c.name): $($c.cfg)" -ForegroundColor Cyan
    $extra = @(); if ($c.hud) { $extra = @('--hud') }
    $o = & $exe --jitter @extra --seconds 9 --record 8.5 --out-dir $out 2>&1 | Out-String
    $mOut = [regex]::Match($o, 'E2E_OUTPUT=(.+)'); $mT0 = [regex]::Match($o, 'E2E_T0_NS=(\d+)')
    if ($LASTEXITCODE -ne 0 -or -not $mOut.Success -or -not $mT0.Success) { Write-Host $o; Check $false "$($c.name): e2e run"; continue }
    $file = $mOut.Groups[1].Value.Trim()
    $log = Get-Content (Join-Path $out 'eye_capture.log') -Raw -Encoding UTF8
    $mFirst = [regex]::Match($log, 'first video frame \(t = 0 in the file\): displayTime (\d+) ns')
    $mSum = [regex]::Match($log, 're-projection summary[^\r\n]+')
    $mSym = [regex]::Match($log, 'symmetric fov[^\r\n]+')
    if ($mSym.Success) { Write-Host "  [$($c.name)] $($mSym.Value)" }
    if ($mSum.Success) {
        Write-Host "  [$($c.name)] $($mSum.Value)"
        $g = [regex]::Match($mSum.Value, 'edge guard limited the correction in (\d+) frames')
        $u = [regex]::Match($mSum.Value, 'did not cover the output \(black edges\) in (\d+) frames')
        $summ[$c.name] = @{ guarded = [int]$g.Groups[1].Value; uncovered = [int]$u.Groups[1].Value; line = $mSum.Value }
    }
    if (-not $mFirst.Success) { Check $false "$($c.name): first-frame displayTime in the log"; continue }
    $offset = ([double]$mFirst.Groups[1].Value - [double]$mT0.Groups[1].Value) / 1e9
    $probe = & $ffprobe -v error -print_format json -show_streams $file | Out-String | ConvertFrom-Json
    $v = $probe.streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
    $W = [int]$v.width; $H = [int]$v.height; $eyeW = [int]($W / 2)
    $dims[$c.name] = @($W, $H); $logs[$c.name] = $log
    $pts = & $ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 $file | Where-Object { $_ -match '\d' } |
        ForEach-Object { [double]($_.Trim().TrimEnd(',')) } | Sort-Object
    $k = $c.k
    $tanL = $c.tans[0]; $tanR = $c.tans[1]; $tanU = $c.tans[2]; $tanD = $c.tans[3]
    if ($c.hud) {
        $hs = [JitterAnalysis]::ScanHud($ffmpeg, $file, $W, $H, $eyeW)
        $n = [math]::Min($hs.Length, $pts.Count)
        Write-Host ("  {0} frames {1}x{2} (RGB scan), motion t of frame 0 = {3:N3} s" -f $n, $W, $H, $offset)
        # pixel centroid -> unit camera ray of the output camera (left or right eye: same symmetric tans)
        $ray = {
            param($px, $py)
            $xt = $k * ($tanL + ($px + 0.5) / $eyeW * ($tanR - $tanL)); $yt = $k * ($tanU + ($py + 0.5) / $H * ($tanD - $tanU))
            $l = [math]::Sqrt($xt * $xt + $yt * $yt + 1); return @(($xt / $l), ($yt / $l), (-1 / $l))
        }
        $rows = New-Object System.Collections.Generic.List[object]
        for ($i = 0; $i -lt $n; $i++) {
            $r = $hs[$i]
            $sep = @([double]::NaN, [double]::NaN); $sepV = @([double]::NaN, [double]::NaN)
            foreach ($e in 0, 1) {
                $b = 6 * $e
                if ([double]::IsNaN($r[$b + 4])) { continue }
                $mk = & $ray $r[$b + 4] $r[$b + 5]
                foreach ($pair in @(@(2, 'w'), @(0, 'v'))) {
                    if ([double]::IsNaN($r[$b + $pair[0]])) { continue }
                    $q = & $ray $r[$b + $pair[0]] $r[$b + $pair[0] + 1]
                    $dot = [math]::Min(1.0, $mk[0] * $q[0] + $mk[1] * $q[1] + $mk[2] * $q[2])
                    $ang = [math]::Acos($dot) * 180 / [math]::PI
                    if ($pair[1] -eq 'w') { $sep[$e] = $ang } else { $sepV[$e] = $ang }
                }
            }
            $rows.Add([pscustomobject]@{ t = $pts[$i] + $offset; r = $r; sepW = $sep; sepV = $sepV })
        }
        $hudData[$c.name] = $rows
        continue
    }
    $scan = [JitterAnalysis]::Scan($ffmpeg, $file, $W, $H, $eyeW)
    $n = [math]::Min($scan.Length, $pts.Count)
    Write-Host ("  {0} frames {1}x{2}, motion t of frame 0 = {3:N3} s" -f $n, $W, $H, $offset)
    $rows = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $n; $i++) {
        $t = $pts[$i] + $offset
        $cx = $scan[$i][0]; $cy = $scan[$i][1]
        $ax = [double]::NaN; $ay = [double]::NaN
        if (-not [double]::IsNaN($cx)) {
            $xt = $k * ($tanL + ($cx + 0.5) / $eyeW * ($tanR - $tanL)); $yt = $k * ($tanU + ($cy + 0.5) / $H * ($tanD - $tanU))
            $ax = [math]::Atan($xt) * 180 / [math]::PI; $ay = [math]::Atan($yt) * 180 / [math]::PI
        }
        $rows.Add([pscustomobject]@{ t = $t; x = $ax; y = $ay; black = $scan[$i][2]; dx = $scan[$i][3] - $cx })
    }
    $data[$c.name] = $rows
}
if ($null -ne $prevTray) { $env:XR_EYE_CAPTURE_REQUIRE_TRAY = $prevTray } else { Remove-Item Env:\XR_EYE_CAPTURE_REQUIRE_TRAY -ErrorAction SilentlyContinue }
if ($null -ne $prevCfg) { $env:XR_EYE_CAPTURE_CONFIG = $prevCfg } else { Remove-Item Env:\XR_EYE_CAPTURE_CONFIG -ErrorAction SilentlyContinue }
if ($null -ne $prevIpc) { $env:XR_EYE_CAPTURE_TRAY_IPC = $prevIpc } else { Remove-Item Env:\XR_EYE_CAPTURE_TRAY_IPC -ErrorAction SilentlyContinue }

function JitterStd($rows) {
    $j = @($rows | Where-Object { $_.t -ge 1.0 -and $_.t -le 3.9 -and -not [double]::IsNaN($_.x) })
    $mx = ($j | Measure-Object x -Average).Average; $my = ($j | Measure-Object y -Average).Average
    $var = 0; foreach ($r in $j) { $var += ($r.x - $mx) * ($r.x - $mx) + ($r.y - $my) * ($r.y - $my) }
    return @([math]::Sqrt($var / [math]::Max(1, $j.Count)), $j.Count)
}
function Settle($rows) {
    $arr = $rows.ToArray()
    $fin = @($arr | Where-Object { $_.t -ge 6.5 -and -not [double]::IsNaN($_.x) })
    $fx = ($fin | Measure-Object x -Average).Average; $fy = ($fin | Measure-Object y -Average).Average
    $last = 5.0
    for ($i = 4; $i -lt $arr.Count - 4; $i++) {
        if ($arr[$i].t -lt 5.0) { continue }
        $sx = 0; $sy = 0; $cnt = 0
        for ($j = $i - 4; $j -le $i + 4; $j++) { if (-not [double]::IsNaN($arr[$j].x)) { $sx += $arr[$j].x; $sy += $arr[$j].y; $cnt++ } }
        if ($cnt -eq 0) { $last = $arr[$i].t; continue }
        $e = [math]::Sqrt([math]::Pow($sx / $cnt - $fx, 2) + [math]::Pow($sy / $cnt - $fy, 2))
        if ($e -gt 1.0) { $last = $arr[$i].t }
    }
    return @(($last - 5.0), $fx, $fy)
}

Write-Host "`n=== checks" -ForegroundColor Cyan
foreach ($name in @('raw', 'rawasym', 'small')) {
    if (-not $data.ContainsKey($name)) { continue }
    $lim = if ($name -eq 'small') { 0.3 } else { 0.15 }
    $se = 0; $cnt = 0; $missing = 0
    foreach ($r in $data[$name]) {
        if ([double]::IsNaN($r.x)) { $missing++; continue }
        $e = [JitterAnalysis]::Expected($r.t)
        $se += [math]::Pow($r.x - $e[0], 2) + [math]::Pow($r.y - $e[1], 2); $cnt++
    }
    $rms = [math]::Sqrt($se / [math]::Max(1, $cnt))
    Check ($rms -lt $lim -and $missing -eq 0) ("{3}: marker follows the mock head pose exactly (RMS error {0:N3} deg over {1} frames, marker missing in {2})" -f $rms, $cnt, $missing, $name)
}
foreach ($name in @('raw', 'rawasym', 'smooth', 'small')) {
    if (-not $data.ContainsKey($name)) { continue }
    $d = @($data[$name] | Where-Object { -not [double]::IsNaN($_.dx) } | ForEach-Object { $_.dx })
    $mean = ($d | Measure-Object -Average).Average
    $maxAbs = ($d | ForEach-Object { [math]::Abs($_) } | Measure-Object -Maximum).Maximum
    $msg = "{0}: far marker x right half - left half: mean {1:N2} px, max |dx| {2:N2} px ({3} frames)" -f $name, $mean, $maxAbs, $d.Count
    if ($name -eq 'rawasym') { Check ([math]::Abs($mean) -gt 50) "$msg -> parallax at infinity without symmetric_fov (the bug)" }
    else { Check ($maxAbs -le 1.0) "$msg -> same x in both eyes" }
}
if ($data.ContainsKey('raw') -and $data.ContainsKey('smooth')) {
    $s0 = JitterStd $data['raw']; $s1 = JitterStd $data['smooth']
    $ratio = $s0[0] / [math]::Max(1e-9, $s1[0])
    Check ($ratio -ge 5) ("jitter phase marker std-dev: raw {0:N3} deg, smooth {1:N3} deg -> {2:N1}x lower ({3} / {4} frames)" -f $s0[0], $s1[0], $ratio, $s0[1], $s1[1])
    if ($data.ContainsKey('noguard')) {
        $s2 = JitterStd $data['noguard']
        Write-Host ("  info noguard std-dev {0:N3} deg -> {1:N1}x lower" -f $s2[0], ($s0[0] / [math]::Max(1e-9, $s2[0])))
    }
}
foreach ($name in @('smooth', 'noguard')) {
    if (-not $data.ContainsKey($name)) { continue }
    $st = Settle $data[$name]
    Check ($st[0] -le 0.5) ("{0}: settles {1:N2} s after the turn ends (<= 0.5 s within 1 deg)" -f $name, $st[0])
    Check ([math]::Abs($st[1] - 30) -le 0.5 -and [math]::Abs($st[2]) -le 0.5) ("{0}: final rest position {1:N2} / {2:N2} deg (want 30 / 0)" -f $name, $st[1], $st[2])
}
foreach ($name in @('raw', 'rawasym', 'smooth', 'noguard', 'small')) {
    if (-not $data.ContainsKey($name)) { continue }
    $j = @($data[$name] | Where-Object { $_.t -ge 1.0 -and $_.t -le 3.9 })
    $bf = @($j | Where-Object { $_.black -gt 0 }).Count
    $maxB = ($j | Measure-Object black -Maximum).Maximum
    $turn = @($data[$name] | Where-Object { $_.t -gt 3.9 -and $_.black -gt 0 }).Count
    $all = $data[$name].ToArray()
    $allB = @($all | Where-Object { $_.black -gt 0 }).Count
    if ($name -eq 'noguard') { Write-Host ("  info noguard (edge_guard = 0): black border pixels in {0} of {1} jitter-phase frames (max {2} px), {3} of {4} frames over the whole recording" -f $bf, $j.Count, $maxB, $allB, $all.Count) }
    else {
        Check ($bf -eq 0) ("{0}: no black border pixels during the jitter phase ({1} frames)" -f $name, $j.Count)
        Check ($allB -eq 0) ("{0}: no black border pixels over the whole recording, both eyes ({1} frames, turn + catch-up included)" -f $name, $all.Count)
    }
}
if ($summ.ContainsKey('smooth')) {
    $sm = $summ['smooth']
    Check ($sm.uncovered -eq 0) ("smooth: layer summary reports 0 frames with uncovered output (got {0})" -f $sm.uncovered)
    Check ($sm.guarded -gt 0) ("smooth: the edge guard acted ({0} frames)" -f $sm.guarded)
} elseif (-not $OnlyHud) { Check $false 'smooth: re-projection summary line in the log' }
if (-not $OnlyHud) {
    if ($dims.ContainsKey('small') -and $logs.ContainsKey('small')) {
        $sw = $dims['small'][0]; $sh = $dims['small'][1]
        $mR = [regex]::Match($logs['small'], 're-projection rendered at (\d+)x(\d+) \(left (\d+) \| right (\d+); max_width 1280;[^\r\n]*?(\d)x\d bilinear taps')
        $want = if ($dims.ContainsKey('raw')) { [math]::Round(1280.0 * $dims['raw'][1] / $dims['raw'][0]) } else { -1 }
        Check ($sw -eq 1280 -and $sh % 2 -eq 0 -and ($want -lt 0 -or [math]::Abs($sh - $want) -le 2)) ("small: MP4 is {0}x{1} (want 1280 x ~{2}, even)" -f $sw, $sh, $want)
        Check ($mR.Success -and [int]$mR.Groups[1].Value -eq $sw -and [int]$mR.Groups[2].Value -eq $sh -and [int]$mR.Groups[5].Value -ge 2) ("small: layer log: {0}" -f $(if ($mR.Success) { $mR.Value } else { 're-projection rendered at ... (max_width 1280) line missing' }))
        Check ($summ.ContainsKey('small') -and $summ['small'].line -match ("re-projection {0}x{1}: GPU" -f $sw, $sh)) "small: the re-projection summary reports the ${sw}x${sh} render size"
    } else { Check $false 'small: recording' }
}
if ($summ.ContainsKey('noguard')) { Write-Host ("  info noguard: layer summary {0} frames uncovered, guard {1}" -f $summ['noguard'].uncovered, $summ['noguard'].guarded) }
function Median($v) { $s = @($v | Sort-Object); if ($s.Count -eq 0) { return [double]::NaN }; return $s[[int][math]::Floor($s.Count / 2)] }
function StdDev($v) {
    $a = @($v | Where-Object { -not [double]::IsNaN($_) }); if ($a.Count -lt 2) { return [double]::NaN }
    $m = ($a | Measure-Object -Average).Average; $q = 0; foreach ($x in $a) { $q += ($x - $m) * ($x - $m) }; return [math]::Sqrt($q / $a.Count)
}
foreach ($name in @('hudraw', 'hudsmooth')) {
    if (-not $hudData.ContainsKey($name)) { Check $false "${name}: recording"; continue }
    $rows = $hudData[$name].ToArray()
    foreach ($e in 0, 1) {
        $b = 6 * $e; $eye = @('left', 'right')[$e]
        # the first frames of a recording have no overlays: the worker creates the overlay atlas at the first frame
        # that has a layer of its format ("before their atlas existed" in the overlay summary)
        $miss = 0; $early = 0
        for ($i = 0; $i -lt $rows.Count; $i++) { if ([double]::IsNaN($rows[$i].r[$b])) { if ($i -lt 5) { $early++ } else { $miss++ } } }
        $xs = @($rows | Where-Object { -not [double]::IsNaN($_.r[$b]) } | ForEach-Object { $_.r[$b] })
        $ys = @($rows | Where-Object { -not [double]::IsNaN($_.r[$b]) } | ForEach-Object { $_.r[$b + 1] })
        $mx = Median $xs; $my = Median $ys
        $dev = 0.0; foreach ($i in 0..($xs.Count - 1)) { $dev = [math]::Max($dev, [math]::Max([math]::Abs($xs[$i] - $mx), [math]::Abs($ys[$i] - $my))) }
        Check ($miss -eq 0 -and $dev -le 1.0) ("{0}: VIEW-space quad, {1} eye: at ({2:N2}, {3:N2}) px, max deviation {4:N3} px over {5} frames (<= 1 px; missing in {6} + {7} first frames before the atlas existed) -> fixed in the video" -f $name, $eye, $mx, $my, $dev, $xs.Count, $miss, $early)
        $j = @($rows | Where-Object { $_.t -ge 1.0 -and $_.t -le 3.9 })
        $sw = StdDev @($j | ForEach-Object { $_.sepW[$e] })
        $sv = StdDev @($j | ForEach-Object { $_.sepV[$e] })
        $wx = StdDev @($j | ForEach-Object { $_.r[$b + 2] }); $wy = StdDev @($j | ForEach-Object { $_.r[$b + 3] })
        $wMiss = @($j | Where-Object { [double]::IsNaN($_.sepW[$e]) }).Count
        Check ($wMiss -eq 0 -and $sw -le 0.05) ("{0}: world-locked quad, {1} eye: angle to the world marker std-dev {2:N4} deg (<= 0.05; jitter phase, {3} frames, missing {4}); it moves with the world: position std-dev {5:N2} / {6:N2} px; VIEW quad vs marker {7:N3} deg" -f $name, $eye, $sw, $j.Count, $wMiss, $wx, $wy, $sv)
    }
}
if ($fails.Count -eq 0) { Write-Host "`nPASS head smoothing" -ForegroundColor Green; exit 0 }
Write-Host "`nFAIL head smoothing ($($fails.Count) checks)" -ForegroundColor Red
exit 1
