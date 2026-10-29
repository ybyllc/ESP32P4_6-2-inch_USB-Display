"""检查真实固件的vendor/HID拆分及发送器默认PID；不装驱动、不打开USB。"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    source = (ROOT / 'main/usb_descriptors.c').read_text(encoding='utf-8')
    config = (ROOT / 'main/tusb_config.h').read_text(encoding='utf-8')
    # Given当前Windows INF用2987匹配整机、2986&MI_00匹配子接口；When切到2986；Then允许Usbccgp拆分HID。
    assert re.search(r'^#define UDISP_USB_PID 0x2986$', source, re.M)
    assert 'TUD_CONFIG_DESCRIPTOR(1, 2,' in source
    assert 'TUD_VENDOR_DESCRIPTOR(0, 4, 0x01, 0x81, packet)' in source
    assert 'TUD_HID_DESCRIPTOR(1, 5,' in source
    assert '#define CFG_TUD_HID 1' in config and '#define CFG_TUD_VENDOR 1' in config
    assert 'TUD_HID_REPORT_DESC_TOUCH_SCREEN(REPORT_ID_TOUCH, 480, 800)' in source
    # Given主机测试工具；When默认连接；Then与新固件PID一致，不操作显示器/HID驱动。
    host = (ROOT / 'tools/usb_display_send.py').read_text(encoding='utf-8')
    assert 'typer.Option(0x2986,' in host
    assert 'claim_interface(device, 0)' in host
    main_source = (ROOT / 'main/main.c').read_text(encoding='utf-8')
    assert 'buffer[0]=1' in main_source and 'REPORT_ID_MAX_COUNT && length' in main_source
    assert 'usb_display_touch_report(report, pressed, last_x, last_y)' in main_source
    print('PASS: 2986 composite vendor MI_00 + HID MI_01, native 480x800 descriptor and matching host PID')


if __name__ == '__main__':
    main()
