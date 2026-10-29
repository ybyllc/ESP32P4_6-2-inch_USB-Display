"""解析HID短描述项的输入位数；用于从最终ELF校验触摸报告长度。"""
from collections.abc import Mapping


class HidDescriptorError(RuntimeError):
    pass


def input_report_bytes(descriptor: bytes) -> Mapping[int, int]:
    size = count = report_id = 0
    totals: dict[int, int] = {}
    stack: list[tuple[int, int, int]] = []
    offset = 0
    while offset < len(descriptor):
        prefix = descriptor[offset]
        offset += 1
        if prefix == 0xFE:
            raise HidDescriptorError('该固定描述符不应包含HID长项')
        length = prefix & 3
        length = 4 if length == 3 else length
        if offset + length > len(descriptor):
            raise HidDescriptorError('HID描述项截断')
        value = int.from_bytes(descriptor[offset:offset + length], 'little')
        offset += length
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        match (kind, tag):
            case (1, 7):
                size = value
            case (1, 8):
                report_id = value
            case (1, 9):
                count = value
            case (1, 10):
                stack.append((size, count, report_id))
            case (1, 11):
                if not stack:
                    raise HidDescriptorError('HID全局栈下溢')
                size, count, report_id = stack.pop()
            case (0, 8):
                if size <= 0 or size > 32 or count <= 0 or count > 1024:
                    raise HidDescriptorError('HID输入项宽度或数量不合法')
                totals[report_id] = totals.get(report_id, 0) + size * count
            case _:
                # 其它合法HID项不影响输入报告长度，包含Usage、Collection、Feature等。
                continue
    if any(bits % 8 for bits in totals.values()):
        raise HidDescriptorError('输入报告位数不是整字节')
    return {identifier: bits // 8 for identifier, bits in totals.items()}
