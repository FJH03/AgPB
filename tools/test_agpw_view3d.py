"""Self-contained binary/resource and read-only HTTP checks; no game install needed."""
from contextlib import contextmanager
import hashlib
from io import BytesIO, StringIO
import json
import lzma
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import Request, urlopen
import zipfile
import zlib

import agpw_view3d as viewer
from agpw3d_bsp import load_bsp
from agpw3d_models import ResourceStore, _VPK, _vertices


def waypoint_bytes(origin=(1, 2, 3), target=-1, flags=0, map_name='test_map'):
    if isinstance(map_name, str):
        map_name = map_name.encode('utf-8')
    header = viewer.HEADER.pack(0x50424741, 1, 1, map_name, b'unit test')
    return header + viewer.RECORD.pack(*origin, flags, 64, 0,
                                      target, *([-1] * 7), *([0] * 8), 1.0)


def make_vpk(folder, *, archive=0x7FFF, corrupt_crc=False,
             name='test', payload=b'geometry bytes'):
    preload = b'prefix:'
    result = preload + payload
    crc = zlib.crc32(result) & 0xFFFFFFFF
    if corrupt_crc:
        crc ^= 1
    entry = struct.pack('<IHHIIH', crc, len(preload), archive, 0, len(payload), 0xFFFF)
    tree = b'mdl\0models\0chair\0' + entry + preload + b'\0\0\0'
    path = folder / (name + '_dir.vpk')
    path.write_bytes(struct.pack('<III', 0x55AA1234, 1, len(tree)) + tree +
                     (payload if archive == 0x7FFF else b''))
    if archive != 0x7FFF:
        (folder / (name + '_%03d.vpk' % archive)).write_bytes(payload)
    return path, result


def write_gameinfo(game_dir, entries=None):
    """Declare every external fixture mount explicitly, just like the game."""
    game_dir.mkdir(parents=True, exist_ok=True)
    if entries is None:
        entries = [('Game', '|gameinfo_path|.')]
    paths = '\n'.join('            %s %s' % (json.dumps(key), json.dumps(value))
                      for key, value in entries)
    text = '"GameInfo"\n{\n    "FileSystem"\n    {\n        "SearchPaths"\n        {\n'
    text += paths + '\n        }\n    }\n}\n'
    (game_dir / 'gameinfo.txt').write_text(text, encoding='utf-8')


def make_vvd(fixup=False):
    data = bytearray(64 + (24 if fixup else 0) + 96)
    struct.pack_into('<4siii', data, 0, b'IDSV', 4, 123, 1)
    struct.pack_into('<8i', data, 16, 2, *([0] * 7))
    start = 88 if fixup else 64
    struct.pack_into('<4i', data, 48, 2 if fixup else 0, 64 if fixup else 0, start, 0)
    if fixup:
        struct.pack_into('<6i', data, 64, 0, 1, 1, 0, 0, 1)
    struct.pack_into('<3f', data, start + 16, 1, 2, 3)
    struct.pack_into('<3f', data, start + 48 + 16, 4, 5, 6)
    return data


@contextmanager
def running_viewer(game_dir):
    """Serve an initially empty viewer; imports remain transient in memory."""
    with viewer.ViewerServer(0, game_dir=game_dir) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = 'http://127.0.0.1:%d/%s/' % (server.server_address[1], server.token)
        try:
            yield server, url
        finally:
            server.shutdown()
            thread.join(timeout=2)


def get_json(url):
    with urlopen(url, timeout=5) as response:
        return json.load(response)


def post_waypoint(url, data, name='uploaded.agpw'):
    request = Request(url + 'import.agpw', data=data, method='POST',
                      headers={'Content-Type': 'application/octet-stream',
                               'X-AGPW-Name': quote(name, safe='')})
    return get_json(request)


