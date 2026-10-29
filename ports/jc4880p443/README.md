# JC4880P443 USB副屏移植版 0.1.3

本目录是GUITION JC4880P443C_I_W / ESP32-P4 rev3.2的独立工程，移植自本仓库作者的USB副屏方案。作者根目录的452×1280 AXS15260工程保持原样；本版为480×800 ST7701 + GT911，不依赖任何手机系统源码或本地绝对路径。

## 功能与实际状态

- 高速USB vendor接收JPEG，P4硬件解码为LCD原生小端RGB565；双缓冲以真实DMA切换事件回收，不提前复用正在扫描的帧。
- 有界接收池与最新帧队列；协议检查尺寸、类型和长度，坏包/断线/缓冲耗尽均有回退。独立接收与解码任务，不因排队持续累积延迟。
- Windows产品字符串报告480×800、JPEG、目标30FPS；VID/PID为`303a:2986`，显示驱动只绑定MI_00，MI_01为标准HID触摸。
- GT911单点触摸，严格匹配作者的51字节HID布局（五个10字节槽+触点数）；抬手保留最后位置，最大触点Feature为1。不是作者原板五点触摸的完整移植。
- CPU400MHz、PSRAM200MHz、DSI28MHz，理论面板扫描约49.8Hz。30FPS是目标配置，不是已测稳定性能。
- 专版不启动Brookesia、LVGL、C6/Wi-Fi、音频、U盘和待机视频，不挂载手机用户文件系统。

原开发仓库已生成0.1.3固件并完成协议、HID报告、最终ELF检查；用户先前确认0.1.1可出图，但报告颜色异常，随后0.1.2修正JPEG字节序。0.1.3复合接口与物理触摸、颜色复核、热插拔、长稳和实际FPS仍待硬件验收。本次移入fork仅做局部测试与路径/来源校验，没有再次完整编译或刷机。

## Windows使用

使用作者原Windows程序/xfz1986驱动。已核对的驱动1.2.25.2支持`2986&MI_00`；新固件应枚举成USB复合设备、独立显示接口和HID兼容触摸屏。不安装/修改第三方INF，不用Zadig覆盖整个复合设备。

烧录使用调试口；传图连接板上**USB3高速Type-C口**，需数据线。仅更新原应用槽`0x60000`，容量10,752,000字节，不全片擦除，不写bootloader、分区表、C6或资源。先备份设备当时的实际应用槽，恢复手机时写回该备份。本仓库不提供自动刷机操作。

若触摸落到其它显示器，在Windows“控制面板→Tablet PC设置→设置”将触摸关联到副屏，固件不能猜测Windows显示器排列。

可用只读工具确认实际关联，再按需启动官方向导：

```powershell
pwsh -File tools/inspect_touch_mapping.ps1
pwsh -File tools/repair_touch_mapping.ps1 -Setup
```

工具使用`GetPointerDevices/GetPointerDeviceRects`比较触摸当前显示区域与唯一480×800/800×480副屏，不按固定DISPLAY编号猜目标。只有确认唯一副屏、唯一HID与唯一触摸设备才启动Windows内置`MultiDigiMon.exe -touch`，不写私有注册表。向导中用Enter跳过其它屏幕，提示到副屏后需用真实手指点一下；鼠标点击不能替代设备身份识别。向导结束再次读取实际关联区域，没有匹配则报失败，不声称已修复。

若仍在原手机开发仓库共享设备环境中使用，须从该仓库`tools/device_session.py`持锁入口前台执行完整向导，避免与烧录/串口任务并发。横纵屏切换由Windows在正确关联后转换，暂不在固件重复旋转。若关联已正确但实际方向仍错，需进一步采集原始触点及显示方向，不直接假定关联是所有方向错误的唯一原因。

`tools/usb_display_send.py`可用来发图或镜像主屏，**不会创建虚拟扩展桌面**；使用它需独立vendor WinUSB/libusb绑定，不能与已占用vendor接口的显示驱动同时使用。作者显示驱动工作时无需启动该工具。

## 构建

1. 准备并激活ESP-IDF **v6.0.2**。固定提交和驱动哈希见`source.json`。不要使用作者原工程的IDF5.5配置替代本端口配置。
2. 在专用SDK副本应用DPI真实帧完成补丁：

```powershell
# 在本目录执行；默认只检查，-Apply明确允许修改所指定的SDK源码
pwsh -File tools/check_sdk.ps1 -IdfPath $env:IDF_PATH -Apply
```

3. 工程与依赖版本已固定在manifest/lock；本板ST7701驱动及许可证位于`components/esp_lcd_st7701`，HID报告头直接复用根目录`main/tusb/usb_descriptors.h`，无需下载个人厂商包。
4. 运行局部测试和构建：

```powershell
pwsh -File tools/test.ps1
pwsh -File tools/build.ps1 -Jobs 12
```

Linux/macOS可在已激活IDF环境中先校验并应用相同SDK补丁，再运行：

```sh
python tools/check_sources.py
idf.py -B build reconfigure
cmake --build build --target app --parallel 12
python tools/check_usb_display_artifact.py build
```

产物位于`output/release/`，构建入口仅生成应用并检查实际PID、HID输入长度、TLS及分区，不刷设备，不打包个人Flash/NVS。`sdkconfig`、依赖下载、固件和私有日志不入Git。

## 来源

保留作者原仓库历史及MIT触摸描述符声明。ST7701驱动保留Espressif Apache-2.0声明和`license.txt`，本板初始化/时序来自GUITION已验证驱动，文本换行规范化为LF并去掉行尾空格。原始及入库哈希均见`source.json`。迁移提交对应关系和未完成项见[MIGRATION.md](MIGRATION.md)。
