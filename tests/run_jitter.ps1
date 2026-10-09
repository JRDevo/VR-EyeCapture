<#
    run_jitter.ps1 -- end-to-end test of head-motion smoothing and symmetric_fov, no headset: real OpenXR loader -> INSTALLED layer ->
    tests/mock_runtime in jitter mode (head motion of tests/test_motion.h: +-2 deg yaw/pitch/roll jitter at 3.3/4.1/4.7 Hz
    for the whole run, plus a deliberate 30 deg yaw turn from t = 4 s to t = 5 s; asymmetric per-eye fov like a real
    headset: left -50/+40, right -40/+50, +-45 deg). layer_e2e --jitter renders a world-locked grid with a white marker
    (a direction, i.e. at infinity) for the located pose every frame, like a game.

    Four recordings:   raw      head_smoothing = 0  (symmetric_fov = 1, the default: output +-40 x +-45 deg per eye)
                       rawasym  head_smoothing = 0, symmetric_fov = 0      (stage bypassed: the raw eye images)
                       smooth   head_smoothing = 0.6, output_fov_scale = 0.9    (edge_guard = 1, the default)
                       noguard  same with edge_guard = 0                         (pure filter + follow)
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
    The recording cases set XR_EYE_CAPTURE_REQUIRE_TRAY=0 and XR_EYE_CAPTURE_CONFIG (test-only overrides).
      powershell -ExecutionPolicy Bypass -File tests\run_jitter.ps1
#>
param([string]$FfmpegDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$exe = Join-Path $bin 'layer_e2e.exe'
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
    @{ name = 'raw';     cfg = 'head_smoothing=0';                                     k = 1.0; tans = $symTans },
    @{ name = 'rawasym'; cfg = 'head_smoothing=0;symmetric_fov=0';                     k = 1.0; tans = $rawTans },
    @{ name = 'smooth';  cfg = 'head_smoothing=0.6;output_fov_scale=0.9';              k = 0.9; tans = $symTans },
    @{ name = 'noguard'; cfg = 'head_smoothing=0.6;output_fov_scale=0.9;edge_guard=0'; k = 0.9; tans = $symTans }
)
$prevTray = $env:XR_EYE_CAPTURE_REQUIRE_TRAY; $prevCfg = $env:XR_EYE_CAPTURE_CONFIG
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
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
    $o = & $exe --jitter --seconds 9 --record 8.5 --out-dir $out 2>&1 | Out-String
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
    $pts = & $ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 $file | Where-Object { $_ -match '\d' } |
        ForEach-Object { [double]($_.Trim().TrimEnd(',')) } | Sort-Object
    $scan = [JitterAnalysis]::Scan($ffmpeg, $file, $W, $H, $eyeW)
    $n = [math]::Min($scan.Length, $pts.Count)
    Write-Host ("  {0} frames {1}x{2}, motion t of frame 0 = {3:N3} s" -f $n, $W, $H, $offset)
    $k = $c.k
    $tanL = $c.tans[0]; $tanR = $c.tans[1]; $tanU = $c.tans[2]; $tanD = $c.tans[3]
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
foreach ($name in @('raw', 'rawasym')) {
    if (-not $data.ContainsKey($name)) { continue }
    $se = 0; $cnt = 0; $missing = 0
    foreach ($r in $data[$name]) {
        if ([double]::IsNaN($r.x)) { $missing++; continue }
        $e = [JitterAnalysis]::Expected($r.t)
        $se += [math]::Pow($r.x - $e[0], 2) + [math]::Pow($r.y - $e[1], 2); $cnt++
    }
    $rms = [math]::Sqrt($se / [math]::Max(1, $cnt))
    Check ($rms -lt 0.15 -and $missing -eq 0) ("{3}: marker follows the mock head pose exactly (RMS error {0:N3} deg over {1} frames, marker missing in {2})" -f $rms, $cnt, $missing, $name)
}
foreach ($name in @('raw', 'rawasym', 'smooth')) {
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
foreach ($name in @('raw', 'rawasym', 'smooth', 'noguard')) {
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
} else { Check $false 'smooth: re-projection summary line in the log' }
if ($summ.ContainsKey('noguard')) { Write-Host ("  info noguard: layer summary {0} frames uncovered, guard {1}" -f $summ['noguard'].uncovered, $summ['noguard'].guarded) }
if ($fails.Count -eq 0) { Write-Host "`nPASS head smoothing" -ForegroundColor Green; exit 0 }
Write-Host "`nFAIL head smoothing ($($fails.Count) checks)" -ForegroundColor Red
exit 1