class WaypointChecks(unittest.TestCase):
    def test_read_preserves_coordinates_unknown_flags_and_input_bytes(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path = Path(folder) / 'map.agpw'
            data = waypoint_bytes(flags=0x80000000)
            path.write_bytes(data)
            point = viewer.read_waypoints(path)['points'][0]
            self.assertEqual(point['origin'], [1, 2, 3])
            self.assertEqual(point['flags'], 0x80000000)
            self.assertEqual(path.read_bytes(), data)

    def test_parse_uploaded_waypoints_uses_bytes_without_opening_source_name(self):
        data = waypoint_bytes(origin=(9, 8, 7), map_name='upload_map')
        with patch.object(Path, 'read_bytes', side_effect=AssertionError('source name is not a file')):
            parsed = viewer.parse_waypoints(data, 'chosen.agpw')
        self.assertEqual(parsed['map_name'], 'upload_map')
        self.assertEqual(parsed['points'][0]['origin'], [9, 8, 7])
        self.assertEqual(data, waypoint_bytes(origin=(9, 8, 7), map_name='upload_map'))

    def test_truncated_and_nonfinite_files_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path = Path(folder) / 'map.agpw'
            for data in (b'AGPB', waypoint_bytes()[:-1], waypoint_bytes((float('nan'), 0, 0))):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    viewer.read_waypoints(path)

    def test_invalid_references_are_reported_without_rewriting(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path = Path(folder) / 'map.agpw'
            data = waypoint_bytes(target=5)
            path.write_bytes(data)
            parsed = viewer.read_waypoints(path)
            self.assertTrue(parsed['warnings'])
            self.assertEqual(parsed['points'][0]['links'], [[5, 0]])
            self.assertEqual(path.read_bytes(), data)

    def test_mesh_encoding_is_little_endian_and_rejects_invalid_data(self):
        values = [1.0, 2.0, 3.0] * 3
        self.assertEqual(viewer.float_bytes(values), struct.pack('<9f', *values))
        for invalid in ([1.0], [float('inf')] * 9, [1e300] * 9):
            with self.assertRaises(ValueError):
                viewer.float_bytes(invalid)



def make_bsp(*, displacement=False, compressed=False):
    face = bytearray(56)
    struct.pack_into('<ihhh', face, 4, 0, 4 if displacement else 3, 0, 0 if displacement else -1)
    vertices = (0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0) if displacement else (0, 0, 0, 1, 0, 0, 0, 1, 0)
    edges = (0, 0, 0, 1, 1, 2, 2, 3, 3, 0) if displacement else (0, 0, 0, 1, 1, 2, 2, 0)
    lumps = {
        0: b'{"classname" "worldspawn"}\0',
        2: struct.pack('<3f5i', 0, 0, 0, 0, 64, 64, 64, 64),
        3: struct.pack('<%df' % len(vertices), *vertices),
        6: struct.pack('<16fii', *([0] * 16), 0, 0),
        7: bytes(face),
        12: struct.pack('<%dH' % len(edges), *edges),
        13: struct.pack('<4i', 1, 2, 3, 4) if displacement else struct.pack('<3i', 1, 2, 3),
        14: struct.pack('<9f3i', 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1),
        43: b'brick/wall\0', 44: struct.pack('<i', 0),
    }
    if displacement:
        info = bytearray(176)
        struct.pack_into('<3i', info, 12, 0, 0, 2)
        lumps[26] = bytes(info)
        lumps[33] = struct.pack('<125f', *([0, 0, 1, 2, 0] * 25))
        lumps[48] = struct.pack('<32H', *([0] * 32))
    header = bytearray(1036)
    struct.pack_into('<4si', header, 0, b'VBSP', 19)
    payload = bytearray()
    for index, data in sorted(lumps.items()):
        raw = 0
        if compressed and index == 3:
            raw = len(data)
            stream = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[
                {'id': lzma.FILTER_LZMA1, 'dict_size': 1 << 20, 'lc': 3, 'lp': 0, 'pb': 2}])
            data = b'LZMA' + struct.pack('<II', raw, len(stream)) + bytes([93]) + struct.pack('<I', 1 << 20) + stream
        struct.pack_into('<4i', header, 8 + index * 16, len(header) + len(payload), len(data), 1 if index == 7 else 0, raw)
        payload.extend(data)
    return bytes(header + payload)


class BSPChecks(unittest.TestCase):
    def test_basic_and_source_lzma_geometry_have_same_world_coordinates(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path = Path(folder) / 'test_map.bsp'
            meshes = []
            for compressed in (False, True):
                path.write_bytes(make_bsp(compressed=compressed))
                result = load_bsp(path)
                self.assertEqual(result['stats']['triangles'], 1)
                self.assertEqual(result['props'], [])
                meshes.append(result['positions'])
            self.assertEqual(meshes[0], meshes[1])
            self.assertEqual(meshes[0], [0, 0, 0, 1, 0, 0, 0, 1, 0])

    def test_displacement_geometry_applies_vertex_elevation(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path = Path(folder) / 'test_map.bsp'
            path.write_bytes(make_bsp(displacement=True))
            result = load_bsp(path)
            self.assertEqual(result['stats']['displacement_triangles'], 32)
            self.assertEqual(result['stats']['displacement_surfaces'], 1)
            self.assertTrue(all(z == 2 for z in result['positions'][2::3]))

    def test_unknown_version_truncation_and_outside_lump_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path = Path(folder) / 'test_map.bsp'
            wrong_version = bytearray(make_bsp())
            struct.pack_into('<i', wrong_version, 4, 99)
            outside = bytearray(make_bsp())
            struct.pack_into('<i', outside, 8 + 3 * 16, len(outside) + 20)
            for data in (make_bsp()[:50], wrong_version, outside):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    load_bsp(path)


class ResourceChecks(unittest.TestCase):
    def test_vpk_embedded_and_split_payloads_include_preload(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            for archive in (0x7FFF, 3):
                path, expected = make_vpk(Path(folder), archive=archive)
                self.assertEqual(_VPK(path).read('models/chair.mdl'), expected)

    def test_vpk_crc_failure_is_not_silently_accepted(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path, _ = make_vpk(Path(folder), corrupt_crc=True)
            with self.assertRaisesRegex(ValueError, 'CRC'):
                _VPK(path).read('models/chair.mdl')

    def test_zip_resource_precedence_and_path_validation(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            root = Path(folder)
            (root / 'models').mkdir()
            (root / 'models' / 'chair.mdl').write_bytes(b'loose')
            pak = BytesIO()
            with zipfile.ZipFile(pak, 'w') as archive:
                archive.writestr('models/chair.mdl', b'embedded')
            _, packed = make_vpk(root, name='assets', payload=b'vpk chair')
            write_gameinfo(root, [('Game', '|gameinfo_path|assets.vpk'),
                                  ('Game', '|gameinfo_path|.')])
            store = ResourceStore(root, pak.getvalue())
            self.assertEqual(store.read('Models\\Chair.mdl'), b'embedded')
            self.assertEqual(ResourceStore(root).read('models/chair.mdl'), packed)
            for name in ('../outside.mdl', '/outside.mdl', 'C:/outside.mdl'):
                with self.assertRaises(ValueError):
                    store.read(name)

    def test_vvd_fixups_reorder_vertices_and_checksum_is_checked(self):
        self.assertEqual(_vertices(make_vvd(), 123), [(1, 2, 3), (4, 5, 6)])
        self.assertEqual(_vertices(make_vvd(True), 123), [(4, 5, 6), (1, 2, 3)])
        with self.assertRaisesRegex(ValueError, 'checksum'):
            _vertices(make_vvd(), 456)
        bad = make_vvd(True)
        struct.pack_into('<i', bad, 68, 999)
        with self.assertRaisesRegex(ValueError, 'fixup'):
            _vertices(bad, 123)


class SearchPathChecks(unittest.TestCase):
    def test_vpk_and_loose_priority_follows_declaration_order(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder) / 'cstrike'
            loose = game / 'loose'
            (loose / 'models').mkdir(parents=True)
            (loose / 'models/chair.mdl').write_bytes(b'loose chair')
            _, packed = make_vpk(game, name='assets', payload=b'packed chair')
            vpk_path, loose_path = '|gameinfo_path|assets.vpk', '|gameinfo_path|loose'
            for paths, expected in (((vpk_path, loose_path), packed),
                                    ((loose_path, vpk_path), b'loose chair')):
                with self.subTest(paths=paths):
                    write_gameinfo(game, [('Game', path) for path in paths])
                    self.assertEqual(ResourceStore(game).read('models/chair.mdl'), expected)

    def test_two_vpk_priority_follows_declaration_order(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder) / 'cstrike'
            game.mkdir()
            _, first = make_vpk(game, name='first', payload=b'first archive')
            _, second = make_vpk(game, name='second', payload=b'second archive')
            paths = ('|gameinfo_path|first.vpk', '|gameinfo_path|second.vpk')
            for order, expected in ((paths, first), (paths[::-1], second)):
                with self.subTest(order=order):
                    write_gameinfo(game, [('Game', path) for path in order])
                    self.assertEqual(ResourceStore(game).read('models/chair.mdl'), expected)

    def test_game_directory_is_not_injected_when_undeclared(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder) / 'cstrike'
            (game / 'models').mkdir(parents=True)
            (game / 'models/chair.mdl').write_bytes(b'not declared')
            (game / 'declared').mkdir()
            write_gameinfo(game, [('Game', '|gameinfo_path|declared')])
            with self.assertRaises(FileNotFoundError):
                ResourceStore(game).read('models/chair.mdl')

    def test_custom_wildcard_mixes_directory_and_vpk_in_alphabetical_order(self):
        for directory_name, archive_name, expect_packed in (
                ('zeta', 'alpha', True), ('alpha', 'zeta', False)):
            with self.subTest(directory=directory_name, archive=archive_name):
                with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
                    game = Path(folder) / 'cstrike'
                    custom = game / 'custom'
                    loose = custom / directory_name
                    (loose / 'models').mkdir(parents=True)
                    (loose / 'models/chair.mdl').write_bytes(b'custom directory')
                    _, packed = make_vpk(custom, name=archive_name, payload=b'custom archive')
                    write_gameinfo(game, [('Game+Mod', '|gameinfo_path|custom/*')])
                    expected = packed if expect_packed else b'custom directory'
                    self.assertEqual(ResourceStore(game).read('models/chair.mdl'), expected)

    def test_directory_index_is_preferred_when_single_vpk_also_exists(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder) / 'cstrike'
            game.mkdir()
            _, indexed = make_vpk(game, name='assets', payload=b'directory-index archive')
            single_path, _ = make_vpk(game, name='single', payload=b'single-file archive')
            single_path.rename(game / 'assets.vpk')
            write_gameinfo(game, [('Game', '|gameinfo_path|assets.vpk')])
            self.assertEqual(ResourceStore(game).read('models/chair.mdl'), indexed)

    def test_custom_wildcard_does_not_mount_split_payload_as_a_vpk(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder) / 'cstrike'
            custom = game / 'custom'
            custom.mkdir(parents=True)
            _, packed = make_vpk(custom, name='assets', archive=0, payload=b'raw chunk bytes')
            self.assertTrue((custom / 'assets_000.vpk').is_file())
            write_gameinfo(game, [('Game', '|gameinfo_path|custom/*')])
            self.assertEqual(ResourceStore(game).read('models/chair.mdl'), packed)

    def test_gameinfo_and_engine_tokens_and_base_relative_path(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            base = Path(folder)
            game = base / 'cstrike'
            locations = ((game / 'declared', 'game.mdl', b'game token'),
                         (base / 'hl2', 'shared.mdl', b'engine token'),
                         (base / 'relative', 'base.mdl', b'base relative'))
            for directory, name, payload in locations:
                (directory / 'models').mkdir(parents=True)
                (directory / 'models' / name).write_bytes(payload)
            write_gameinfo(game, [('Game+Mod+Mod_Write', '|gameinfo_path|declared'),
                                  ('Game', '|all_source_engine_paths|hl2'),
                                  ('Game', 'relative')])
            store = ResourceStore(game)
            for directory, name, payload in locations:
                self.assertEqual(store.read('models/' + name), payload)

    def test_mod_only_mount_does_not_override_game_models(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder) / 'cstrike'
            for directory, payload in (('mod_only', b'mod file'), ('game_content', b'game file')):
                mount = game / directory
                (mount / 'models').mkdir(parents=True)
                (mount / 'models/chair.mdl').write_bytes(payload)
            write_gameinfo(game, [('Mod+Mod_Write', '|gameinfo_path|mod_only'),
                                  ('Game', '|gameinfo_path|game_content')])
            self.assertEqual(ResourceStore(game).read('models/chair.mdl'), b'game file')

    def test_missing_invalid_or_empty_searchpaths_are_explicit_errors(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder)
            with self.assertRaises(ValueError):
                ResourceStore(game)
            bad_gameinfos = (
                '',
                '"GameInfo" { "FileSystem" {} }',
                '"GameInfo" { "FileSystem" { "SearchPaths" {} } }',
                '"GameInfo" { "FileSystem" { "SearchPaths" { "Platform" "|gameinfo_path|." } } }',
                '"GameInfo" { "FileSystem" { "SearchPaths" { "Mod+Mod_Write" "|gameinfo_path|." } } }',
                '"GameInfo" { "FileSystem" { "SearchPaths" { "Game" "|gameinfo_path|." } }',
            )
            for text in bad_gameinfos:
                with self.subTest(gameinfo=text):
                    (game / 'gameinfo.txt').write_text(text, encoding='utf-8')
                    with self.assertRaises(ValueError):
                        ResourceStore(game)


class SceneChecks(unittest.TestCase):
    def test_model_geometry_is_shared_and_failed_props_remain_visible(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            root = Path(folder)
            write_gameinfo(root)
            waypoint = root / 'test_map.agpw'
            waypoint.write_bytes(waypoint_bytes())
            prop = dict(model='models/chair.mdl', origin=[0, 0, 0], angles=[0, 0, 0])
            missing = dict(prop, model='models/missing.mdl')
            bsp = dict(name='test_map', version=19, positions=[0, 0, 0, 1, 0, 0, 0, 1, 0],
                       bounds={'min': [0, 0, 0], 'max': [1, 1, 0]}, props=[prop, prop, missing])
            bounds = {'min': [-1, -1, -1], 'max': [1, 1, 1]}
            def model_result(store, name):
                if 'missing' in name:
                    return dict(positions=[], bounds=bounds, warnings=['unsupported fixture model'])
                return dict(positions=bsp['positions'], bounds=bounds, warnings=[])
            with patch.object(viewer, 'load_bsp', return_value=bsp), patch.object(viewer, 'load_model', side_effect=model_result) as reader:
                scene, meshes = viewer.build_scene(root / 'map.bsp', waypoint, root)
            self.assertEqual(reader.call_count, 2)
            self.assertEqual(len(scene['models']), 1)
            self.assertEqual(len(scene['props']), 3)
            self.assertIsNone(scene['props'][2]['model_id'])
            self.assertEqual(scene['props'][2]['bounds'], bounds)
            self.assertEqual(scene['map']['stats']['missing_props'], 1)
            self.assertEqual(len(meshes), 2)
            self.assertTrue(any('unsupported fixture' in text for text in scene['warnings']))

    def test_http_only_exposes_scene_and_bundled_assets_and_reloads_waypoints(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            path = Path(folder) / 'test_map.agpw'
            path.write_bytes(waypoint_bytes())
            original = hashlib.sha256(path.read_bytes()).hexdigest()
            scene = dict(map={'name': 'test_map'}, waypoints={}, warnings=[])
            with viewer.ViewerServer(0, scene, {'world.bin': b'abc'}, path) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                base = 'http://127.0.0.1:%d/' % server.server_address[1]
                url = base + server.token + '/'
                try:
                    with urlopen(url + 'scene.json') as response:
                        loaded = json.load(response)
                    self.assertEqual(loaded['waypoints']['points'][0]['origin'], [1, 2, 3])
                    with urlopen(url + 'world.bin') as response:
                        self.assertEqual(response.read(), b'abc')
                    for forbidden in (base + 'scene.json', url + '../AGENTS.md', url + '%2e%2e/AGENTS.md'):
                        with self.assertRaises(HTTPError) as raised:
                            urlopen(forbidden)
                        self.assertEqual(raised.exception.code, 404)
                    with self.assertRaises(HTTPError) as raised:
                        urlopen(Request(url + 'scene.json', data=b'overwrite', method='POST'))
                    self.assertEqual(raised.exception.code, 501)
                    self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), original)
                    path.write_bytes(waypoint_bytes((9, 8, 7)))
                    with urlopen(url + 'scene.json') as response:
                        self.assertEqual(json.load(response)['waypoints']['points'][0]['origin'], [9, 8, 7])
                finally:
                    server.shutdown()
                    thread.join(timeout=2)


class ImportChecks(unittest.TestCase):
    def test_build_scene_accepts_uploaded_waypoint_and_bsp_bytes_without_source_files(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder)
            write_gameinfo(game)
            before = set(game.rglob('*'))
            scene, meshes = viewer.build_scene(game / 'maps/test_map.bsp', game / 'chosen.agpw', game,
                                              waypoint_data=waypoint_bytes(origin=(3, 2, 1)),
                                              bsp_data=make_bsp())
            self.assertEqual(scene['map']['name'], 'test_map')
            self.assertEqual(scene['waypoints']['points'][0]['origin'], [3, 2, 1])
            self.assertEqual(len(meshes['world.bin']), 36)
            self.assertEqual(set(game.rglob('*')), before)

    def test_viewer_starts_empty_without_selecting_a_default_map(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder)
            write_gameinfo(game)
            with running_viewer(game) as (server, url):
                self.assertTrue(get_json(url + 'scene.json')['empty'])

    def test_import_switches_map_geometry_and_uploaded_reload_uses_memory(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder)
            write_gameinfo(game)
            (game / 'maps').mkdir()
            (game / 'maps/test_map.bsp').write_bytes(make_bsp())
            (game / 'maps/new_map.bsp').write_bytes(make_bsp(displacement=True))
            before = {path.relative_to(game): path.read_bytes() for path in game.rglob('*') if path.is_file()}
            with running_viewer(game) as (server, url):
                first = post_waypoint(url, waypoint_bytes(), name='selected_first.agpw')
                self.assertEqual(first['map']['name'], 'test_map')
                self.assertEqual(first['waypoints']['path'], 'selected_first.agpw')
                self.assertEqual(first['waypoints']['source_kind'], 'upload')
                with urlopen(url + first['world']['url'], timeout=5) as response:
                    old_geometry = response.read()
                second = post_waypoint(url, waypoint_bytes((6, 5, 4), map_name='new_map'),
                                       name='selected second % waypoint.agpw')
                self.assertEqual(second['map']['name'], 'new_map')
                self.assertNotEqual(second['scene_id'], first['scene_id'])
                self.assertEqual(second['waypoints']['path'], 'selected second % waypoint.agpw')
                self.assertEqual(second['waypoints']['source_kind'], 'upload')
                self.assertEqual(second['waypoints']['points'][0]['origin'], [6, 5, 4])
                with urlopen(url + second['world']['url'], timeout=5) as response:
                    new_geometry = response.read()
                self.assertNotEqual(new_geometry, old_geometry)
                self.assertEqual(len(new_geometry), 32 * 36)
                with patch.object(viewer, 'read_waypoints', side_effect=AssertionError('uploaded path must not be read')):
                    reloaded = get_json(url + 'scene.json')
                self.assertEqual(reloaded['waypoints'], second['waypoints'])
            after = {path.relative_to(game): path.read_bytes() for path in game.rglob('*') if path.is_file()}
            self.assertEqual(after, before)

    def test_missing_map_and_invalid_upload_report_errors_without_replacing_current_scene(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder)
            write_gameinfo(game)
            (game / 'maps').mkdir()
            (game / 'maps/test_map.bsp').write_bytes(make_bsp())
            with running_viewer(game) as (server, url):
                original = post_waypoint(url, waypoint_bytes(), name='original.agpw')
                with urlopen(url + original['world']['url'], timeout=5) as response:
                    geometry = response.read()
                for data, missing_name in ((waypoint_bytes(map_name='absent_map'), 'absent_map'),
                                           (b'not an agpw file', None)):
                    with self.subTest(missing_map=missing_name):
                        with self.assertRaises(HTTPError) as raised:
                            post_waypoint(url, data, name='failed.agpw')
                        self.assertEqual(raised.exception.code, 422)
                        error = json.load(raised.exception)
                        self.assertTrue(error.get('error'))
                        if missing_name:
                            self.assertIn(missing_name, error['error'])
                        current = get_json(url + 'scene.json')
                        self.assertEqual(current['map'], original['map'])
                        self.assertEqual(current['waypoints'], original['waypoints'])
                        with urlopen(url + current['world']['url'], timeout=5) as response:
                            self.assertEqual(response.read(), geometry)

    def test_import_map_is_read_from_declared_game_searchpaths(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder)
            declared = game / 'declared'
            (game / 'maps').mkdir()
            (declared / 'maps').mkdir(parents=True)
            (game / 'maps/test_map.bsp').write_bytes(make_bsp())
            (declared / 'maps/test_map.bsp').write_bytes(make_bsp(displacement=True))
            write_gameinfo(game, [('Game', '|gameinfo_path|declared')])
            with running_viewer(game) as (server, url):
                scene = post_waypoint(url, waypoint_bytes())
                self.assertEqual(scene['map']['stats']['world_triangles'], 32)
                with urlopen(url + scene['world']['url'], timeout=5) as response:
                    self.assertEqual(len(response.read()), 32 * 36)

    def test_stats_requires_explicit_file_even_when_one_deployed_waypoint_exists(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-test-') as folder:
            game = Path(folder)
            write_gameinfo(game)
            deployed = game / 'addons/AgPB/waypoints'
            deployed.mkdir(parents=True)
            (deployed / 'cs_office.agpw').write_bytes(waypoint_bytes(map_name='cs_office'))
            (game / 'maps').mkdir()
            (game / 'maps/cs_office.bsp').write_bytes(make_bsp())
            with patch.object(viewer, 'deployment_dir', return_value=game), \
                    patch.object(viewer, 'choose_waypoint') as choose, \
                    patch.object(viewer, 'build_scene', return_value=({}, {})) as build, \
                    patch.object(viewer, 'print_stats'), patch('sys.stderr', new=StringIO()):
                try:
                    status = viewer.main(['--stats'])
                except SystemExit as error:
                    status = error.code
            self.assertNotEqual(status, 0)
            choose.assert_not_called()
            build.assert_not_called()


if __name__ == '__main__':
    unittest.main()