"""Pure-memory AGPW editing checks; no engine, files, or game install required."""
import hashlib
import struct
import unittest

from agpw3d_document import (AgpwDocument, DocumentConflict, FIELD_SPECS,
                             HEADER_SIZE, KNOWN_FLAGS_MASK, RECORD_SIZE, schema)


def complex_bytes():
    # Deliberately invalid UTF-8 plus embedded NULs and nonzero padding bytes.
    map_name = b'test_map\0\xff\x80' + bytes(range(21))
    author = bytes(range(255, 223, -1))
    header = struct.pack('<III32s32s', 0x50424741, 1, 3, map_name, author)
    record = struct.Struct('<3fIBB8h8Hf')
    origins = ((-0.0, 1.25, -4.0), (100, -200, 0.125), (2, 3, 4))
    flags = ((1 << 31) | (1 << 8) | (1 << 2) | (1 << 29), 0, 0xFFFFFFFF)
    chunks = []
    for index in range(3):
        # Includes invalid positive links, repeated references, negative slots,
        # and arbitrary flags in empty slots. Editing must not repair any of it.
        links = (-1, 32767, -123, 0, 0, -1, 2, -32768)
        link_flags = (0xFFFF, 0x8000, 0x1234, 0xFEDC, 1, 0xCCCC, 0, 0xABCD)
        chunks.append(record.pack(*origins[index], flags[index], 17 + index, 243 - index,
                                  *links, *link_flags, -0.0 if index == 1 else 0.75))
    return header + b''.join(chunks) + b'\x00TRAILER\xff\x80\x01\x02'


def record_offset(index):
    return 76 + index * 54


def old_flags(data, index):
    return struct.unpack_from('<I', data, record_offset(index) + 12)[0]


