"""Read-only base-color VMT resolution and opaque VTF previews.

Only $basetexture is used. VMT shaders, alpha and other material parameters do
not affect rendering. VTF layouts follow the SDK's public/vtf/vtf.h and vtf.cpp;
compressed color blocks use the standard BC1/BC2/BC3 color interpolation rules.
"""
from __future__ import annotations

import posixpath
import struct
import zlib


_FORMATS = {0: ('RGBA8888', 4), 2: ('RGB888', 3), 3: ('BGR888', 3),
            12: ('BGRA8888', 4), 13: ('DXT1', 8), 14: ('DXT3', 16),
            15: ('DXT5', 16), 16: ('BGRX8888', 4), 20: ('DXT1_ONEBITALPHA', 8)}
_BLOCK_FORMATS = {13, 14, 15, 20}


def _path(value: str, extension: str) -> str:
    name = value.strip().replace('\\', '/').lower()
    if not name or name.startswith('/') or ':' in name or '..' in name.split('/'):
        raise ValueError('Material/texture path must be relative to materials/')
    name = posixpath.normpath(name)
    if name.startswith('materials/'):
        name = name[len('materials/'):]
    if name.endswith(extension):
        name = name[:-len(extension)]
    if not name or name == '.':
        raise ValueError('Empty material/texture path')
    return name


def _keyvalues(text: str) -> dict:
    """Small strict KeyValues reader; unused material blocks are only parsed."""
    tokens = []
    offset = 0
    while offset < len(text):
        char = text[offset]
        if char.isspace():
            offset += 1
        elif text.startswith('//', offset):
            end = text.find('\n', offset)
            offset = len(text) if end < 0 else end + 1
        elif text.startswith('/*', offset):
            end = text.find('*/', offset + 2)
            if end < 0:
                raise ValueError('Unterminated VMT comment')
            offset = end + 2
        elif char in '{}':
            tokens.append(char)
            offset += 1
        elif char == '"':
            offset += 1
            value = []
            while offset < len(text) and text[offset] != '"':
                if text[offset] == '\\' and offset + 1 < len(text) and text[offset + 1] in ('"', '\\'):
                    offset += 1
                value.append(text[offset])
                offset += 1
            if offset >= len(text):
                raise ValueError('Unterminated VMT string')
            tokens.append(''.join(value))
            offset += 1
        else:
            end = offset
            while end < len(text) and not text[end].isspace() and text[end] not in '{}"':
                end += 1
            if end == offset:
                raise ValueError('Invalid VMT token')
            tokens.append(text[offset:end])
            offset = end
    cursor = 0

    def block(nested=False, depth=0):
        nonlocal cursor
        if depth > 64:
            raise ValueError('VMT KeyValues nesting exceeds 64 levels')
        result = {}
        while cursor < len(tokens):
            key = tokens[cursor]
            cursor += 1
            if key == '}':
                if nested:
                    return result
                raise ValueError('Unexpected VMT closing brace')
            if key.startswith('[') and key.endswith(']'):
                continue
            if key in ('{', '') or key.startswith('#') or cursor == len(tokens):
                raise ValueError('Invalid or unsupported VMT key/value')
            value = tokens[cursor]
            cursor += 1
            if value == '{':
                value = block(True, depth + 1)
            elif value == '}':
                raise ValueError('Missing VMT value')
            result[key.lower()] = value
        if nested:
            raise ValueError('Missing VMT closing brace')
        return result

    return block()


