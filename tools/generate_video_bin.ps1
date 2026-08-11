param(
    [string]$InputFile = "untitled.mp4",
    [string]$OutputFile = "assets\\video\\video.bin",
    [int]$Fps = 30,
    [int]$Quality = 24,
    [string]$FfmpegDir = "tools\\ffmpeg\\bin",
    [string]$MaxSize = "0x1900000"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

function Resolve-ArgPath {
    param([string]$Path, [string]$Root)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $Root $Path
}

$InputPath = Resolve-ArgPath $InputFile $ProjectRoot
$OutputPath = Resolve-ArgPath $OutputFile $ProjectRoot
$FfmpegPath = Resolve-ArgPath $FfmpegDir $ProjectRoot
$PackScript = Join-Path $ProjectRoot "tools\pack_video.py"

if (!(Test-Path $InputPath)) {
    throw "Input video not found: $InputPath"
}

if (!(Test-Path $PackScript)) {
    throw "pack_video.py not found: $PackScript"
}

if (!(Test-Path $FfmpegPath)) {
    throw "ffmpeg dir not found: $FfmpegPath"
}

Push-Location $ProjectRoot
try {
    $args = @(
        $PackScript,
        "--input", $InputPath,
        "--output", $OutputPath,
        "--ffmpeg-dir", $FfmpegPath,
        "--fps", $Fps.ToString(),
        "--quality", $Quality.ToString(),
        "--max-size", $MaxSize
    )
    & python @args
} finally {
    Pop-Location
}
