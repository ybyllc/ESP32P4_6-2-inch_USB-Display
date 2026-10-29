"""HID描述符长度解析的独立边界回归，不访问设备。"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from usb_hid_report import HidDescriptorError, input_report_bytes


def main() -> None:
    # Given五指槽每槽80位、触点数8位；When解析；Then载荷51字节而非41字节。
    descriptor = bytes([0x85, 1, 0x75, 8, 0x95, 50, 0x81, 2, 0x95, 1, 0x81, 2])
    assert input_report_bytes(descriptor) == {1: 51}
    assert input_report_bytes(bytes([0x85, 1, 0x75, 8, 0x95, 41, 0x81, 2])) == {1: 41}
    # Given嵌套全局字段；When push/pop；Then独立报告ID不能串算。
    assert input_report_bytes(bytes([0x85, 1, 0x75, 8, 0x95, 1, 0xA4,
                                     0x85, 2, 0x95, 2, 0x81, 2, 0xB4, 0x81, 2])) == {1: 1, 2: 2}
    # Given截断、长项、非整字节或下溢；When解析；Then拒绝无效描述符。
    for bad in (bytes([0x76, 1]), bytes([0xFE]), bytes([0xB4]),
                bytes([0x75, 1, 0x95, 1, 0x81, 2])):
        try:
            input_report_bytes(bad)
        except HidDescriptorError:
            pass
        else:
            raise AssertionError('malformed HID descriptor accepted')
    print('PASS: 51 vs 41 payload, report IDs, global push/pop, truncation and bit alignment')


if __name__ == '__main__':
    main()
