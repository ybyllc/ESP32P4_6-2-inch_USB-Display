"""验证iProduct能被xfz1986的公开配置语法解析；不打开设备。"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def parse_config(text: str) -> tuple[int, int, int, int]:
    width = height = 0
    fps = 60
    limit = 1920 * 1080 * 4
    for field in text.split('_')[1:]:
        size = re.fullmatch(r'R(\d+)x(\d+)', field)
        if size:
            width, height = map(int, size.groups())
        frequency = re.fullmatch(r'Fps(\d+)', field)
        if frequency:
            fps = int(frequency[1])
        budget = re.fullmatch(r'Bl(\d+)', field)
        if budget:
            limit = int(budget[1]) * 1024
    return width, height, fps, limit


def main() -> None:
    # Given旧产品字符串；When按Windows驱动语法解析；Then无法获得设备尺寸。
    assert parse_config('ESP_Extern_Screen')[:2] == (0, 0)
    source = (ROOT / 'main/usb_descriptors.c').read_text(encoding='utf-8')
    index = re.search(r'\.iProduct\s*=\s*(\d+)', source)
    assert index is not None
    array = re.search(r'static const char \*const strings\[\] = \{(.*?)\};', source, re.S)
    assert array is not None
    strings = re.findall(r'"([^"\\]*)"', array[1])
    # Given固件真实iProduct索引；When读该字符串；Then尺寸/帧率/缓冲上限均可解析。
    product = strings[int(index[1])]
    assert parse_config(product) == (480, 800, 30, 292 * 1024)
    assert 292 * 1024 <= 300000
    assert '_Ejpg6_' in product
    assert parse_config(strings[4])[:2] == (480, 800)
    print('PASS: old iProduct misses geometry; actual iProduct advertises 480x800/JPEG/30FPS and bounded KiB budget')


if __name__ == '__main__':
    main()
