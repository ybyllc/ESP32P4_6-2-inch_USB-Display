"""检查主机封包与设备固定小端协议，不打开USB。"""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from usb_display_packet import FRAME_LIMIT, InvalidJpegFrame, encode_jpeg_frame


def main() -> None:
    # Given baseline JPEG与跨10位帧号；When封包；Then设备解析出的尺寸/长度正确。
    jpeg = b'\xff\xd8' + bytes(range(256)) * 3 + b'\xff\xd9'
    packet = encode_jpeg_frame(jpeg, 1025)
    crc, kind, command, x, y, width, height, word = struct.unpack('<HBBHHHHI', packet[:16])
    assert (crc, kind, command, x, y, width, height) == (0, 3, 0, 0, 0, 480, 800)
    assert word & 1023 == 1 and word >> 10 == len(jpeg)
    assert packet[16:] == jpeg and len(packet[:512]) == 512
    # Given非法标记或超限；When封包；Then拒绝，不能向USB发送未校验的帧。
    for invalid in (b'', b'\xff\xd8', b'not-jpeg', b'\xff\xd8' + b'x' * FRAME_LIMIT + b'\xff\xd9'):
        try:
            encode_jpeg_frame(invalid, 0)
        except InvalidJpegFrame as error:
            assert error.length == len(invalid)
        else:
            raise AssertionError('invalid JPEG accepted')
    print('PASS: host header endian/geometry/frame-id/length and invalid JPEG rejection; no USB access')


if __name__ == '__main__':
    main()