def resolve_basetexture(store, material_path: str) -> str | None:
    """Resolve only $basetexture, including Patch include/insert/replace.

    The result is a normalized texture name without materials/ or .vtf. Missing
    resources propagate OSError; malformed materials/cycles raise ValueError.
    A material with no base texture returns None. Other parameters are ignored.
    """
    cache = {}

    def resolve(name, stack):
        name = _path(name, '.vmt')
        if name in stack:
            raise ValueError('VMT Patch include cycle: ' + ' -> '.join((*stack, name)))
        if len(stack) >= 32:
            raise ValueError('VMT Patch include depth exceeds 32 materials')
        if name in cache:
            return cache[name]
        raw = store.read('materials/' + name + '.vmt')
        if len(raw) > 4 * 1024 * 1024:
            raise ValueError('VMT is too large: ' + name)
        tree = _keyvalues(raw.decode('utf-8-sig', errors='replace'))
        if len(tree) != 1:
            raise ValueError('VMT must contain one shader block: ' + name)
        shader, params = next(iter(tree.items()))
        if not isinstance(params, dict):
            raise ValueError('VMT shader must contain a block: ' + name)
        if shader == 'patch':
            include = params.get('include')
            if not isinstance(include, str) or not include.strip():
                raise ValueError('VMT Patch has no include: ' + name)
            present, value = resolve(include, (*stack, name))
            for operation in ('insert', 'replace'):
                patch = params.get(operation, {})
                if not isinstance(patch, dict):
                    raise ValueError('Invalid VMT Patch ' + operation + ': ' + name)
                # SDK ApplyPatchKeyValues: insert adds/overwrites; replace
                # only touches an existing key, even if its value was empty.
                if '$basetexture' in patch and (operation == 'insert' or present):
                    present, value = True, patch['$basetexture']
        else:
            present, value = '$basetexture' in params, params.get('$basetexture')
        if value is not None:
            if not isinstance(value, str):
                raise ValueError('VMT $basetexture must be a string: ' + name)
            value = _path(value, '.vtf') if value.strip() else None
        cache[name] = (present, value)
        return cache[name]

    return resolve(material_path, ())[1]


def _span(data, offset, size, label):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('Truncated/invalid VTF ' + label)


