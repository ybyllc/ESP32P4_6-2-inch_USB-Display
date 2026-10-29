# 只生成C协议/HID封包的小型主机测试，不构建ESP固件或访问USB。
$ErrorActionPreference='Stop'
$port=[IO.Path]::GetFullPath("$PSScriptRoot/..")
$out="$port/output/tests"
[IO.Directory]::CreateDirectory($out)|Out-Null
foreach($case in @(@('usb_display_protocol_test','udisp_protocol'),@('usb_display_touch_report_test','touch_report'))){
    $name=$case[0];$implementation=$case[1]
    & gcc -std=c11 -Wall -Wextra -Werror -I "$port/main" "$port/tests/$name.c" "$port/main/$implementation.c" -o "$out/$name.exe"
    if($LASTEXITCODE){throw "主机测试编译失败：$name"}
    & "$out/$name.exe"
    if($LASTEXITCODE){throw "主机测试失败：$name"}
}
foreach($name in @('usb_display_packet_test','usb_display_windows_contract_test','usb_display_composite_test','usb_hid_report_test')){
    & python "$port/tests/$name.py"
    if($LASTEXITCODE){throw "Python检查失败：$name"}
}
if($env:IDF_PATH){
    & python "$port/tests/usb_display_rgb565_test.py"
    if($LASTEXITCODE){throw '实际SDK字节序检查失败'}
}
& python "$port/tools/check_sources.py"
if($LASTEXITCODE){throw '来源或路径检查失败'}
& "$port/tests/touch_mapping_rect_test.ps1"
