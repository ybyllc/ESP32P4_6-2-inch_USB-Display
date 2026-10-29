# 默认只读检查；-Apply仅用于用户专门准备的IDF6工作区，不隐式修改共享SDK。
param([string]$IdfPath=$env:IDF_PATH,[switch]$Apply)
$ErrorActionPreference='Stop'
if(-not $IdfPath){throw '先激活ESP-IDF v6.0.2环境或传入-IdfPath'}
$sdk=[IO.Path]::GetFullPath($IdfPath)
$port=[IO.Path]::GetFullPath("$PSScriptRoot/..")
$source=Get-Content "$port/source.json" -Raw|ConvertFrom-Json
$actual=(git -C $sdk rev-parse HEAD).Trim()
if($LASTEXITCODE -or $actual -ne $source.sdk_commit){throw 'SDK提交不符，请使用source.json固定的IDF6版本'}
$patch=Join-Path $port $source.sdk_required_patch
if((Get-FileHash $patch).Hash.ToLowerInvariant() -ne $source.sdk_patch_sha256){throw 'SDK补丁文件哈希不符'}
git -C $sdk apply --reverse --check $patch 2>$null
if($LASTEXITCODE -eq 0){'SDK版本及真实DMA切帧完成补丁检查通过';return}
git -C $sdk apply --check $patch 2>$null
if($LASTEXITCODE){throw 'DPI源码有其它变化，不能自动覆盖；请审查SDK工作区'}
if(-not $Apply){throw '缺少DPI完成通知补丁；在专用SDK副本确认后用-Apply准备'}
git -C $sdk apply $patch
if($LASTEXITCODE){throw 'DPI补丁应用失败'}
'已在指定SDK应用DPI完成通知补丁；未操作设备'
