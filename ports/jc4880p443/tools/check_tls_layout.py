#!/usr/bin/env python3
"""核对RISC-V ELF的TLS模板与FreeRTOS初始化符号，刷前拒绝错位。"""
import argparse
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile


def check_linker_tls_alignment(path):
    # ELF恰好对齐也可能掩盖旧脚本，必须确认模板修复实际进入生成产物。
    script = ''.join(Path(path).read_text().split())
    expected = '.=ALIGN(MAX(ALIGNOF(.flash.tdata),ALIGNOF(.flash.tbss)));'
    if expected not in script:
        raise ValueError('生成链接脚本未包含TLS前驱对齐修复，拒绝复用旧产物')


def inspect_tls(path):
    with Path(path).open('rb') as stream:
        elf = ELFFile(stream)
        segments = [segment for segment in elf.iter_segments() if segment['p_type'] == 'PT_TLS']
        symbols = elf.get_section_by_name('.symtab')
        names = ['_thread_local_data_start', '_thread_local_data_end',
                 '_thread_local_bss_start', '_thread_local_bss_end']
        if len(segments) != 1 or symbols is None:
            raise ValueError('缺少唯一PT_TLS或符号表，不能核对TLS')
        values = {}
        for name in names:
            matches = symbols.get_symbol_by_name(name)
            if not matches or len(matches) != 1:
                raise ValueError('缺少唯一TLS初始化符号：' + name)
            values[name] = matches[0]['st_value']
        data_start, data_end, bss_start, bss_end = [values[name] for name in names]
        tls = segments[0]
        # 运行时以data_start为TP并顺序复制/清零；编译器以PT_TLS为零偏移。
        if tls['p_vaddr'] != data_start:
            raise ValueError(f'TLS起点错位：PT_TLS={tls["p_vaddr"]:#x}，FreeRTOS={data_start:#x}')
        if not data_start <= data_end == bss_start <= bss_end:
            raise ValueError('TLS初始化段不连续或边界逆序')
        if tls['p_memsz'] != bss_end - data_start:
            raise ValueError('PT_TLS大小与FreeRTOS初始化范围不一致')
        if tls['p_filesz'] > data_end - data_start:
            raise ValueError('TLS初始化数据超出运行时复制范围')
        # 当前port仅保证16字节栈对齐；更高对齐需同时修改运行时，不能盲刷。
        if tls['p_align'] > 16 or data_start % max(1, tls['p_align']):
            raise ValueError('TLS对齐超出当前FreeRTOS运行时约束')
        return {'start': hex(data_start), 'data_bytes': data_end - data_start,
                'bss_bytes': bss_end - bss_start, 'alignment': tls['p_align']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect_tls(args.elf), ensure_ascii=False))
