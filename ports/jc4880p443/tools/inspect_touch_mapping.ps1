# 只读Windows官方Pointer API，确认触摸实际绑定的显示区域；不改注册表或固件。
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class UdispPointerMapping {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct Device {
        public uint orientation;
        public IntPtr device;
        public uint type;
        public IntPtr monitor;
        public uint firstCursor;
        public ushort maxContacts;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=520)] public string product;
    }
    [DllImport("user32.dll", EntryPoint="GetPointerDevices", SetLastError=true)]
    public static extern bool Count(ref uint count, IntPtr devices);
    [DllImport("user32.dll", EntryPoint="GetPointerDevices", SetLastError=true)]
    public static extern bool Devices(ref uint count, [In,Out] Device[] devices);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool GetPointerDeviceRects(IntPtr device, out Rect input, out Rect display);
}
'@
$count=[uint32]0
if(-not [UdispPointerMapping]::Count([ref]$count,[IntPtr]::Zero)){
    throw "GetPointerDevices失败，Win32错误$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}
if($count -eq 0){Write-Output '{"pointer_devices":[]}';return}
$devices=New-Object 'UdispPointerMapping+Device[]' $count
if(-not [UdispPointerMapping]::Devices([ref]$count,$devices)){throw '无法读取Pointer设备'}
$results=@(for($i=0;$i -lt $count;$i++){
    $device=$devices[$i]
    $input=New-Object UdispPointerMapping+Rect
    $display=New-Object UdispPointerMapping+Rect
    if(-not [UdispPointerMapping]::GetPointerDeviceRects($device.device,[ref]$input,[ref]$display)){continue}
    [pscustomobject]@{product=$device.product;type=$device.type;orientation=$device.orientation;
        max_contacts=$device.maxContacts;input=$input;display=$display}
})
Add-Type -AssemblyName System.Windows.Forms
$screens=@([Windows.Forms.Screen]::AllScreens|ForEach-Object {
    [pscustomobject]@{name=$_.DeviceName;primary=$_.Primary;x=$_.Bounds.X;y=$_.Bounds.Y;
                     width=$_.Bounds.Width;height=$_.Bounds.Height}
})
@{pointer_devices=$results;screens=$screens;read_only=$true}|ConvertTo-Json -Depth 5
