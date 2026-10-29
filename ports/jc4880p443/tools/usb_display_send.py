# /// script
# requires-python = ">=3.12"
# dependencies = ["typer>=0.16,<1", "pillow>=11,<13", "pyusb==1.3.1", "libusb-package>=1.0,<2"]
# ///
"""USB副屏测试发送器：发静态图或镜像主屏，不是Windows虚拟显示驱动。"""
from contextlib import contextmanager
from collections.abc import Iterator
from io import BytesIO
from pathlib import Path
import time
from typing import Protocol

import typer
from PIL import Image, ImageGrab, ImageOps
from usb_display_packet import WIDTH, HEIGHT, encode_jpeg_frame

app = typer.Typer(add_completion=False)


class BulkDevice(Protocol):
    def write(self, endpoint: int, data: bytes, timeout: int) -> int: ...


@contextmanager
def device_connection(pid: int) -> Iterator[BulkDevice]:
    import usb.core
    import usb.util
    import usb.backend.libusb1
    import libusb_package

    backend = usb.backend.libusb1.get_backend(find_library=libusb_package.find_library)
    device = usb.core.find(idVendor=0x303A, idProduct=pid, backend=backend)
    if device is None:
        raise typer.BadParameter('未找到副屏；连接USB3高速口，确认固件及vendor接口驱动')
    # 只认领vendor interface 0，不卸载/替换同设备的HID触摸驱动。
    usb.util.claim_interface(device, 0)
    try:
        yield device
    finally:
        usb.util.release_interface(device, 0)
        usb.util.dispose_resources(device)


def make_jpeg(image: Image.Image, quality: int) -> bytes:
    rgb = ImageOps.pad(image.convert('RGB'), (WIDTH, HEIGHT), color='black')
    output = BytesIO()
    # P4硬件解码使用baseline JPEG，禁止progressive。
    rgb.save(output, 'JPEG', quality=quality, subsampling=2, progressive=False)
    return output.getvalue()


@app.command()
def send(
    image: Path | None = typer.Option(None, exists=True, dir_okay=False, help='发送图片；未指定则镜像主屏'),
    seconds: int = typer.Option(10, min=1, max=3600),
    fps: int = typer.Option(20, min=1, max=30),
    quality: int = typer.Option(75, min=10, max=95),
    pid: int = typer.Option(0x2986, help='默认复合设备PID，十进制输入；旧版2987可显式覆盖'),
) -> None:
    """运行时避免与刷机或其它vendor接口客户端并发占用；不替换HID驱动。"""
    fixed: bytes | None = None
    if image is not None:
        with Image.open(image) as source:
            fixed = make_jpeg(source, quality)
    deadline = time.monotonic() + seconds
    sent = 0
    with device_connection(pid) as device:
        while time.monotonic() < deadline:
            started = time.monotonic()
            if fixed is None:
                with ImageGrab.grab(all_screens=False) as capture:
                    jpeg = make_jpeg(capture, quality)
            else:
                jpeg = fixed
            packet = encode_jpeg_frame(jpeg, sent)
            if device.write(0x01, packet, timeout=2000) != len(packet):
                raise typer.Exit(code=2)
            sent += 1
            time.sleep(max(0, 1/fps - (time.monotonic()-started)))
    typer.echo(f'已发送{sent}帧；这是主机发送次数，不代表屏幕显示FPS')


if __name__ == '__main__':
    app()
