# ESP32-P4 6.2 寸 USB 副屏

基于 ESP32-P4 + AXS15260 6.2 寸 MIPI-DSI 屏的 USB 副屏 / 触摸屏固件。

**硬件与详细说明：[https://oshwhub.com/an_ye/project_ygsitoma](https://oshwhub.com/an_ye/project_ygsitoma)**

## 功能

- **USB 副屏**：Windows 驱动通过 USB vendor 接口下发 JPEG 帧，硬件 JPEG 解码后显示在 452×1280 屏上
- **触摸屏**：支持 5 点 HID 触摸上报，可作为主机触摸屏使用
- **MSC U 盘**：assets 分区以只读 U 盘形式暴露，可查看固件内置视频资源
- **Idle 空闲页**：USB 未连接/仅供电时显示 LVGL 空闲页（背景视频循环播放、连接状态、设置页）；检测到视频流后 3 秒倒计时切换为副屏，主机断开后自动回到空闲页
- **设置页**：页面隐藏、HID 触摸开关，配置保存在 NVS

## 构建与烧录

使用 ESP-IDF 5.5（ESP32-P4 目标）：

```bash
idf.py build
```

只烧录应用：

```bash
idf.py -p COMx app-flash
```

全量烧录（bootloader + 分区表 + app + assets）：

```bash
idf.py -p COMx flash
```

## 合成固件

提供当前版本的全量固件（bootloader、分区表、app、assets 合并），可从 0x0 一次写入：

```bash
python -m esptool --chip esp32p4 write_flash 0x0 XXXXXX.bin
```

## 视频资源打包

空闲页背景视频位于 assets 分区的 `video/video.bin`，由 MP4 转换打包生成：

```powershell
.\tools\generate_video_bin.ps1 -InputFile untitled.mp4
```

脚本依赖 `tools\ffmpeg\bin` 下的 ffmpeg/ffprobe，也可直接运行 `tools\pack_video.py`。
