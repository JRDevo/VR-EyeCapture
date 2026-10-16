<#
    verify_mp4.ps1 -- check a recording made from the test pattern (tests/test_pattern.h) with ffprobe / ffmpeg.

    Checks: video codec, SBS size, frame rate, duration, the st3d box (ffprobe "Stereo 3D / side by side" side data,
    plus a raw byte scan for 'st3d'), the AAC stream and that it is not silent (optional), and decoded pixel values
    of the grey steps / eye-id band in both halves (gamma not crushed or doubled, eyes not swapped).
    -Packing tb_half|sbs_half|sbs_full|tb_full (record_for_youtube recordings; pass the frame as -OutW / -OutH): the st3d
    checks expect top-bottom (stereo_mode 1, ffprobe "top and bottom") for tb_*, and the pixel checks sample each eye in
    its packed rectangle (tb: left eye in the top half, right eye in the bottom half; sbs: left | right), with the eye's
    -EyeCrop (16:9 default / 4:3 / 4:3pad / none, the recorder's centre crop of the -EyeW x -EyeH source eye) and the squeeze.
    4:3pad: the picture is narrower than the packed eye (black bars left and right, yt_layout.h pw / padX): the samples are
    taken inside it, and the middle of each bar must be black.

    Windows PowerShell 5.1 compatible.
      powershell -ExecutionPolicy Bypass -File tests\verify_mp4.ps1 -File x.mp4 -EyeW 1920 -EyeH 1080 -Fps 90 -Seconds 5
#>
param(
    [Parameter(Mandatory = $true)][string]$File,
    [int]$EyeW = 1920,
    [int]$EyeH = 1080,
    [double]$Fps = 90,
    [double]$Seconds = 5,
    [double]$DurationTolerance = 0.35,
    [string]$Codec = 'hevc',
    [ValidateSet('srgb', 'unorm')][string]$Format = 'srgb',
    [switch]$ExpectAudio,
    [switch]$ExpectSound,          # audio must not be silent (a tone was played)
    [switch]$SkipPixels,           # for scaled outputs
    [int]$OutW = 0,                # expected encoded size when the encoder scaled the SBS frame (0 = 2*EyeW x EyeH)
    [int]$OutH = 0,
    [ValidateSet('tb_half', 'sbs_half', 'sbs_full', 'tb_full')][string]$Packing,   # record_for_youtube layout ('' = plain SBS)
    [ValidateSet('16:9', '4:3', '4:3pad', 'none')][string]$EyeCrop = '16:9',
    [string]$FfmpegDir = ''
)
$ErrorActionPreference = 'Stop'
$fails = New-Object System.Collections.Generic.List[string]
$tb = [bool]$Packing -and $Packing.StartsWith('tb_')
$wantStereo = if ($tb) { 'top and bottom' } else { 'side by side' }
$wantMode = if ($tb) { 1 } else { 2 }
function Check($ok, $what) {
    if ($ok) { Write-Host "  ok   $what" -ForegroundColor Green } else { Write-Host "  FAIL $what" -ForegroundColor Red; $fails.Add($what) }
}

if (-not $FfmpegDir) {
    $cand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffprobe.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cand) { $FfmpegDir = $cand.DirectoryName } else {
        $onPath = Get-Command ffprobe -ErrorAction SilentlyContinue
        if ($onPath) { $FfmpegDir = Split-Path $onPath.Source } else { throw 'ffprobe not found (pass -FfmpegDir)' }
    }
}
$ffprobe = Join-Path $FfmpegDir 'ffprobe.exe'
$ffmpeg = Join-Path $FfmpegDir 'ffmpeg.exe'
if (-not (Test-Path $File)) { throw "no such file: $File" }

Write-Host "verify $File"
$json = & $ffprobe -v error -print_format json -show_streams -show_format $File | Out-String | ConvertFrom-Json
$v = $json.streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
$a = $json.streams | Where-Object { $_.codec_type -eq 'audio' } | Select-Object -First 1

