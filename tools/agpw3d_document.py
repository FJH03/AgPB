"""Byte-preserving AGPW edit documents for the independent waypoint tool.

This module has no engine/plugin dependencies and performs no file I/O. Edits
are validated as a transaction, then patch only the registered field bytes.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
import struct
from types import MappingProxyType


HEADER_SIZE = 76
RECORD_SIZE = 54
MAX_WAYPOINTS = 8192
_MAGIC = 0x50424741
_VERSION = 1


class DocumentConflict(ValueError):
    """The caller's expected source hash does not match this document."""


@dataclass(frozen=True)
class FieldSpec:
    """One editable integer field, expressed relative to its AGPW record."""
    key: str
    offset: int
    format: str
    minimum: int
    maximum: int
    label: str
    preserve_mask: int = 0

    def encode(self, value) -> bytes:
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError('%s must be an integer' % self.key)
        if not self.minimum <= value <= self.maximum:
            raise ValueError('%s must be between %d and %d' %
                             (self.key, self.minimum, self.maximum))
        return struct.pack(self.format, value)

    def schema(self) -> dict:
        return dict(key=self.key, type='integer', min=self.minimum,
                    max=self.maximum, label=self.label)


# Exact supported bits from src/waypoint.h; undefined bits remain valid data.
KNOWN_FLAGS = (
    (1 << 1, 'LIFT', '电梯'),
    (1 << 2, 'CROUCH', '蹲伏'),
    (1 << 3, 'CROSSING', '穿越点'),
    (1 << 4, 'GOAL', '任务目标'),
    (1 << 5, 'LADDER', '梯子'),
    (1 << 6, 'RESCUE', '人质救援'),
    (1 << 7, 'CAMP', '蹲守'),
    (1 << 9, 'DJUMP', '连跳'),
    (1 << 11, 'AVOID', '尽量避开'),
    (1 << 12, 'USEBUTTON', '使用按钮'),
    (1 << 17, 'FALLRISK', '禁止侧移'),
    (1 << 26, 'FALLCHECK', '检查脚下'),
    (1 << 27, 'JUMP', '跳跃'),
    (1 << 28, 'SNIPER', '狙击'),
    (1 << 29, 'TERRORIST', 'T 专用'),
    (1 << 30, 'COUNTER', 'CT 专用'),
)
KNOWN_FLAGS_MASK = sum(bit for bit, _, _ in KNOWN_FLAGS)
known_flags_mask = KNOWN_FLAGS_MASK


# Adding a field here provides its validation, serialization and UI metadata.
# File offsets match waypoint.cpp's explicit AGPW v1 serializer, not C++ padding.
FIELD_SPECS = MappingProxyType({spec.key: spec for spec in (
    FieldSpec('radius', 16, '<B', 0, 255, '半径'),
    FieldSpec('flags', 12, '<I', 0, 0xFFFFFFFF, '标志',
              preserve_mask=(~KNOWN_FLAGS_MASK) & 0xFFFFFFFF),
)})


def schema() -> dict:
    """Return fresh JSON-compatible field and flag metadata."""
    return dict(fields=[spec.schema() for spec in FIELD_SPECS.values()],
                flags=[dict(bit=bit, name=name, label=label) for bit, name, label in KNOWN_FLAGS],
                known_flags_mask=KNOWN_FLAGS_MASK)


class AgpwDocument:
    """An immutable source snapshot; each export returns a separate byte result.

    `flags` edits are complete uint32 values whose unknown bits must equal the
    source flags. Known bits may be added or removed freely. Invalid requests
    fail rather than silently changing the requested value. All other bytes stay
    unchanged, including empty link slots and trailing data.
    """
    __slots__ = ('_data', '_source_name', '_source_hash', '_count')

    def __init__(self, data: bytes, source_name: str):
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise ValueError('AGPW content must be bytes')
        raw = bytes(data)
        if len(raw) < HEADER_SIZE:
            raise ValueError('AGPW header is truncated')
        magic, version, count = struct.unpack_from('<III', raw)
        if magic != _MAGIC or version != _VERSION:
            raise ValueError('Unsupported AGPW magic/version')
        if count > MAX_WAYPOINTS or HEADER_SIZE + count * RECORD_SIZE > len(raw):
            raise ValueError('Invalid AGPW count or truncated records')
        for index in range(count):
            offset = HEADER_SIZE + index * RECORD_SIZE
            values = (*struct.unpack_from('<3f', raw, offset),
                      struct.unpack_from('<f', raw, offset + 50)[0])
            if not all(math.isfinite(value) for value in values):
                raise ValueError('Non-finite coordinate/gravity at waypoint #%d' % index)
        self._data = raw
        self._source_name = str(source_name)
        self._source_hash = hashlib.sha256(raw).hexdigest()
        self._count = count

    @property
    def data(self) -> bytes:
        return self._data

    @property
    def source_name(self) -> str:
        return self._source_name

    @property
    def source_hash(self) -> str:
        return self._source_hash

    @property
    def count(self) -> int:
        return self._count

    @property
    def known_flags_mask(self) -> int:
        return KNOWN_FLAGS_MASK

    @staticmethod
    def schema() -> dict:
        return schema()

    def export_changes(self, changes: list[dict], expected_hash: str | None = None) -> bytes:
        """Validate all edits, reject duplicate indices, then patch a fresh copy."""
        if expected_hash is not None:
            if not isinstance(expected_hash, str):
                raise ValueError('expected_hash must be a string or null')
            if expected_hash != self._source_hash:
                raise DocumentConflict('Source AGPW hash does not match the expected hash')
        if not isinstance(changes, list):
            raise ValueError('changes must be a list')
        updates = []
        seen = set()
        for change in changes:
            if not isinstance(change, dict):
                raise ValueError('Each change must be an object')
            unknown = set(change) - {'index'} - FIELD_SPECS.keys()
            if unknown:
                raise ValueError('Unsupported edit field: %s' %
                                 ', '.join(str(key) for key in sorted(unknown, key=str)))
            if 'index' not in change:
                raise ValueError('Each change requires a waypoint index')
            index = change['index']
            if isinstance(index, bool) or not isinstance(index, int):
                raise ValueError('Waypoint index must be an integer')
            if not 0 <= index < self._count:
                raise ValueError('Waypoint index is outside the document: %d' % index)
            if index in seen:
                raise ValueError('Duplicate waypoint index in changes: %d' % index)
            seen.add(index)
            record = HEADER_SIZE + index * RECORD_SIZE
            for key, spec in FIELD_SPECS.items():
                if key in change:
                    encoded = spec.encode(change[key])
                    if spec.preserve_mask:
                        old_value = struct.unpack_from(spec.format, self._data, record + spec.offset)[0]
                        if (change[key] & spec.preserve_mask) != (old_value & spec.preserve_mask):
                            raise ValueError('Unknown flag bits must be preserved')
                    updates.append((record + spec.offset, encoded))
        if not updates:
            return self._data
        result = bytearray(self._data)
        for offset, encoded in updates:
            result[offset:offset + len(encoded)] = encoded
        return bytes(result)
