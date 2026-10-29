"""核对副屏实际配置、链接符号、既有分区及TLS；兼容确实没有TLS的精简C固件。"""
import json
from pathlib import Path
import struct
import sys
from typing import Final, TypedDict

from elftools.elf.elffile import ELFFile
from check_tls_layout import inspect_tls
from usb_hid_report import input_report_bytes

TLS_SYMBOLS: Final = ('_thread_local_data_start', '_thread_local_data_end',
                      '_thread_local_bss_start', '_thread_local_bss_end')


class ArtifactError(RuntimeError):
    pass


class CheckResult(TypedDict):
    tls: str
    required_symbols: bool
    cpu_mhz: int
    psram_mhz: int
    partition_offset: str
    hid_touch_report_bytes: int
    usb_pid: str


def inspect(build: Path) -> CheckResult:
    elf_path = build / 'jc4880_usb_display.elf'
    with elf_path.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        if symbols is None:
            raise ArtifactError('ELF缺少符号表')
        reports = symbols.get_symbol_by_name('report')
        if not reports or len(reports) != 1:
            raise ArtifactError('缺少唯一的实际HID报告描述符')
        symbol = reports[0]
        section = elf.get_section(symbol['st_shndx'])
        start = symbol['st_value'] - section['sh_addr']
        descriptor = section.data()[start:start + symbol['st_size']]
        report_sizes = input_report_bytes(descriptor)
        if report_sizes.get(1) != 51:
            raise ArtifactError('最终HID触摸描述符不是51字节报告')
        devices = symbols.get_symbol_by_name('device')
        if not devices or len(devices) != 1:
            raise ArtifactError('缺少唯一的实际USB设备描述符')
        device_symbol = devices[0]
        device_section = elf.get_section(device_symbol['st_shndx'])
        device_start = device_symbol['st_value'] - device_section['sh_addr']
        device = device_section.data()[device_start:device_start + device_symbol['st_size']]
        if len(device) != 18 or struct.unpack_from('<H', device, 10)[0] != 0x2986 or device[4] != 0:
            raise ArtifactError('实际USB描述符不是2986标准复合设备')
        for name in ('app_main', 'jpeg_decoder_process', 'tud_vendor_rx_cb',
                     'tud_hid_descriptor_report_cb', 'tud_hid_get_report_cb', 'usb_display_touch_report',
                     'tud_descriptor_other_speed_configuration_cb'):
            matches = symbols.get_symbol_by_name(name)
            if not matches or not any(symbol['st_shndx'] != 'SHN_UNDEF' and
                                      symbol['st_info']['bind'] == 'STB_GLOBAL' for symbol in matches):
                raise ArtifactError('缺少真实链接符号：' + name)
        tls_present = any(segment['p_type'] == 'PT_TLS' for segment in elf.iter_segments())
        if not tls_present:
            nonempty_tls = any(section['sh_flags'] & 0x400 and section['sh_size'] for section in elf.iter_sections())
            boundaries: list[int] = []
            for name in TLS_SYMBOLS:
                matches = symbols.get_symbol_by_name(name)
                if not matches or len(matches) != 1:
                    raise ArtifactError('空TLS也必须保留唯一边界符号：' + name)
                boundaries.append(matches[0]['st_value'])
            if nonempty_tls or len(set(boundaries)) != 1:
                raise ArtifactError('缺失PT_TLS但存在非空TLS数据，拒绝产物')
    if tls_present:
        inspect_tls(elf_path)
    config = (build / 'config/sdkconfig.h').read_text(encoding='utf-8')
    for value in ('#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 400', '#define CONFIG_SPIRAM_SPEED 200',
                  '#define CONFIG_ESP32P4_REV_MIN_FULL 301'):
        if value not in config.splitlines():
            raise ArtifactError('实际生成配置不符：' + value)
    table = (build / 'partition_table/partition-table.bin').read_bytes()
    found = False
    for offset in range(0, len(table)-31, 32):
        magic, kind, subtype, address, capacity, label, _flags = struct.unpack_from('<HBBII16sI', table, offset)
        if magic == 0x50AA and kind == 0 and subtype == 0:
            found = address == 0x60000 and capacity == 10752000 and label.rstrip(b'\0') == b'factory'
    if not found:
        raise ArtifactError('实际分区表不是已确认的P4应用槽')
    return {'tls': 'checked' if tls_present else 'empty_verified', 'required_symbols': True,
            'cpu_mhz': 400, 'psram_mhz': 200, 'partition_offset': '0x60000',
            'hid_touch_report_bytes': 51, 'usb_pid': '0x2986'}


if __name__ == '__main__':
    print(json.dumps(inspect(Path(sys.argv[1])), ensure_ascii=False))