Check ($v -ne $null) 'video stream present'
if ($v) {
    Check ($v.codec_name -eq $Codec) "codec $($v.codec_name) (want $Codec)"
    $wantW = 2 * $EyeW - ((2 * $EyeW) % 2); $wantH = $EyeH - ($EyeH % 2)
    if ($OutW -gt 0) { $wantW = $OutW; $wantH = $OutH }
    Check (($v.width -eq $wantW) -and ($v.height -eq $wantH)) "size $($v.width)x$($v.height) (want ${wantW}x${wantH})"
    $p = $v.avg_frame_rate.Split('/'); $rate = [double]$p[0] / [double]$p[1]
    Check ([math]::Abs($rate - $Fps) -le 2) ("avg frame rate {0:N2} (want ~{1})" -f $rate, $Fps)
    $dur = [double]$v.duration
    Check ([math]::Abs($dur - $Seconds) -le $DurationTolerance) ("video duration {0:N2} s (want ~{1})" -f $dur, $Seconds)
    $sd = $v.side_data_list | Where-Object { $_.side_data_type -eq 'Stereo 3D' }
    Check ($sd -ne $null -and $sd.type -eq $wantStereo) "st3d parsed by ffprobe: $($sd.side_data_type) / $($sd.type) (want $wantStereo)"
    Check ($v.color_space -eq 'bt709' -and $v.color_range -eq 'tv') "colour tags $($v.color_space) / $($v.color_range)"
}
# raw scan of the moov for 'st3d' inside the sample entry
$bytes = [System.IO.File]::ReadAllBytes($File)
$txt = [System.Text.Encoding]::ASCII.GetString($bytes)
$i = $txt.LastIndexOf('st3d')
$j = [math]::Max([math]::Max($txt.LastIndexOf('hvc1'), $txt.LastIndexOf('avc1')), $txt.LastIndexOf('av01'))
Check ($i -gt 0 -and $j -gt 0 -and $i -gt $j -and $bytes[$i + 8] -eq $wantMode) "st3d box bytes after the sample entry, stereo_mode=$(if ($i -gt 0) { $bytes[$i + 8] } else { '?' }) (want $wantMode)"

if ($ExpectAudio) {
    Check ($a -ne $null -and $a.codec_name -eq 'aac') "audio stream $($a.codec_name) $($a.sample_rate) Hz $($a.channels) ch"
    if ($a) {
        Check ([math]::Abs([double]$a.duration - $Seconds) -le $DurationTolerance + 0.15) ("audio duration {0:N2} s" -f [double]$a.duration)
        if ($ExpectSound) {
            # ffmpeg prints to stderr; PS 5.1 turns native stderr into errors under 'Stop', so go through cmd.
            $vd = cmd /c "`"$ffmpeg`" -hide_banner -i `"$File`" -map 0:a -af volumedetect -f null - 2>&1" | Out-String
            $m = [regex]::Match($vd, 'mean_volume:\s*(-?[0-9.]+)')
            $mean = if ($m.Success) { [double]$m.Groups[1].Value } else { -999 }
            Check ($mean -gt -50) "audio not silent (mean volume $mean dB)"
        }
    }
}

