"""核对移植输入来源和目录独立性，不调用编译器、不访问设备。"""
import hashlib
import json
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]


def main() -> None:
    manifest = json.loads((PORT / 'source.json').read_text(encoding='utf-8'))
    repo = PORT.parents[1]
    header = repo / manifest['hid_header']
    assert hashlib.sha256(header.read_bytes()).hexdigest() == manifest['hid_sha256'], 'upstream HID header changed'
    assert hashlib.sha256((PORT / manifest['sdk_required_patch']).read_bytes()).hexdigest() == manifest['sdk_patch_sha256']
    for relative, expected in manifest['driver_sha256'].items():
        path = PORT / 'components/esp_lcd_st7701' / relative
        assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, relative
    cmake = (PORT / 'main/CMakeLists.txt').read_text(encoding='utf-8')
    assert '${CMAKE_CURRENT_LIST_DIR}/../../..' in cmake
    assert '.tools/' not in cmake and 'brookesia' not in cmake
    config = (PORT / 'sdkconfig.defaults').read_text(encoding='utf-8')
    assert 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"' in config
    assert '../brookesia_super' not in config
    print('PASS: pinned HID header, display driver hashes and standalone port paths')


if __name__ == '__main__':
    main()
