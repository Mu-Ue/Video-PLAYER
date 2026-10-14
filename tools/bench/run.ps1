# Runs the bench suite against a player build and appends the rows to results.csv, labeled. Make the clips first
# with make_clips.ps1 and the bench with build.bat. Keep the mouse still while it runs: moving it over the player
# shows the controls, which the bench counts.

param(
    [Parameter(Mandatory)][string]$Label,
    [string]$Player,
    [string]$Csv,
    [int]$Repeat = 1
)
$ErrorActionPreference = 'Stop'
# Defaults here: Windows PowerShell has no $PSScriptRoot yet while binding parameters.
if (-not $Player) { $Player = Join-Path $PSScriptRoot '..\..\build\VideoPLAYER.exe' }
if (-not $Csv) { $Csv = Join-Path $PSScriptRoot 'results.csv' }
$bench = Join-Path $PSScriptRoot 'out\bench.exe'
$clips = Join-Path $PSScriptRoot 'clips'

# Clip, seconds, seeks, extra arguments.
$suite = @(
    @('r23976_1080_h264.mp4', 30, 0, @('--subs')),         # SRT exactness, cadence, A/V
    @('r23976_1080_h264_nob.mp4', 30, 0, @('--subs')),     # the same without B-frames or an edit list
    @('r24_1080_h264.mp4', 20, 0, @()),
    @('r30_1080_h264.mp4', 20, 0, @()),
    @('r60_1080_h264.mp4', 20, 0, @()),
    @('r23976_1080_h264_subs.mkv', 30, 6, @('--subs')),    # MKV subtitles across seeks
    @('r23976_1080_h264_pcm.mkv', 30, 0, @()),             # the same with PCM audio: A/V without encoder delay
    @('v60_1080_h264.mp4', 20, 0, @()),                    # variable frame rate
    @('r23976_2160_hevc_subs.mkv', 30, 6, @('--subs')),    # 4K HEVC
    @('r23976_1080_h264_noindex.mkv', 20, 3, @('--subs')), # no index
    @('r23976_2160_hevc_subs.mkv', 30, 6, @('--subs', '--cores', '2'))  # 2 cores
)

for ($r = 0; $r -lt $Repeat; $r++) {
    foreach ($t in $suite) {
        $a = @($Player, (Join-Path $clips $t[0]), '--seconds', $t[1], '--seeks', $t[2], '--label', $Label, '--csv', $Csv) + $t[3]
        & $bench @a | Select-Object -Last 1
        Start-Sleep -Seconds 1
    }
}
