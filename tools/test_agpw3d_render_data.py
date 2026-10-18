"""Synthetic base-texture/geometry/cache/HTTP integration checks."""
from collections import OrderedDict
from contextlib import contextmanager
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import urlopen

import agpw_view3d as viewer
from test_agpw3d_materials import MemoryStore, make_vtf
from test_agpw_view3d import make_bsp, post_waypoint, running_viewer, waypoint_bytes, write_gameinfo


@contextmanager
def isolated_texture_cache(limit=64 * 1024 * 1024):
    with patch.object(viewer, '_TEXTURE_CACHE', OrderedDict()), \
            patch.object(viewer, '_TEXTURE_CACHE_BYTES', 0), \
            patch.object(viewer, '_TEXTURE_CACHE_LIMIT', limit):
        yield


def write_resource(game, name, data):
    target = game / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data.encode('utf-8') if isinstance(data, str) else data)


def geometry(materials, groups, *, triangles=1):
    positions = [0, 0, 0, 1, 0, 0, 0, 1, 0] * triangles
    uvs = [0.25, -1.0, 0.5, 0.75, 1.0, 0.0] * triangles
    return dict(positions=positions, uvs=uvs, materials=materials, groups=groups,
                bounds={'min': [0, 0, 0], 'max': [1, 1, 0]}, warnings=[])


def scene_geometries():
    world = geometry(['world/a', 'shared/b'],
                     [dict(start=0, count=3, material=1), dict(start=3, count=3, material=0)],
                     triangles=2)
    world.update(name='test_map', version=19,
                 props=[dict(model='models/chair.mdl', origin=[0, 0, 0], angles=[0, 0, 0])])
    model = geometry(['unused/material', 'model/c'], [dict(start=0, count=3, material=1)])
    return world, model


def shared_texture_game(game):
    write_gameinfo(game)
    for material in ('world/a', 'shared/b', 'model/c'):
        write_resource(game, 'materials/' + material + '.vmt',
                       'AnyShader { "$basetexture" "textures/shared" "$bumpmap" "ignored" }')
    write_resource(game, 'materials/textures/shared.vtf', make_vtf())


def build_fixture(game, world, model):
    with patch.object(viewer, 'load_bsp', return_value=world), \
            patch.object(viewer, 'load_model', return_value=model):
        return viewer.build_scene(game / 'maps/test_map.bsp', 'selected.agpw', game,
                                  waypoint_data=waypoint_bytes())


