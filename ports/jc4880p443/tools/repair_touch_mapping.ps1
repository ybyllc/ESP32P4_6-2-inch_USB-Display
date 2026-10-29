# Windows官方关联向导：不写私有注册表，不在固件里猜桌面偏移或重复旋转。
# -Setup可能需要用户在真实副屏上点一下，鼠标点击不能替代触摸设备身份识别。
param([switch]$Setup)
$ErrorActionPreference='Stop'
function Test-UdispMappedRect($DisplayRect,$Screen){
    return $DisplayRect.left -eq $Screen.x -and $DisplayRect.top -eq $Screen.y -and
        ($DisplayRect.right-$DisplayRect.left) -eq $Screen.width -and
        ($DisplayRect.bottom-$DisplayRect.top) -eq $Screen.height
}
$hid=@(Get-PnpDevice -PresentOnly|Where-Object {
    $_.InstanceId -match '^HID\\VID_303A&PID_2986&MI_01' -and $_.Status -eq 'OK'
})
if($hid.Count -ne 1){throw '没有唯一可用的副屏HID触摸接口；先确认副屏0.1.3固件和USB连接，不能给手机固件设置副屏映射'}
$state=(& "$PSScriptRoot/inspect_touch_mapping.ps1"|Out-String)|ConvertFrom-Json
$targets=@($state.screens|Where-Object {
    ($_.width -eq 480 -and $_.height -eq 800) -or ($_.width -eq 800 -and $_.height -eq 480)
})
if($targets.Count -ne 1){throw '无法唯一识别480×800副屏，停止修改；不按DISPLAY固定编号猜目标'}
$target=$targets[0]
$touches=@($state.pointer_devices|Where-Object {$_.type -eq 3})
if($touches.Count -ne 1){throw '存在零个或多个触摸设备，需在系统向导中明确选择；不能冒改其它触摸屏映射'}
$touch=$touches[0]
if(Test-UdispMappedRect $touch.display $target){
    @{mapped=$true;display=$target;input=$touch.input;orientation=$touch.orientation;
      changed=$false;physical_test='pending'}|ConvertTo-Json -Depth 4
    return
}
@{mapped=$false;actual=$touch.display;expected=$target;input=$touch.input;changed=$false}|ConvertTo-Json -Depth 4
if(-not $Setup){'请使用-Setup启动Windows官方触摸关联向导';return}
$wizard=Join-Path $env:SystemRoot 'System32/MultiDigiMon.exe'
if(-not (Test-Path $wizard)){throw 'Windows缺少MultiDigiMon关联工具，未修改注册表或触摸坐标'}
'即将显示触摸关联提示：用Enter跳过非副屏，提示到达副屏后，用手指点一下副屏。Esc可取消。'
# 前台等待整个向导，供外层device_session在所有识别操作期间保持共享锁。
$process=Start-Process -FilePath $wizard -ArgumentList '-touch' -WindowStyle Normal -PassThru
$process.WaitForExit()
$after=(& "$PSScriptRoot/inspect_touch_mapping.ps1"|Out-String)|ConvertFrom-Json
$afterTouches=@($after.pointer_devices|Where-Object {$_.type -eq 3})
if($afterTouches.Count -eq 1 -and (Test-UdispMappedRect $afterTouches[0].display $target)){
    'Windows报告触摸已关联副屏；仍需实际点按角落及切换方向确认'
}else{
    throw '向导结束，但Windows尚未报告正确关联；没有修改固件或其它显示器设置'
}
