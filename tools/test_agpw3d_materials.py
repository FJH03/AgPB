"""Synthetic VMT/base-color VTF checks; no game files or image packages needed."""
import struct
import unittest
import zlib

from agpw3d_materials import decode_vtf, parse_vtf_header, resolve_basetexture


class MemoryStore:
    def __init__(self, files):
        self.files = {name: text.encode('utf-8') if isinstance(text, str) else text
                      for name, text in files.items()}
        self.reads = []

    def read(self, name):
        self.reads.append(name)
        try:
            return self.files[name]
        except KeyError:
            raise FileNotFoundError(name)


def color_block(a=0xF800, b=0x001F, indices=None):
    indices = [0, 1, 2, 3] * 4 if indices is None else indices
    bits = sum(value << (2 * index) for index, value in enumerate(indices))
    return struct.pack('<HHI', a, b, bits)


def make_vtf(width=4, height=4, image_format=13, mips=None, *, version=(7, 2),
             frames=1, depth=1, image_offset=None, thumbnail=False, flags=0, first_frame=0):
    mips = [color_block()] if mips is None else mips
    resource = version >= (7, 3) and image_offset is not None
    header_size = 88 if resource else 80 if version >= (7, 2) else 64
    header = bytearray(header_size)
    struct.pack_into('<4sIIIHHIHH', header, 0, b'VTF\0', *version, header_size,
                     width, height, flags, frames, first_frame)
    struct.pack_into('<iB', header, 52, image_format, len(mips))
    struct.pack_into('<iBB', header, 57, 13 if thumbnail else -1,
                     4 if thumbnail else 0, 4 if thumbnail else 0)
    if version >= (7, 2):
        struct.pack_into('<H', header, 63, depth)
    if version >= (7, 3):
        struct.pack_into('<I', header, 68, 1 if resource else 0)
    if resource:
        struct.pack_into('<3sBI', header, 80, b'\x30\0\0', 0, image_offset)
        prefix = bytes(header) + bytes(image_offset - header_size)
    else:
        prefix = bytes(header) + (b'THUMB___' if thumbnail else b'')
    return prefix + b''.join(reversed(mips))