class DocumentChecks(unittest.TestCase):
    def setUp(self):
        self.raw = complex_bytes()
        self.document = AgpwDocument(self.raw, 'selected.agpw')

    def assert_only_offsets_change(self, before, after, allowed):
        self.assertEqual(len(before), len(after))
        changed = {index for index, (a, b) in enumerate(zip(before, after)) if a != b}
        self.assertTrue(changed.issubset(set(allowed)), changed - set(allowed))

    def test_original_snapshot_hash_and_empty_export_are_byte_exact(self):
        self.assertEqual((HEADER_SIZE, RECORD_SIZE), (76, 54))
        self.assertEqual(self.document.count, 3)
        self.assertEqual(self.document.source_name, 'selected.agpw')
        self.assertEqual(self.document.source_hash, hashlib.sha256(self.raw).hexdigest())
        self.assertEqual(self.document.data, self.raw)
        self.assertEqual(self.document.export_changes([]), self.raw)
        self.assertEqual(self.document.export_changes([{'index': 0}]), self.raw)
        self.assertEqual(self.document.export_changes([], self.document.source_hash), self.raw)

    def test_radius_changes_exactly_one_byte_and_retains_all_flag_and_link_bytes(self):
        exported = self.document.export_changes([{'index': 0, 'radius': 255}])
        expected = bytearray(self.raw)
        expected[record_offset(0) + 16] = 255
        self.assertEqual(exported, bytes(expected))
        self.assert_only_offsets_change(self.raw, exported, [record_offset(0) + 16])
        self.assertEqual(old_flags(exported, 0), old_flags(self.raw, 0))
        self.assertEqual(self.document.data, self.raw)

    def test_known_flag_changes_preserve_unknown_bits_and_only_patch_uint32(self):
        unknown = old_flags(self.raw, 0) & (~KNOWN_FLAGS_MASK & 0xFFFFFFFF)
        value = unknown | (1 << 27) | (1 << 30)
        exported = self.document.export_changes([{'index': 0, 'flags': value}])
        expected = bytearray(self.raw)
        struct.pack_into('<I', expected, record_offset(0) + 12, value)
        self.assertEqual(exported, bytes(expected))
        self.assert_only_offsets_change(self.raw, exported, range(record_offset(0) + 12, record_offset(0) + 16))
        self.assertTrue(old_flags(exported, 0) & (1 << 31))
        self.assertTrue(old_flags(exported, 0) & (1 << 8))

    def test_integer_boundaries_and_multiple_records_patch_without_reserializing(self):
        changes = [{'index': 0, 'radius': 0}, {'index': 1, 'radius': 255, 'flags': 0},
                   {'index': 2, 'flags': 0xFFFFFFFF}]
        exported = self.document.export_changes(changes)
        expected = bytearray(self.raw)
        expected[record_offset(0) + 16] = 0
        expected[record_offset(1) + 16] = 255
        struct.pack_into('<I', expected, record_offset(1) + 12, 0)
        struct.pack_into('<I', expected, record_offset(2) + 12, 0xFFFFFFFF)
        self.assertEqual(exported, bytes(expected))

    def test_unknown_flag_bits_cannot_be_cleared_or_added(self):
        original = old_flags(self.raw, 0)
        for value in (0, original ^ (1 << 31), original | (1 << 0)):
            with self.subTest(flags=value):
                with self.assertRaisesRegex(ValueError, 'Unknown flag bits must be preserved'):
                    self.document.export_changes([{'index': 0, 'flags': value}])
        self.assertEqual(self.document.data, self.raw)

    def test_invalid_edits_and_duplicate_indices_are_rejected(self):
        invalid = (None, {}, (), 'edits', [None], [[]], [{}],
                   [{'index': True}], [{'index': 1.0}], [{'index': -1}], [{'index': 3}],
                   [{'index': 0, 'origin': [0, 0, 0]}], [{'index': 0, 123: 1}],
                   [{'index': 0, 'radius': True}], [{'index': 0, 'radius': -1}],
                   [{'index': 0, 'radius': 256}], [{'index': 0, 'radius': 2.5}],
                   [{'index': 0, 'radius': '1'}], [{'index': 0, 'radius': None}],
                   [{'index': 0, 'flags': False}], [{'index': 0, 'flags': -1}],
                   [{'index': 0, 'flags': 0x100000000}], [{'index': 0, 'flags': 1.5}],
                   [{'index': 0, 'radius': 1}, {'index': 0, 'radius': 2}])
        for changes in invalid:
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    self.document.export_changes(changes)
        self.assertEqual(self.document.data, self.raw)

    def test_hash_conflicts_are_distinct_and_even_empty_changes_check_the_hash(self):
        self.assertTrue(issubclass(DocumentConflict, ValueError))
        with self.assertRaises(DocumentConflict):
            self.document.export_changes([], expected_hash='0' * 64)
        with self.assertRaises(ValueError) as raised:
            self.document.export_changes([], expected_hash=123)
        self.assertNotIsInstance(raised.exception, DocumentConflict)
        self.assertEqual(self.document.data, self.raw)
        self.assertEqual(self.document.source_hash, hashlib.sha256(self.raw).hexdigest())

    def test_failed_transaction_and_later_exports_never_mutate_the_document(self):
        first = self.document.export_changes([{'index': 0, 'radius': 9}])
        with self.assertRaises(ValueError):
            self.document.export_changes([{'index': 0, 'radius': 7}, {'index': 1, 'radius': 256}])
        second = self.document.export_changes([{'index': 1, 'radius': 8}])
        self.assertEqual(first[record_offset(1) + 16], self.raw[record_offset(1) + 16])
        self.assertEqual(second[record_offset(0) + 16], self.raw[record_offset(0) + 16])
        self.assertEqual(self.document.data, self.raw)
        self.assertEqual(self.document.source_hash, hashlib.sha256(self.raw).hexdigest())

    def test_mutable_input_is_snapshotted_and_public_properties_are_read_only(self):
        mutable = bytearray(self.raw)
        document = AgpwDocument(mutable, 'copied.agpw')
        mutable[-1] ^= 255
        self.assertEqual(document.data, self.raw)
        with self.assertRaises(AttributeError):
            document.data = b'other bytes'
        with self.assertRaises(AttributeError):
            document.source_hash = 'wrong'

    def test_schema_lists_exact_current_bits_and_returns_independent_objects(self):
        metadata = schema()
        fields = {field['key']: field for field in metadata['fields']}
        self.assertEqual(set(fields), {'radius', 'flags'})
        self.assertEqual((fields['radius']['type'], fields['radius']['min'], fields['radius']['max']), ('integer', 0, 255))
        self.assertEqual((fields['flags']['min'], fields['flags']['max']), (0, 0xFFFFFFFF))
        expected_bits = [1, 2, 3, 4, 5, 6, 7, 9, 11, 12, 17, 26, 27, 28, 29, 30]
        self.assertEqual([flag['bit'] for flag in metadata['flags']], [1 << bit for bit in expected_bits])
        self.assertEqual(metadata['known_flags_mask'], sum(1 << bit for bit in expected_bits))
        self.assertEqual(self.document.known_flags_mask, metadata['known_flags_mask'])
        self.assertEqual(self.document.schema(), metadata)
        metadata['fields'][0]['max'] = -1
        metadata['flags'][0]['bit'] = 0
        self.assertEqual(schema()['fields'][0]['max'], 255)
        self.assertEqual(schema()['flags'][0]['bit'], 2)
        self.assertEqual(FIELD_SPECS['radius'].offset, 16)
        self.assertEqual(FIELD_SPECS['flags'].offset, 12)

    def test_malformed_headers_records_and_nonfinite_values_are_rejected(self):
        invalid = [b'AGPW', 'not bytes']
        for offset, value in ((0, 0), (4, 2), (8, 8193), (8, 10)):
            raw = bytearray(self.raw)
            struct.pack_into('<I', raw, offset, value)
            invalid.append(bytes(raw))
        invalid.append(self.raw[:76 + 3 * 54 - 1])
        for offset, value in ((76, float('nan')), (76 + 50, float('inf')), (76 + 54, -float('inf'))):
            raw = bytearray(self.raw)
            struct.pack_into('<f', raw, offset, value)
            invalid.append(bytes(raw))
        for raw in invalid:
            with self.subTest(raw=repr(raw)[:60]):
                with self.assertRaises(ValueError):
                    AgpwDocument(raw, 'invalid.agpw')


if __name__ == '__main__':
    unittest.main()
