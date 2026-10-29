"""核对真实SDK的JPEG字节序语义和本板配置；不打开设备。"""
from pathlib import Path
import re
import struct
import os

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    sdk = Path(os.environ['IDF_PATH'])
    header = (sdk / 'components/esp_driver_jpeg/include/driver/jpeg_types.h').read_text()
    driver = (sdk / 'components/esp_driver_jpeg/jpeg_decode.c').read_text()
    receiver = (ROOT / 'main/receiver.c').read_text(encoding='utf-8')
    # Given实际SDK；When检查大/小端契约；Then确认这不是LCD红蓝通道顺序开关。
    assert re.search(r'JPEG_DEC_RGB_ELEMENT_ORDER_BGR[^\n]+small endian', header)
    assert re.search(r'JPEG_DEC_RGB_ELEMENT_ORDER_RGB[^\n]+big endian', header)
    function = driver[driver.index('static void jpeg_dec_config_dma_csc('):]
    function = function[:function.index('// Configure color space conversion option.')]
    assert 'post_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0' in function
    assert 'rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_RGB' in function
    assert 'post_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_0_1' in function
    assert '.rgb_order=JPEG_DEC_RGB_ELEMENT_ORDER_BGR' in receiver
    assert '.conv_std=JPEG_YUV_RGB_CONV_STD_BT601' in receiver
    # Given红绿蓝白；When错误地按大端输出给小端面板；Then彩色错而白色仍相同。
    for color in (0xF800, 0x07E0, 0x001F):
        assert struct.unpack('<H', struct.pack('>H', color))[0] != color
        assert struct.unpack('<H', struct.pack('<H', color))[0] == color
    assert struct.unpack('<H', struct.pack('>H', 0xFFFF))[0] == 0xFFFF
    print('PASS: actual SDK endian contract and native RGB565; RGB fail/white unchanged regression')


if __name__ == '__main__':
    main()
