"""Synthetic HTTP checks for byte-preserving AGPW edit downloads.

All files live in TemporaryDirectory; these tests never inspect installed maps
or permit the export endpoint to write a source/destination file.
"""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import agpw_view3d as viewer
from agpw3d_document import schema
from test_agpw_view3d import (
    get_json, make_bsp, post_waypoint, running_viewer, waypoint_bytes, write_gameinfo,
)


_UNKNOWN_FLAG = 0x80000000
_ORIGINAL_FLAGS = _UNKNOWN_FLAG | (1 << 1)


def source_bytes():
    """Keep an untouched second record, unknown flags, signed zero and a tail."""
    first = waypoint_bytes(origin=(-0.0, 2, 3), flags=_ORIGINAL_FLAGS)
    second = waypoint_bytes(origin=(15, -20, 31), target=0, flags=0x10000000)
    result = bytearray(first)
    struct.pack_into('<I', result, 8, 2)
    result.extend(second[viewer.HEADER.size:])
    result.extend(b'\x00opaque-tail\xfe\xff')
    return bytes(result)


def export_payload(manifest, changes):
    return dict(scene_id=manifest['scene_id'],
                source_hash=manifest['editor']['source_hash'], changes=changes)


def export_request(base, payload):
    data = json.dumps(payload, ensure_ascii=True, allow_nan=True).encode('utf-8')
    return Request(base + 'export.agpw', data=data,
                   headers={'Content-Type': 'application/json'}, method='POST')


def download(base, payload):
    with urlopen(export_request(base, payload), timeout=5) as response:
        return response.read(), response.headers


@contextmanager
def running_path_viewer(game_dir, source_path):
    manifest, resources = viewer.build_scene(game_dir / 'maps' / 'test_map.bsp',
                                            source_path, game_dir)
    with viewer.ViewerServer(0, manifest, resources, source_path, game_dir=game_dir) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = 'http://127.0.0.1:%d/%s/' % (server.server_address[1], server.token)
        try:
            yield server, url
        finally:
            server.shutdown()
            thread.join(timeout=2)