class RenderDataChecks(unittest.TestCase):
    def test_shared_base_texture_keeps_distinct_global_material_ids_and_uv_bytes(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-render-') as folder, isolated_texture_cache():
            game = Path(folder)
            shared_texture_game(game)
            world, model = scene_geometries()
            scene, resources = build_fixture(game, world, model)
            self.assertEqual(len(scene['materials']), 3)
            self.assertEqual(len(scene['textures']), 1)
            material_ids = {row['name']: row['id'] for row in scene['materials']}
            self.assertNotIn('unused/material', material_ids)
            self.assertEqual(len(set(material_ids.values())), 3)
            self.assertEqual([g['material'] for g in scene['world']['groups']],
                             [material_ids['shared/b'], material_ids['world/a']])
            self.assertEqual(scene['models'][0]['groups'][0]['material'], material_ids['model/c'])
            self.assertEqual({row['texture_id'] for row in scene['materials']}, {0})
            self.assertEqual(resources[scene['world']['uv_url']], struct.pack('<12f', *world['uvs']))
            self.assertEqual(resources[scene['models'][0]['uv_url']], struct.pack('<6f', *model['uvs']))
            self.assertEqual(sum(name.endswith('.png') for name in resources), 1)
            self.assertTrue(resources[scene['textures'][0]['url']].startswith(b'\x89PNG\r\n\x1a\n'))
            self.assertEqual(scene['map']['stats']['loaded_props'], 1)

    def test_missing_vmt_or_bad_vtf_keeps_world_and_model_geometry_as_gray_fallback(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-render-') as folder, isolated_texture_cache():
            game = Path(folder)
            write_gameinfo(game)
            write_resource(game, 'materials/bad/format.vmt', 'Shader { "$basetexture" "bad/texture" }')
            write_resource(game, 'materials/bad/texture.vtf', b'not a VTF image')
            world, model = scene_geometries()
            world['materials'] = ['missing/vmt', 'bad/format']
            model['materials'] = ['unused/material', 'bad/format']
            scene, resources = build_fixture(game, world, model)
            self.assertEqual(len(scene['materials']), 2)
            self.assertEqual(scene['textures'], [])
            self.assertTrue(all(row['texture_id'] is None for row in scene['materials']))
            self.assertEqual(resources[scene['world']['url']], struct.pack('<18f', *world['positions']))
            self.assertEqual(resources[scene['models'][0]['url']], struct.pack('<9f', *model['positions']))
            self.assertIn('uv_url', scene['world'])
            self.assertIn('uv_url', scene['models'][0])
            self.assertEqual(scene['map']['stats']['loaded_props'], 1)
            self.assertEqual(scene['map']['stats']['missing_models'], 0)
            self.assertNotIn('missing', scene['props'][0])
            self.assertTrue(any('Material uses gray fallback: missing/vmt' in text for text in scene['warnings']))
            self.assertTrue(any('Base texture unavailable: bad/texture' in text for text in scene['warnings']))

    def test_content_cache_isolates_same_name_in_different_packages_and_reuses_identical_bytes(self):
        name = 'materials/same/path.vtf'
        pack_a = MemoryStore({name: b'package A bytes'})
        pack_b = MemoryStore({name: b'package B bytes'})
        def decode(data, max_size=512):
            return dict(png=b'PNG:' + data + str(max_size).encode('ascii'))
        with isolated_texture_cache(), patch.object(viewer, 'decode_vtf', side_effect=decode) as decoder:
            first = viewer.cached_texture(pack_a.read(name))
            second = viewer.cached_texture(pack_b.read(name))
            same_bytes_from_other_source = viewer.cached_texture(bytes(bytearray(pack_a.read(name))))
            self.assertIs(same_bytes_from_other_source, first)
            self.assertNotEqual(first['png'], second['png'])
            self.assertEqual(decoder.call_count, 2)
            small = viewer.cached_texture(pack_a.read(name), max_size=256)
            self.assertNotEqual(small['png'], first['png'])
            self.assertEqual(decoder.call_count, 3)

    def test_texture_cache_evicts_least_recently_used_image_within_byte_limit(self):
        def decode(data, max_size=512):
            return {'png': b'image' + data}
        with isolated_texture_cache(limit=12), patch.object(viewer, 'decode_vtf', side_effect=decode) as decoder:
            first = viewer.cached_texture(b'A')
            viewer.cached_texture(b'B')
            self.assertIs(viewer.cached_texture(b'A'), first)
            viewer.cached_texture(b'C')
            self.assertIs(viewer.cached_texture(b'A'), first)
            self.assertEqual(decoder.call_count, 3)
            viewer.cached_texture(b'B')
            self.assertEqual(decoder.call_count, 4)
            self.assertLessEqual(viewer._TEXTURE_CACHE_BYTES, 12)

    def test_import_prefixes_mesh_uv_and_texture_urls_and_serves_png_mime(self):
        with tempfile.TemporaryDirectory(prefix='agpw3d-render-') as folder, isolated_texture_cache():
            game = Path(folder)
            shared_texture_game(game)
            write_resource(game, 'maps/test_map.bsp', make_bsp())
            world, model = scene_geometries()
            with patch.object(viewer, 'load_bsp', return_value=world), \
                    patch.object(viewer, 'load_model', return_value=model):
                with running_viewer(game) as (server, url):
                    scene = post_waypoint(url, waypoint_bytes())
                    prefix = 'scenes/' + scene['scene_id'] + '/'
                    for row in [scene['world'], *scene['models']]:
                        self.assertTrue(row['url'].startswith(prefix))
                        self.assertTrue(row['uv_url'].startswith(prefix))
                        with urlopen(url + row['uv_url'], timeout=5) as response:
                            self.assertEqual(response.headers.get_content_type(), 'application/octet-stream')
                            self.assertEqual(len(response.read()), row['triangles'] * 3 * 2 * 4)
                    texture = scene['textures'][0]
                    self.assertTrue(texture['url'].startswith(prefix))
                    with urlopen(url + texture['url'], timeout=5) as response:
                        self.assertEqual(response.headers.get_content_type(), 'image/png')
                        self.assertTrue(response.read().startswith(b'\x89PNG\r\n\x1a\n'))
                    with self.assertRaises(HTTPError) as raised:
                        urlopen(url + 'textures/0.png', timeout=5)
                    self.assertEqual(raised.exception.code, 404)


if __name__ == '__main__':
    unittest.main()
