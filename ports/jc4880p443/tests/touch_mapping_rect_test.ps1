# 只测试实际修复脚本中的区域匹配函数，不查询或修改Windows设备。
$ErrorActionPreference='Stop'
$script=Join-Path $PSScriptRoot '../tools/repair_touch_mapping.ps1'
$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($script,[ref]$null,[ref]$errors)
if($errors){throw $errors}
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-UdispMappedRect'},$true)
if(-not $function){throw '未找到真实映射匹配函数'}
. ([scriptblock]::Create($function.Extent.Text))
# Given主屏和横向副屏；When比较；Then不能因尺寸/偏移相似误认为已正确关联。
$screen=[pscustomobject]@{x=298;y=1080;width=800;height=480}
$primary=[pscustomobject]@{left=0;top=0;right=1920;bottom=1080}
$secondary=[pscustomobject]@{left=298;top=1080;right=1098;bottom=1560}
if(Test-UdispMappedRect $primary $screen){throw '错误接受主屏映射'}
if(-not (Test-UdispMappedRect $secondary $screen)){throw '错误拒绝横向副屏'}
# Given副屏改纵向；When区域尚未更新；Then旧横向映射不能冒充纵向正确。
$portrait=[pscustomobject]@{x=298;y=1080;width=480;height=800}
if(Test-UdispMappedRect $secondary $portrait){throw '错误接受旧横向区域'}
$rotated=[pscustomobject]@{left=298;top=1080;right=778;bottom=1880}
if(-not (Test-UdispMappedRect $rotated $portrait)){throw '错误拒绝纵向副屏'}
'PASS: primary mismatch, secondary offset, landscape and portrait mapping checks'