def png_pixels(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise AssertionError('Not PNG')
    cursor = 8
    compressed = bytearray()
    width = height = None
    while cursor < len(data):
        length, = struct.unpack_from('>I', data, cursor)
        kind = data[cursor + 4:cursor + 8]
        body = data[cursor + 8:cursor + 8 + length]
        crc, = struct.unpack_from('>I', data, cursor + 8 + length)
        if crc != zlib.crc32(kind + body) & 0xFFFFFFFF:
            raise AssertionError('Invalid PNG CRC')
        if kind == b'IHDR':
            width, height, bits, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            if (bits, color, compression, filtering, interlace) != (8, 2, 0, 0, 0):
                raise AssertionError('Expected opaque RGB PNG')
        elif kind == b'IDAT':
            compressed.extend(body)
        cursor += 12 + length
    raw = zlib.decompress(compressed)
    stride = width * 3
    if len(raw) != (stride + 1) * height:
        raise AssertionError('Invalid PNG scanline length')
    pixels = []
    for row in range(height):
        offset = row * (stride + 1)
        if raw[offset] != 0:
            raise AssertionError('Fixture reader expects filter zero')
        pixels.extend(tuple(raw[start:start + 3])
                      for start in range(offset + 1, offset + 1 + stride, 3))
    return width, height, pixels


class MaterialChecks(unittest.TestCase):
    def test_only_basetexture_is_resolved_and_paths_are_normalized(self):
        store = MemoryStore({'materials/test/material.vmt': r'''
            "AnyShader" {
                "$BaseTexture" "Materials\\BRICK//Wall.VTF"
                "$bumpmap" "never/read/normal"
                "$translucent" "1" "$alphatest" "1" "$nocull" "1"
                "Proxies" { "AnimatedTexture" { "animatedtexturevar" "$basetexture" } }
            }
        '''})
        self.assertEqual(resolve_basetexture(store, r'MATERIALS\\Test//Material.VMT'), 'brick/wall')
        self.assertEqual(store.reads, ['materials/test/material.vmt'])

    def test_patch_include_insert_and_replace_affect_only_base_texture(self):
        store = MemoryStore({
            'materials/base.vmt': 'Shader { "$basetexture" "a/first" }',
            'materials/insert.vmt': 'Patch { include "materials/base.vmt" insert { "$basetexture" "ignored" "$bumpmap" "ignored" } }',
            'materials/replace.vmt': 'Patch { include "insert.vmt" replace { "$basetexture" "Materials/B/Second.VTF" "$translucent" "1" } }',
            'materials/empty.vmt': 'Shader { "$bumpmap" "ignored" }',
            'materials/fill.vmt': 'Patch { include "empty" insert { "$basetexture" "c/fill" } }',
            'materials/replace_missing.vmt': 'Patch { include "empty" replace { "$basetexture" "not_inserted" } }',
            'materials/empty_key.vmt': 'Shader { "$basetexture" "" }',
            'materials/replace_empty.vmt': 'Patch { include "empty_key" replace { "$basetexture" "replaced_empty" } }',
        })
        self.assertEqual(resolve_basetexture(store, 'insert'), 'ignored')
        self.assertEqual(resolve_basetexture(store, 'replace'), 'b/second')
        self.assertEqual(resolve_basetexture(store, 'fill'), 'c/fill')
        self.assertIsNone(resolve_basetexture(store, 'empty'))
        self.assertIsNone(resolve_basetexture(store, 'replace_missing'))
        self.assertEqual(resolve_basetexture(store, 'replace_empty'), 'replaced_empty')

    def test_missing_cycles_depth_and_malformed_materials_fail_clearly(self):
        files = {'materials/a.vmt': 'Patch { include "b" }',
                 'materials/b.vmt': 'Patch { include "a" }',
                 'materials/bad.vmt': 'Shader { "$basetexture" "unterminated',
                 'materials/noinclude.vmt': 'Patch { replace { "$basetexture" "x" } }',
                 'materials/comment.vmt': 'Shader {} /* unfinished'}
        files.update({'materials/deep%d.vmt' % n: 'Patch { include "deep%d" }' % (n + 1)
                      for n in range(35)})
        store = MemoryStore(files)
        with self.assertRaises(FileNotFoundError):
            resolve_basetexture(store, 'missing')
        for material in ('a', 'deep0', 'bad', 'noinclude', 'comment', '../escape'):
            with self.subTest(material=material):
                with self.assertRaises(ValueError):
                    resolve_basetexture(store, material)


class TextureChecks(unittest.TestCase):
    def test_dxt1_endpoints_and_four_color_interpolation(self):
        result = decode_vtf(make_vtf())
        width, height, pixels = png_pixels(result['png'])
        self.assertEqual((width, height), (4, 4))
        self.assertEqual(pixels[:4], [(255, 0, 0), (0, 0, 255), (170, 0, 85), (85, 0, 170)])
        self.assertEqual(result['format'], 'DXT1')
        self.assertEqual(result['version'], '7.2')

    def test_dxt1_three_color_case_and_small_block_edges(self):
        block = color_block(0, 0xFFFF)
        _, _, pixels = png_pixels(decode_vtf(make_vtf(mips=[block]))['png'])
        self.assertEqual(pixels[:4], [(0, 0, 0), (255, 255, 255), (127, 127, 127), (0, 0, 0)])
        _, _, cropped = png_pixels(decode_vtf(make_vtf(width=2, height=1, mips=[block]))['png'])
        self.assertEqual(cropped, pixels[:2])

    def test_dxt3_and_dxt5_decode_four_colors_and_ignore_stored_alpha(self):
        colors = color_block(0x001F, 0xF800)
        for image_format in (14, 15):
            images = []
            for alpha in (bytes(8), bytes([255]) * 8):
                images.append(decode_vtf(make_vtf(image_format=image_format, mips=[alpha + colors])))
            self.assertEqual(images[0]['png'], images[1]['png'])
            _, _, pixels = png_pixels(images[0]['png'])
            self.assertEqual(pixels[:4], [(0, 0, 255), (255, 0, 0), (85, 0, 170), (170, 0, 85)])

    def test_uncompressed_channel_order_and_opaque_policy(self):
        for image_format, payload in ((0, bytes([1, 2, 3, 0])), (2, bytes([1, 2, 3])),
                                      (3, bytes([3, 2, 1])), (12, bytes([3, 2, 1, 0])),
                                      (16, bytes([3, 2, 1, 255]))):
            with self.subTest(image_format=image_format):
                result = decode_vtf(make_vtf(width=1, height=1, image_format=image_format, mips=[payload]))
                self.assertEqual(png_pixels(result['png'])[2], [(1, 2, 3)])

    def test_existing_mip_is_selected_and_legacy_thumbnail_is_skipped(self):
        red = color_block(indices=[0] * 16)
        blue = color_block(0x001F, 0, [0] * 16)
        green = color_block(0x07E0, 0, [0] * 16)
        white = color_block(0xFFFF, 0, [0] * 16)
        data = make_vtf(width=8, height=4, mips=[red * 2, blue, green, white], thumbnail=True)
        result = decode_vtf(data, max_size=4)
        self.assertEqual((result['width'], result['height'], result['source_width'], result['source_height']), (4, 2, 8, 4))
        self.assertEqual(set(png_pixels(result['png'])[2]), {(0, 0, 255)})

    def test_v73_resource_offset_and_frame_zero_depth_zero_are_used(self):
        red = color_block(indices=[0] * 16)
        blue = color_block(0x001F, 0, [0] * 16)
        data = make_vtf(version=(7, 3), image_offset=128, mips=[red])
        self.assertEqual(parse_vtf_header(data)['image_offset'], 128)
        self.assertEqual(set(png_pixels(decode_vtf(data)['png'])[2]), {(255, 0, 0)})
        # Each mip contains every frame; each frame contains all depth slices.
        volume = make_vtf(mips=[red + blue + blue + blue, blue + red], frames=2, depth=2)
        self.assertEqual(set(png_pixels(decode_vtf(volume, max_size=4)['png'])[2]), {(255, 0, 0)})
        self.assertEqual(set(png_pixels(decode_vtf(volume, max_size=2)['png'])[2]), {(0, 0, 255)})

    def test_cube_mips_skip_other_faces_and_frames(self):
        red = color_block(indices=[0] * 16)
        blue = color_block(0x001F, 0, [0] * 16)
        green = color_block(0x07E0, 0, [0] * 16)
        data = make_vtf(version=(7, 5), flags=0x4000, frames=2,
                        mips=[red + blue * 11, green + blue * 11])
        self.assertEqual(parse_vtf_header(data)['faces'], 6)
        self.assertEqual(set(png_pixels(decode_vtf(data, max_size=4)['png'])[2]), {(255, 0, 0)})
        self.assertEqual(set(png_pixels(decode_vtf(data, max_size=2)['png'])[2]), {(0, 255, 0)})

    def test_versions_unsupported_formats_and_truncation(self):
        for minor in range(6):
            data = make_vtf(version=(7, minor))
            self.assertEqual(decode_vtf(data)['version'], '7.%d' % minor)
        wrong_format = bytearray(make_vtf())
        struct.pack_into('<i', wrong_format, 52, 999)
        bad_resource = bytearray(make_vtf(version=(7, 3), image_offset=128))
        struct.pack_into('<I', bad_resource, 84, 4)
        for data in (b'VTF', make_vtf()[:-1], wrong_format, bad_resource):
            with self.assertRaises(ValueError):
                decode_vtf(data)
        with self.assertRaises(ValueError):
            decode_vtf(make_vtf(width=8, height=8, mips=[color_block() * 4]), max_size=4)


if __name__ == '__main__':
    unittest.main()
