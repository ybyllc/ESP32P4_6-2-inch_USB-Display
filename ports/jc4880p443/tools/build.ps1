# 使用已激活的ESP-IDF6环境，独立缓存、j12，只生成应用，不刷机。
param([ValidateRange(1,64)][int]$Jobs=12)
$ErrorActionPreference='Stop'
$port=[IO.Path]::GetFullPath("$PSScriptRoot/..")
$repo=[IO.Path]::GetFullPath("$port/../..")
& "$PSScriptRoot/check_sdk.ps1"
if($LASTEXITCODE){throw 'SDK校验失败'}
& python "$PSScriptRoot/check_sources.py"
if($LASTEXITCODE){throw '固定来源检查失败'}
$dirty=@(git -C $repo status --porcelain -- ports/jc4880p443 main/tusb/usb_descriptors.h)
if($dirty.Count){throw '先提交副屏输入再生成候选，不能用工作树冒充记录的提交'}
$source=(git -C $repo rev-parse HEAD).Trim()
$out="$port/output/release"
[IO.Directory]::CreateDirectory($out)|Out-Null
Push-Location $port
try{
    & idf.py -B build reconfigure *> "$out/build-private.log"
    if($LASTEXITCODE){throw '配置失败，见output/release/build-private.log'}
    & cmake --build build --target app --parallel $Jobs *>> "$out/build-private.log"
    if($LASTEXITCODE){throw '应用构建失败，见output/release/build-private.log'}
}finally{Pop-Location}
& python "$PSScriptRoot/check_usb_display_artifact.py" "$port/build" *> "$out/artifact-check.json"
if($LASTEXITCODE){throw 'ELF、HID、配置或分区检查失败'}
$description=Get-Content "$port/build/project_description.json" -Raw|ConvertFrom-Json
$version=[string]$description.project_version
if($version -notmatch '^\d+\.\d+\.\d+$'){throw '固件版本格式不符'}
$name="usb-display-$version-app-0x60000.bin"
$binary="$port/build/jc4880_usb_display.bin"
if((Get-Item $binary).Length -gt 10752000){throw '应用超出原分区'}
Copy-Item $binary "$out/$name"
@{source_commit=$source;version=$version;file=$name;address='0x60000';capacity=10752000;
  bytes=(Get-Item $binary).Length;sha256=(Get-FileHash $binary).Hash.ToLowerInvariant();device_updated=$false}|
    ConvertTo-Json|Set-Content "$out/candidate.json" -Encoding utf8NoBOM
"已生成 $out/$name；未刷机，实际显示与触摸仍须验收"