class EditorHTTPChecks(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix='agpw3d-editor-http-')
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.game = self.root / 'game'
        write_gameinfo(self.game)
        (self.game / 'maps').mkdir()
        (self.game / 'maps' / 'test_map.bsp').write_bytes(make_bsp())
        self.original = source_bytes()
        self.source = self.root / 'original.agpw'
        self.source.write_bytes(self.original)
        self.source_digest = hashlib.sha256(self.original).hexdigest()

    def assertRejected(self, request, codes=(400, 422)):
        with self.assertRaises(HTTPError) as raised:
            with urlopen(request, timeout=5) as response:
                response.read()
        code = raised.exception.code
        raised.exception.close()
        self.assertIn(code, codes)

    def test_import_manifest_editor_uses_document_schema_and_raw_source_hash(self):
        with running_viewer(self.game) as (_, url):
            manifest = post_waypoint(url, self.original, 'chosen.agpw')
            self.assertEqual(manifest['editor']['source_hash'], self.source_digest)
            for key, value in schema().items():
                self.assertEqual(manifest['editor'][key], value)
            self.assertFalse(schema()['known_flags_mask'] & _UNKNOWN_FLAG)
            self.assertEqual(get_json(url + 'scene.json')['editor'], manifest['editor'])

    def test_export_changes_only_registered_field_bytes_and_reimport_reads_result(self):
        with running_viewer(self.game) as (_, url):
            manifest = post_waypoint(url, self.original)
            flags = _UNKNOWN_FLAG | (1 << 2) | (1 << 27)
            exported, headers = download(url, export_payload(manifest, [dict(index=0, radius=123, flags=flags)]))
            expected = bytearray(self.original)
            struct.pack_into('<I', expected, viewer.HEADER.size + 12, flags)
            expected[viewer.HEADER.size + 16] = 123
            self.assertEqual(exported, bytes(expected))
            self.assertIn('application/octet-stream', headers.get('Content-Type', ''))
            disposition = headers.get('Content-Disposition', '').lower()
            self.assertTrue(disposition.startswith('attachment'))
            self.assertIn('.agpw', disposition)
            # Export is a download, not an implicit commit to the open document.
            unchanged = get_json(url + 'scene.json')
            self.assertEqual(unchanged['editor']['source_hash'], self.source_digest)
            self.assertEqual(unchanged['waypoints']['points'][0]['radius'], 64)
            self.assertEqual(unchanged['waypoints']['points'][0]['flags'], _ORIGINAL_FLAGS)
            imported = post_waypoint(url, exported, 'edited.agpw')
            self.assertEqual(imported['waypoints']['points'][0]['radius'], 123)
            self.assertEqual(imported['waypoints']['points'][0]['flags'], flags)
            self.assertEqual(imported['editor']['source_hash'], hashlib.sha256(exported).hexdigest())
            self.assertEqual(imported['waypoints']['points'][1], manifest['waypoints']['points'][1])
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_zero_changes_are_bit_exact_and_radius_only_preserves_unknown_flags(self):
        with running_viewer(self.game) as (_, url):
            manifest = post_waypoint(url, self.original)
            untouched, _ = download(url, export_payload(manifest, []))
            self.assertEqual(untouched, self.original)
            exported, _ = download(url, export_payload(manifest, [dict(index=0, radius=0)]))
            expected = bytearray(self.original)
            expected[viewer.HEADER.size + 16] = 0
            self.assertEqual(exported, bytes(expected))
            self.assertEqual(struct.unpack_from('<I', exported, viewer.HEADER.size + 12)[0], _ORIGINAL_FLAGS)
            untouched_again, _ = download(url, export_payload(manifest, []))
            self.assertEqual(untouched_again, self.original)

    def test_invalid_edits_are_rejected_atomically_without_changing_open_snapshot(self):
        invalid = (
            [dict(index=-1, radius=4)], [dict(index=2, radius=4)],
            [dict(index=True, radius=4)], [dict(index='0', radius=4)],
            [dict(index=0, radius=-1)], [dict(index=0, radius=256)],
            [dict(index=0, radius=1.5)], [dict(index=0, radius='4')],
            [dict(index=0, radius=True)], [dict(index=0, radius=float('nan'))],
            [dict(index=0, flags=-1)], [dict(index=0, flags=0x100000000)],
            [dict(index=0, flags=True)], [dict(index=0, flags=3.5)],
            [dict(index=0, flags=1 << 2)],  # Clearing the existing unknown bit.
            [dict(index=0, flags=_ORIGINAL_FLAGS | 1)],  # Adding another unknown bit.
            [dict(index=0, origin=[0, 0, 0])], [dict(index=0, mesh=9)],
            [dict(index=0, radius=7), dict(index=0, flags=0)],
            [dict(index=0, radius=7), dict(index=9, radius=10)],
            [dict(radius=7)], ['not an edit'], None, {},
        )
        with running_viewer(self.game) as (_, url):
            manifest = post_waypoint(url, self.original)
            for changes in invalid:
                with self.subTest(changes=changes):
                    self.assertRejected(export_request(url, export_payload(manifest, changes)))
            after, _ = download(url, export_payload(manifest, []))
            self.assertEqual(after, self.original)

    def test_malformed_export_bodies_are_rejected(self):
        with running_viewer(self.game) as (_, url):
            manifest = post_waypoint(url, self.original)
            for data in (b'not json', b'null', b'[]', b'{}'):
                with self.subTest(data=data):
                    request = Request(url + 'export.agpw', data=data,
                                      headers={'Content-Type': 'application/json'}, method='POST')
                    self.assertRejected(request)
            for missing in ('scene_id', 'source_hash', 'changes'):
                with self.subTest(missing=missing):
                    payload = export_payload(manifest, [])
                    del payload[missing]
                    self.assertRejected(export_request(url, payload))

    def test_stale_scene_or_hash_cannot_export_another_snapshot(self):
        with running_viewer(self.game) as (_, url):
            manifest = post_waypoint(url, self.original)
            wrong_scene = export_payload(manifest, [])
            wrong_scene['scene_id'] = 'old-scene'
            self.assertRejected(export_request(url, wrong_scene), (409,))
            wrong_hash = export_payload(manifest, [])
            wrong_hash['source_hash'] = '0' * 64
            self.assertRejected(export_request(url, wrong_hash), (409,))
            post_waypoint(url, self.original, 'same-bytes-new-scene.agpw')
            self.assertRejected(export_request(url, export_payload(manifest, [])), (409,))

    def test_empty_viewer_has_no_exportable_document(self):
        with running_viewer(self.game) as (_, url):
            self.assertTrue(get_json(url + 'scene.json')['empty'])
            payload = dict(scene_id='no-scene', source_hash='0' * 64, changes=[])
            self.assertRejected(export_request(url, payload), (409,))

    def test_path_source_export_leaves_original_file_sha_unchanged(self):
        with running_path_viewer(self.game, self.source) as (_, url):
            manifest = get_json(url + 'scene.json')
            self.assertEqual(manifest['editor']['source_hash'], self.source_digest)
            exported, _ = download(url, export_payload(manifest, [dict(index=1, radius=255)]))
            expected = bytearray(self.original)
            expected[viewer.HEADER.size + viewer.RECORD.size + 16] = 255
            self.assertEqual(exported, bytes(expected))
            self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(), self.source_digest)
            self.assertEqual(get_json(url + 'scene.json')['editor']['source_hash'], self.source_digest)

    def test_get_reload_updates_path_source_hash_and_rejects_old_export(self):
        with running_path_viewer(self.game, self.source) as (_, url):
            previous = get_json(url + 'scene.json')
            external = bytearray(self.original)
            external[viewer.HEADER.size + 16] = 91
            self.source.write_bytes(external)
            refreshed = get_json(url + 'scene.json')
            self.assertNotEqual(refreshed['editor']['source_hash'], previous['editor']['source_hash'])
            self.assertEqual(refreshed['editor']['source_hash'], hashlib.sha256(external).hexdigest())
            self.assertEqual(refreshed['waypoints']['points'][0]['radius'], 91)
            self.assertRejected(export_request(url, export_payload(previous, [])), (409,))
            exported, _ = download(url, export_payload(refreshed, []))
            self.assertEqual(exported, bytes(external))
            self.assertEqual(self.source.read_bytes(), bytes(external))

    def test_arbitrary_destination_paths_and_unknown_payload_fields_are_rejected(self):
        destination = self.root / 'must-not-write.agpw'
        sentinel = b'leave this file untouched'
        destination.write_bytes(sentinel)
        with running_viewer(self.game) as (_, url):
            manifest = post_waypoint(url, self.original)
            for field in ('path', 'output_path', 'destination', 'filename', 'source_path'):
                with self.subTest(field=field):
                    payload = export_payload(manifest, [])
                    payload[field] = str(destination)
                    self.assertRejected(export_request(url, payload))
            self.assertRejected(export_request(url, export_payload(manifest, [dict(index=0, radius=10, path=str(destination))])))
            wrong_token = url.rsplit('/', 2)[0] + '/wrong-token/export.agpw'
            self.assertRejected(Request(wrong_token, data=b'{}', method='POST'), (404,))
        self.assertEqual(destination.read_bytes(), sentinel)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_editor_module_is_served_from_static_whitelist(self):
        with running_viewer(self.game) as (_, url):
            with urlopen(url + 'waypoint_editor.js', timeout=5) as response:
                self.assertIn('javascript', response.headers.get('Content-Type', ''))
                self.assertTrue(response.read())
            with self.assertRaises(HTTPError) as raised:
                urlopen(url + '../original.agpw', timeout=5)
            self.assertEqual(raised.exception.code, 404)
            raised.exception.close()


if __name__ == '__main__':
    unittest.main()
