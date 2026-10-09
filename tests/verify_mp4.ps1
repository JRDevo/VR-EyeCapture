<#
    verify_mp4.ps1 -- check a recording made from the test pattern (tests/test_pattern.h) with ffprobe / ffmpeg.

    Checks: video codec, SBS size, frame rate, duration, the st3d box (ffprobe "Stereo 3D / side by side" side data,
    plus a raw byte scan for 'st3d'), the AAC stream and that it is not silent (optional), and decoded pixel values
    of the grey steps / eye-id band in both halves (gamma not crushed or doubled, eyes not swapped).

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
    [string]$FfmpegDir = ''
)
$ErrorActionPreference = 'Stop'
$fails = New-Object System.Collections.Generic.List[string]
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
    Check (($v.width -eq $wantW) -and ($v.height -eq $wantH)) "size $($v.width)x$($v.height) (want ${wantW}x${wantH})"
    $p = $v.avg_frame_rate.Split('/'); $rate = [double]$p[0] / [double]$p[1]
    Check ([math]::Abs($rate - $Fps) -le 2) ("avg frame rate {0:N2} (want ~{1})" -f $rate, $Fps)
    $dur = [double]$v.duration
    Check ([math]::Abs($dur - $Seconds) -le $DurationTolerance) ("video duration {0:N2} s (want ~{1})" -f $dur, $Seconds)
    $sd = $v.side_data_list | Where-Object { $_.side_data_type -eq 'Stereo 3D' }
    Check ($sd -ne $null -and $sd.type -eq 'side by side') "st3d parsed by ffprobe: $($sd.side_data_type) / $($sd.type)"
    Check ($v.color_space -eq 'bt709' -and $v.color_range -eq 'tv') "colour tags $($v.color_space) / $($v.color_range)"
}
# raw scan of the moov for 'st3d' inside the sample entry
$bytes = [System.IO.File]::ReadAllBytes($File)
$txt = [System.Text.Encoding]::ASCII.GetString($bytes)
$i = $txt.LastIndexOf('st3d')
$j = [math]::Max($txt.LastIndexOf('hvc1'), $txt.LastIndexOf('avc1'))
Check ($i -gt 0 -and $j -gt 0 -and $i -gt $j -and $bytes[$i + 8] -eq 2) "st3d box bytes after the sample entry, stereo_mode=$(if ($i -gt 0) { $bytes[$i + 8] } else { '?' })"

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
    $grey = @(0, 32, 64, 96, 128, 160, 192, 224, 255)
    $worst = 0
    for ($eye = 0; $eye -lt 2; $eye++) {
        $ox = $eye * $EyeW
        for ($k = 0; $k -lt 9; $k++) {
            $g = $grey[$k]
            if ($Format -eq 'unorm') {   # linear data -> encoder applies the sRGB curve
                $l = $g / 255.0
                $s = if ($l -le 0.0031308) { 12.92 * $l } else { 1.055 * [math]::Pow($l, 1 / 2.4) - 0.055 }
                $g = [math]::Round(255 * $s)
            }
            $c = Px ([int]($ox + ($k + 0.5) * $EyeW / 9)) ([int]($EyeH / 8))
            $d = [math]::Abs($c[0] - $g); if ($d -gt $worst) { $worst = $d }
        }
        $id = Px ([int]($ox + $EyeW / 2)) ([int](9 * $EyeH / 16))
        if ($eye -eq 0) { Check ($id[0] -gt $id[2] + 60) "left half is the LEFT eye (id band $($id -join ','))" }
        else { Check ($id[2] -gt $id[0] + 60) "right half is the RIGHT eye (id band $($id -join ','))" }
    }
    Check ($worst -le 6) "grey steps within 6 code values of the source in both eyes (worst $worst)"
}

if ($fails.Count -eq 0) { Write-Host "PASS $File" -ForegroundColor Green; exit 0 }
Write-Host "FAIL $File ($($fails.Count) checks)" -ForegroundColor Red
exit 1