if (-not $SkipPixels -and $v) {
    $raw = Join-Path ([System.IO.Path]::GetTempPath()) ("eyecap_verify_{0}.rgb" -f $PID)
    & $ffmpeg -v error -y -ss ($Seconds / 2) -i $File -frames:v 1 -vf 'scale=in_color_matrix=bt709:in_range=limited:out_range=full,format=rgb24' -f rawvideo $raw
    $px = [System.IO.File]::ReadAllBytes($raw)
    Remove-Item $raw -ErrorAction SilentlyContinue
    $W = [int]$v.width
    function Px($x, $y) { $o = ($y * $W + $x) * 3; return @($px[$o], $px[$o + 1], $px[$o + 2]) }
    # Source eye (pattern coordinates, EyeW x EyeH) -> frame pixel. Plain SBS: identity in each half. -Packing: the eye is
    # centre-cropped to ch rows (from row $top) and scaled into its sw x sh rectangle at (eye x0, eye y0); 4:3pad: into the
    # picture vw wide at x0 + vx inside it (the eye padded to pw in source units, the bars padX wide on the left).
    $top = 0.0; $ch = [double]$EyeH; $sw = [double]$EyeW; $sh = [double]$EyeH; $vx = 0.0; $vw = [double]$EyeW
    if ($Packing) {
        $n = if ($EyeCrop -eq '16:9') { 16 } elseif ($EyeCrop -eq '4:3' -or $EyeCrop -eq '4:3pad') { 4 } else { 0 }
        $d = if ($EyeCrop -eq '16:9') { 9 } elseif ($EyeCrop -eq '4:3' -or $EyeCrop -eq '4:3pad') { 3 } else { 1 }
        if ($n -gt 0) { $want = 2 * [math]::Floor($EyeW * $d / $n / 2.0); if ($want -lt $EyeH) { $ch = $want } }
        $top = ($EyeH - $ch) / 2.0
        if ($tb) { $sw = [double]$v.width; $sh = [double]$v.height / 2 } else { $sw = [double]$v.width / 2; $sh = [double]$v.height }
        $pw = [double]$EyeW
        if ($EyeCrop -eq '4:3pad') { $pw = [math]::Max($EyeW, 2 * [math]::Floor($ch * 16 / 9 / 2.0 + 0.5)) }
        $padX = 2 * [math]::Floor(($pw - $EyeW) / 4)
        $vw = $sw * $EyeW / $pw; $vx = $sw * $padX / $pw
        $padText = if ($pw -gt $EyeW) { " (padded to {0}: picture {1:0.#} wide at +{2:0.#})" -f $pw, $vw, $vx } else { '' }
        Write-Host ("  packing $Packing, eye_crop ${EyeCrop}: source eye ${EyeW}x${EyeH} rows {0}..{1} -> {2}x{3} per eye{4}" -f $top, ($top + $ch), $sw, $sh, $padText)
    }
    function MapX($eye, $sx) { if ($Packing) { $x0 = if ($tb) { 0 } else { $eye * $sw }; return [int]($x0 + $vx + $sx * $vw / $EyeW) } else { return [int]($eye * $EyeW + $sx) } }
    function MapY($eye, $sy) { if ($Packing) { $y0 = if ($tb) { $eye * $sh } else { 0 }; return [int]($y0 + ($sy - $top) * $sh / $ch) } else { return [int]$sy } }
    # grey steps: the middle of the part of band A [0, EyeH/4) that survived the crop (EyeH/8 without a crop)
    $aLo = [math]::Max(0.0, $top); $aHi = [math]::Min($EyeH / 4.0, $top + $ch)
    $greyY = ($aLo + $aHi) / 2
    $grey = @(0, 32, 64, 96, 128, 160, 192, 224, 255)
    $worst = 0
    for ($eye = 0; $eye -lt 2; $eye++) {
        if ($aHi - $aLo -lt 2) { break }   # the crop removed the grey band entirely (not the case for the test cases)
        for ($k = 0; $k -lt 9; $k++) {
            $g = $grey[$k]
            if ($Format -eq 'unorm') {   # linear data -> encoder applies the sRGB curve
                $l = $g / 255.0
                $s = if ($l -le 0.0031308) { 12.92 * $l } else { 1.055 * [math]::Pow($l, 1 / 2.4) - 0.055 }
                $g = [math]::Round(255 * $s)
            }
            $c = Px (MapX $eye (($k + 0.5) * $EyeW / 9)) (MapY $eye $greyY)
            $dv = [math]::Abs($c[0] - $g); if ($dv -gt $worst) { $worst = $dv }
        }
    }
    for ($eye = 0; $eye -lt 2; $eye++) {
        $id = Px (MapX $eye ($EyeW / 2)) (MapY $eye (9 * $EyeH / 16))
        $where = if ($tb) { @('top', 'bottom')[$eye] } else { @('left', 'right')[$eye] }
        if ($eye -eq 0) { Check ($id[0] -gt $id[2] + 60) "$where half is the LEFT eye (id band $($id -join ','))" }
        else { Check ($id[2] -gt $id[0] + 60) "$where half is the RIGHT eye (id band $($id -join ','))" }
    }
    if ($Packing -and $vx -ge 8) {   # 4:3pad: the middle of both bars of both eyes, at the eye's middle row, is black
        $barMax = 0
        for ($eye = 0; $eye -lt 2; $eye++) {
            $x0 = if ($tb) { 0 } else { $eye * $sw }
            $y = [int]((MapY $eye ($EyeH / 2)))
            foreach ($bx in @(($x0 + $vx / 2), ($x0 + $sw - $vx / 2))) { $c = Px ([int]$bx) $y; foreach ($ch3 in $c) { if ($ch3 -gt $barMax) { $barMax = $ch3 } } }
        }
        Check ($barMax -le 12) "4:3pad: black bars left and right of both eyes (max channel $barMax)"
    }
    if ($aHi - $aLo -ge 2) { Check ($worst -le 6) "grey steps within 6 code values of the source in both eyes (worst $worst)" }
    else { Write-Host '  note grey band cropped away: grey-step check skipped' }
}

if ($fails.Count -eq 0) { Write-Host "PASS $File" -ForegroundColor Green; exit 0 }
Write-Host "FAIL $File ($($fails.Count) checks)" -ForegroundColor Red
exit 1