def _size(width, height, image_format):
    if image_format not in _FORMATS:
        raise ValueError('Unsupported VTF image format: %d' % image_format)
    unit = _FORMATS[image_format][1]
    if image_format in _BLOCK_FORMATS:
        return ((width + 3) // 4) * ((height + 3) // 4) * unit
    return width * height * unit


def parse_vtf_header(data: bytes) -> dict:
    """Validate VTF 7.0-7.5 layout and locate its high-resolution image data."""
    _span(data, 0, 64, 'header')
    signature, major, minor, header_size = struct.unpack_from('<4sIII', data)
    if signature != b'VTF\0' or major != 7 or not 0 <= minor <= 5:
        raise ValueError('Unsupported VTF signature/version')
    minimum = 80 if minor >= 3 else 65 if minor >= 2 else 64
    if header_size < minimum:
        raise ValueError('Invalid VTF header size')
    _span(data, 0, header_size, 'header size')
    width, height, flags, frames, first_frame = struct.unpack_from('<HHIHH', data, 16)
    if not 1 <= width <= 16384 or not 1 <= height <= 16384 or not 1 <= frames <= 4096:
        raise ValueError('Invalid VTF dimensions/frame count')
    image_format, = struct.unpack_from('<i', data, 52)
    if image_format not in _FORMATS:
        raise ValueError('Unsupported VTF image format: %d' % image_format)
    mip_count = data[56]
    depth = struct.unpack_from('<H', data, 63)[0] if minor >= 2 else 1
    if not 1 <= depth <= 2048 or not 1 <= mip_count <= max(width, height, depth).bit_length():
        raise ValueError('Invalid VTF mip/depth count')
    faces = 1
    if flags & 0x4000:
        faces = 7 if 1 <= minor <= 4 and first_frame != 0xFFFF else 6
    image_offset = None
    if minor >= 3:
        resources, = struct.unpack_from('<I', data, 68)
        if resources > 4096 or 80 + resources * 8 > header_size:
            raise ValueError('Invalid VTF resource directory')
        for index in range(resources):
            offset = 80 + index * 8
            tag, resource_flags = data[offset:offset + 3], data[offset + 3]
            location, = struct.unpack_from('<I', data, offset + 4)
            if tag == b'\x30\0\0':
                if image_offset is not None or resource_flags & 2:
                    raise ValueError('Invalid/duplicate VTF image resource')
                image_offset = location
        if resources and image_offset is None:
            raise ValueError('VTF resource directory has no image data')
    if image_offset is None:
        low_format, = struct.unpack_from('<i', data, 57)
        low_width, low_height = data[61:63]
        thumbnail = _size(low_width, low_height, low_format) if low_width and low_height else 0
        image_offset = header_size + thumbnail
    if image_offset < header_size:
        raise ValueError('VTF image data overlaps header')
    total = sum(_size(max(1, width >> level), max(1, height >> level), image_format)
                * max(1, depth >> level) * faces * frames for level in range(mip_count))
    _span(data, image_offset, total, 'image payload')
    return dict(width=width, height=height, depth=depth, frames=frames, faces=faces,
                mip_count=mip_count, image_offset=image_offset, image_format=image_format,
                format=_FORMATS[image_format][0], version=f'{major}.{minor}')


def _rgb565(value):
    r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _decode_rgb(payload, width, height, image_format):
    """Decode color only; stored alpha never affects the opaque RGB result."""
    result = bytearray(width * height * 3)
    if image_format not in _BLOCK_FORMATS:
        stride = _FORMATS[image_format][1]
        if image_format == 2:
            return bytes(payload)
        bgr = image_format in (3, 12, 16)
        for index in range(width * height):
            source = index * stride
            r, g, b = payload[source:source + 3]
            if bgr:
                r, b = b, r
            result[index * 3:index * 3 + 3] = bytes((r, g, b))
        return bytes(result)
    block_size = _FORMATS[image_format][1]
    blocks_x = (width + 3) // 4
    for by in range((height + 3) // 4):
        for bx in range(blocks_x):
            offset = (by * blocks_x + bx) * block_size
            if image_format in (14, 15):
                offset += 8  # BC2/BC3 alpha block is intentionally ignored.
            endpoint0, endpoint1, bits = struct.unpack_from('<HHI', payload, offset)
            c0, c1 = _rgb565(endpoint0), _rgb565(endpoint1)
            if image_format in (14, 15) or endpoint0 > endpoint1:
                c2 = tuple((2 * c0[i] + c1[i]) // 3 for i in range(3))
                c3 = tuple((c0[i] + 2 * c1[i]) // 3 for i in range(3))
            else:
                c2 = tuple((c0[i] + c1[i]) // 2 for i in range(3))
                c3 = (0, 0, 0)
            colors = [bytes(value) for value in (c0, c1, c2, c3)]
            for y in range(min(4, height - by * 4)):
                dest = ((by * 4 + y) * width + bx * 4) * 3
                row_bits = bits >> (8 * y)
                for x in range(min(4, width - bx * 4)):
                    result[dest + x * 3:dest + x * 3 + 3] = colors[(row_bits >> (2 * x)) & 3]
    return bytes(result)


def _png_rgb(rgb, width, height):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)
    stride = width * 3
    scanlines = b''.join(b'\0' + rgb[row:row + stride] for row in range(0, len(rgb), stride))
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
            + chunk(b'IDAT', zlib.compress(scanlines, 3)) + chunk(b'IEND', b''))


def decode_vtf(data: bytes, max_size: int = 512) -> dict:
    """Decode frame 0 / face 0 / depth slice 0 from an existing fitting mip.

    PNG output is always opaque RGB; transparency, shader parameters and color
    effects are outside this preview's scope. Unsupported layouts fail clearly.
    """
    if not isinstance(max_size, int) or not 1 <= max_size <= 4096:
        raise ValueError('VTF preview max_size must be between 1 and 4096')
    header = parse_vtf_header(data)
    chosen = next((level for level in range(header['mip_count'])
                   if max(1, header['width'] >> level) <= max_size
                   and max(1, header['height'] >> level) <= max_size), None)
    if chosen is None:
        raise ValueError('No existing VTF mip fits the preview max_size')
    width, height = max(1, header['width'] >> chosen), max(1, header['height'] >> chosen)
    fmt = header['image_format']
    offset = header['image_offset']
    for level in range(header['mip_count'] - 1, chosen, -1):
        offset += (_size(max(1, header['width'] >> level), max(1, header['height'] >> level), fmt)
                   * max(1, header['depth'] >> level) * header['faces'] * header['frames'])
    size = _size(width, height, fmt)
    rgb = _decode_rgb(memoryview(data)[offset:offset + size], width, height, fmt)
    return dict(png=_png_rgb(rgb, width, height), width=width, height=height,
                source_width=header['width'], source_height=header['height'],
                format=header['format'], version=header['version'])
