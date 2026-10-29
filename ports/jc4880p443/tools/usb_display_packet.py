"""panyihang udisp包格式；不打开设备，供发送端和测试共用。"""
from typing import Final
import struct

WIDTH: Final = 480
HEIGHT: Final = 800
FRAME_LIMIT: Final = 300_000


def encode_jpeg_frame(jpeg: bytes, frame_id: int) -> bytes:
    """单次bulk写入，USB栈按512/64字节分包；头部只放在每帧首包。"""
    if not 4 <= len(jpeg) <= FRAME_LIMIT or not jpeg.startswith(b'\xff\xd8') or not jpeg.endswith(b'\xff\xd9'):
        raise InvalidJpegFrame(len(jpeg))
    word = ((len(jpeg) << 10) | (frame_id & 1023))
    return struct.pack('<HBBHHHHI', 0, 3, 0, 0, 0, WIDTH, HEIGHT, word) + jpeg


class InvalidJpegFrame(Exception):
    def __init__(self, length: int) -> None:
        self.length = length
        super().__init__(f'JPEG标记或大小不符：{length}字节，最大{FRAME_LIMIT}')
