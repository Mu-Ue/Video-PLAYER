# Generates the bench clips into tools\bench\clips with ffmpeg.
#
# Each frame carries its own number as 16 bits across the frame: a row of black or white blocks, then the same
# blocks inverted below, so a torn or blended capture is caught. Every 2 seconds a white box flashes in the
# middle for exactly one frame, with a 10 ms click on the audio at the same time, for the A/V offset. The rest is
# gray, so white subtitle text stands out. Cue edges fall at varied phases within frames.

param([int]$Seconds = 60, [string]$Only)  # Only: make just the clip of this name
$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot 'clips'
New-Item -ItemType Directory -Force $out | Out-Null

# Frame rates as ffmpeg rationals, with their frame period in seconds.
$rates = @{ '23.976' = '24000/1001'; '24' = '24'; '30' = '30'; '60' = '60' }

function Write-Srt([string]$path, [double]$fps) {
    # Cues 0.35 s long, every 0.7 s, starting at a phase within a frame that cycles through these fractions,
    # so starts and ends land just after, just before and between frame times.
    $phases = 0, 0.001, 0.25, 0.5, 0.75, 0.999
    $lines = New-Object System.Collections.Generic.List[string]
    $period = 1 / $fps
    $i = 0
    for ($t = 1.0; $t + 0.4 -lt $Seconds; $t += 0.7) {
        $start = [math]::Floor($t * $fps) / $fps + $phases[$i % $phases.Count] * $period
        $end = $start + 0.35 + $phases[($i + 3) % $phases.Count] * $period
        $fmt = { param($s) $ms = [int64][math]::Round($s * 1000); '{0:00}:{1:00}:{2:00},{3:000}' -f [math]::Floor($ms / 3600000), ([math]::Floor($ms / 60000) % 60), ([math]::Floor($ms / 1000) % 60), ($ms % 1000) }
        $lines.Add("$($i + 1)"); $lines.Add("$(& $fmt $start) --> $(& $fmt $end)"); $lines.Add('IIIIIIIIIIII'); $lines.Add('')
        $i++
    }
    [IO.File]::WriteAllLines($path, $lines)
}

function New-Clip([string]$name, [string]$rate, [int]$w, [int]$h, [string]$codec, [string[]]$extra) {
    if ($Only -and $name -ne $Only) { return }
    $fps = $rates[$rate]
    $f = [double](Invoke-Expression ($fps -replace '/', ' / '))
    # The pattern at a small size, scaled up without smoothing; FPS*2 frames apart is 2 s.
    $flashEvery = [math]::Round(2 * $f)
    # The bits sit from 15% to 25% of the height, clear of the player's top-left OSD and the subtitles.
    $geq = "lum='if(between(Y,H*0.15,H*0.2-1),if(mod(floor(N/pow(2,floor(X*16/W))),2),255,0),if(between(Y,H*0.2,H*0.25-1),if(mod(floor(N/pow(2,floor(X*16/W))),2),0,255),if(eq(mod(N,$flashEvery),0)*between(Y,H*0.35,H*0.65)*between(X,W*0.35,W*0.65),255,96)))':cb=128:cr=128"
    $video = "color=c=gray:s=256x144:r=${fps}:d=$Seconds,format=yuv444p,geq=$geq,scale=${w}:${h}:flags=neighbor,format=yuv420p"
    # Variable rate: every 2 s it keeps every 3rd, every, every 2nd, then every 5th frame, so the rate both
    # rises and falls. Each segment starts on a flash frame, which is kept.
    if ($extra -contains 'vfr') {
        $video += ",select='if(eq(mod(floor(n/$flashEvery),4),0),not(mod(n,3)),if(eq(mod(floor(n/$flashEvery),4),1),1,if(eq(mod(floor(n/$flashEvery),4),2),not(mod(n,2)),not(mod(n,5)))))'"
    }
    # The click starts with the flash frame: frame k*flashEvery is at k*flashEvery/fps seconds.
    $audio = "aevalsrc='if(lt(t-floor(t*$f/$flashEvery)*$flashEvery/$f,0.01),0.8*sin(2*PI*1000*t),0)':s=48000:d=$Seconds"
    $srt = Join-Path $out "$name.srt"
    Write-Srt $srt $f
    $codecArgs = if ($codec -eq 'hevc') { '-c:v', 'libx265', '-x265-params', 'log-level=error', '-tag:v', 'hvc1' } else { '-c:v', 'libx264' }
    # No B-frames: an MP4 then needs no edit list to start its video at 0.
    if ($extra -contains 'nob') { $codecArgs += '-bf', '0' }
    $ext = if ($extra -contains 'mkv') { 'mkv' } else { 'mp4' }
    $a = @('-y', '-v', 'error', '-f', 'lavfi', '-i', $video, '-f', 'lavfi', '-i', $audio)
    if ($ext -eq 'mkv') { $a += '-i', $srt }
    # PCM has no encoder delay, so it shows the offset of the player alone.
    $audioArgs = if ($extra -contains 'pcm') { '-c:a', 'pcm_s16le' } else { '-c:a', 'aac', '-b:a', '128k' }
    $a += $codecArgs + @('-preset', 'veryfast', '-g', [string]$flashEvery) + $audioArgs
    if ($ext -eq 'mkv') { $a += '-c:s', 'srt' }
    if ($extra -contains 'live') { $a += '-live', '1' }  # no index
    if ($extra -contains 'vfr') { $a += '-fps_mode', 'vfr' }
    $file = Join-Path $out "$name.$ext"
    Write-Host "Making $file"
    & ffmpeg @a $file
    if ($LASTEXITCODE) { throw "ffmpeg failed for $name" }
    # Each frame's pts as the container stores it (MKV rounds to milliseconds), in frame order: the truth the
    # bench checks subtitles against.
    $pts = & ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 $file |
        ForEach-Object { [double]$_ } | Sort-Object
    [IO.File]::WriteAllLines((Join-Path $out "$name.pts"), [string[]]($pts | ForEach-Object { $_.ToString('R', [Globalization.CultureInfo]::InvariantCulture) }))
}

New-Clip 'r23976_1080_h264' '23.976' 1920 1080 'h264' @()
New-Clip 'r23976_1080_h264_nob' '23.976' 1920 1080 'h264' @('nob')
New-Clip 'r24_1080_h264' '24' 1920 1080 'h264' @()
New-Clip 'r30_1080_h264' '30' 1920 1080 'h264' @()
New-Clip 'r60_1080_h264' '60' 1920 1080 'h264' @()
New-Clip 'r23976_1080_h264_subs' '23.976' 1920 1080 'h264' @('mkv')
New-Clip 'r23976_1080_h264_pcm' '23.976' 1920 1080 'h264' @('mkv', 'pcm')
New-Clip 'r23976_2160_hevc_subs' '23.976' 3840 2160 'hevc' @('mkv')
New-Clip 'r23976_1080_h264_noindex' '23.976' 1920 1080 'h264' @('mkv', 'live')
New-Clip 'v60_1080_h264' '60' 1920 1080 'h264' @('vfr')
