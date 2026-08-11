param(
    [string]$Port = "",
    [string]$VideoBin = "assets\\video\\video.bin",
    [string]$IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.2"
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

$VideoPath = Resolve-ArgPath $VideoBin $ProjectRoot
if (!(Test-Path $VideoPath)) {
    throw "Video image not found: $VideoPath. Run tools\generate_video_bin.ps1 first."
}

$esptool = Join-Path $IdfPath "components\esptool_py\esptool\esptool.py"
if (!(Test-Path $esptool)) {
    throw "esptool.py not found: $esptool"
}

$cmd = @("python", $esptool)
if ($Port -ne "") {
    $cmd += @("--port", $Port)
}
$cmd += @("--chip", "esp32p4", "write_flash", "0x210000", $VideoPath)

Push-Location $ProjectRoot
try {
    & $cmd[0] $cmd[1..($cmd.Count - 1)]
} finally {
    Pop-Location
}
