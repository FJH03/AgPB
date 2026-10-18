#!/usr/bin/env python3
"""Standalone BSP + editable .agpw tool. Python standard library only.

Run this file or agpw_view3d.bat; the browser is the local 3D display.
Map/model resources stay read-only. AGPW edits are exported for explicit saving.
"""
from __future__ import annotations

import argparse
from array import array
from collections import OrderedDict
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import re
import secrets
import struct
import sys
import threading
from urllib.parse import quote, unquote, urlsplit
import webbrowser

sys.dont_write_bytecode = True

from agpw3d_bsp import load_bsp
from agpw3d_models import ResourceStore, load_model
from agpw3d_materials import resolve_basetexture, decode_vtf
from agpw3d_document import AgpwDocument, DocumentConflict

ROOT = Path(__file__).resolve().parent.parent
WEB_ROOT = Path(__file__).resolve().parent / 'agpw3d_web'
HEADER = struct.Struct('<III32s32s')
RECORD = struct.Struct('<3fIBB8h8Hf')
_TEXTURE_CACHE = OrderedDict()
_TEXTURE_CACHE_BYTES = 0
_TEXTURE_CACHE_LIMIT = 64 * 1024 * 1024
_TEXTURE_CACHE_LOCK = threading.Lock()


def deployment_dir():
    """Use the same AGENTS.md destination as the independent 2D tool."""
    requirements = ROOT / 'AGENTS.md'
    if requirements.is_file():
        match = re.search(r'部署目标[：:]\s*`([^`]+)`', requirements.read_text(encoding='utf-8'))
        if match:
            return Path(match.group(1).strip())
    return None


def read_waypoints(path):
    path = Path(path).resolve()
    return parse_waypoints(path.read_bytes(), str(path))


def parse_waypoints(data, source_name):
    """Parse selected file bytes without creating or reading a temporary file."""
    if len(data) < HEADER.size:
        raise ValueError('waypoint header is truncated')
    magic, version, count, map_bytes, author = HEADER.unpack_from(data)
    if magic != 0x50424741 or version != 1:
        raise ValueError('unsupported waypoint magic/version (expected AGPB v1)')
    if count > 8192 or len(data) < HEADER.size + count * RECORD.size:
        raise ValueError('invalid waypoint count or truncated records')
    warnings = []
    points = []
    for index in range(count):
        row = RECORD.unpack_from(data, HEADER.size + index * RECORD.size)
        if not all(math.isfinite(value) for value in (*row[:3], row[-1])):
            raise ValueError('non-finite coordinate/gravity at waypoint #%d' % index)
        links = [[target, flags] for target, flags in zip(row[6:14], row[14:22]) if target >= 0]
        if any(target >= count or target == index for target, flags in links):
            warnings.append('Waypoint #%d contains an invalid or self link.' % index)
        if len({target for target, flags in links}) != len(links):
            warnings.append('Waypoint #%d contains duplicate outgoing links.' % index)
        points.append(dict(origin=list(row[:3]), flags=row[3], radius=row[4],
                           mesh=row[5], gravity=row[-1], links=links))
    if len(data) > HEADER.size + count * RECORD.size:
        warnings.append('Waypoint file has trailing data; it is left untouched.')
    return dict(path=str(source_name),
                map_name=map_bytes.split(b'\0', 1)[0].decode('utf-8', 'replace'),
                author=author.split(b'\0', 1)[0].decode('utf-8', 'replace'),
                points=points, warnings=warnings)


def float_bytes(values):
    if len(values) % 9:
        raise ValueError('triangle coordinate array is not divisible by 9')
    return attribute_bytes(values)


def attribute_bytes(values):
    if not all(math.isfinite(v) for v in values):
        raise ValueError('mesh contains non-finite coordinates')
    try:
        packed = array('f', values)
    except OverflowError as exc:
        raise ValueError('mesh coordinates exceed the 32-bit preview range') from exc
    if not all(math.isfinite(value) for value in packed):
        raise ValueError('mesh coordinates exceed the 32-bit preview range')
    if sys.byteorder != 'little':
        packed.byteswap()
    return packed.tobytes()


def cached_texture(data, max_size=512):
    """Cache decoded pixels by content, never by path across maps/game mounts."""
    global _TEXTURE_CACHE_BYTES
    key = (hashlib.sha256(data).digest(), max_size)
    with _TEXTURE_CACHE_LOCK:
        if key in _TEXTURE_CACHE:
            _TEXTURE_CACHE.move_to_end(key)
            return _TEXTURE_CACHE[key]
    result = decode_vtf(data, max_size=max_size)
    size = len(result['png'])
    if size <= _TEXTURE_CACHE_LIMIT:
        with _TEXTURE_CACHE_LOCK:
            if key in _TEXTURE_CACHE:
                return _TEXTURE_CACHE[key]
            while _TEXTURE_CACHE and _TEXTURE_CACHE_BYTES + size > _TEXTURE_CACHE_LIMIT:
                _, removed = _TEXTURE_CACHE.popitem(last=False)
                _TEXTURE_CACHE_BYTES -= len(removed['png'])
            _TEXTURE_CACHE[key] = result
            _TEXTURE_CACHE_BYTES += size
    return result


class BaseTextures:
    """Only resolve $basetexture; all appearance/shader parameters are ignored."""

    def __init__(self, store, resources, warnings, report):
        self.store, self.resources = store, resources
        self.warnings, self.report = warnings, report
        self.materials, self.textures = [], []
        self.material_ids, self.texture_ids = {}, {}

    def material(self, name):
        name = re.sub('/+', '/', str(name).replace('\\', '/')).strip('/').lower()
        if name.startswith('materials/'):
            name = name[len('materials/'):]
        if name.endswith('.vmt'):
            name = name[:-4]
        if name in self.material_ids:
            return self.material_ids[name]
        material_id = len(self.materials)
        self.material_ids[name] = material_id
        row = dict(id=material_id, name=name, texture_id=None)
        self.materials.append(row)
        if not name:
            return material_id
        try:
            texture_name = resolve_basetexture(self.store, name)
            if not texture_name:
                raise ValueError('no $basetexture')
            if texture_name not in self.texture_ids:
                self.texture_ids[texture_name] = None
                try:
                    self.report('Reading base texture %d: %s' % (len(self.texture_ids), texture_name))
                    image = cached_texture(self.store.read('materials/' + texture_name + '.vtf'))
                    texture_id = len(self.textures)
                    url = 'textures/%d.png' % texture_id
                    self.resources[url] = image['png']
                    self.textures.append(dict(id=texture_id, name=texture_name, url=url,
                                              width=image['width'], height=image['height'],
                                              source_width=image['source_width'], source_height=image['source_height']))
                    self.texture_ids[texture_name] = texture_id
                except (ValueError, OSError, struct.error, NotImplementedError) as exc:
                    self.warnings.append('Base texture unavailable: %s (%s)' % (texture_name, exc))
            row['texture_id'] = self.texture_ids[texture_name]
        except (ValueError, OSError, struct.error, NotImplementedError) as exc:
            self.warnings.append('Material uses gray fallback: %s (%s)' % (name, exc))
        return material_id

    def attach(self, row, geometry):
        # Old scenes and intentionally geometry-only fixtures remain supported.
        if 'uvs' not in geometry:
            return
        try:
            vertex_count = len(geometry['positions']) // 3
            uvs, groups, names = geometry['uvs'], geometry['groups'], geometry['materials']
            if len(uvs) != vertex_count * 2:
                raise ValueError('UV count does not match geometry vertices')
            packed_uvs = attribute_bytes(uvs)
            cursor = 0
            for group in groups:
                start, count, material = group['start'], group['count'], group['material']
                if (not all(isinstance(v, int) for v in (start, count, material))
                        or start != cursor or count <= 0 or count % 3 or start + count > vertex_count
                        or material < 0 or material >= len(names)):
                    raise ValueError('invalid material group coverage or index')
                cursor += count
            if cursor != vertex_count:
                raise ValueError('material groups do not cover the geometry')
        except (ValueError, KeyError, TypeError) as exc:
            self.warnings.append('Geometry uses gray fallback: %s (%s)' % (row['url'], exc))
            return
        converted = [dict(start=g['start'], count=g['count'], material=self.material(names[g['material']]))
                     for g in groups]
        uv_url = row['url'].removesuffix('.bin') + '.uv.bin'
        self.resources[uv_url] = packed_uvs
        row.update(uv_url=uv_url, groups=converted)


class ModelUnavailable(ValueError):
    def __init__(self, message, bounds):
        super().__init__(message)
        self.bounds = bounds


def build_scene(bsp_path, waypoint_path, game_dir, progress=None, *, waypoint_data=None, bsp_data=None):
    report = progress or (lambda message: None)
    report('Reading BSP geometry: %s' % Path(bsp_path).name)
    bsp = load_bsp(bsp_path, data=bsp_data) if bsp_data is not None else load_bsp(bsp_path)
    warnings = list(bsp.get('warnings', []))
    resources = {'world.bin': float_bytes(bsp['positions'])}
    model_rows, instances = [], []
    # Shared model geometry is sent once even when a map has many instances.
    model_cache = {}
    store = ResourceStore(Path(game_dir), bsp.get('pak_bytes') or None)
    appearance = BaseTextures(store, resources, warnings, report)
    world = dict(url='world.bin', triangles=len(resources['world.bin']) // 36)
    appearance.attach(world, bsp)
    for prop in bsp.get('props', []):
        name = prop.get('model', '').replace('\\', '/').lower()
        if name not in model_cache:
            report('Reading model %d: %s' % (len(model_cache) + 1, name))
            try:
                model = load_model(store, name)
                binary = float_bytes(model['positions'])
                if not binary:
                    reason = '; '.join(model.get('warnings', [])) or 'model has no renderable triangles'
                    model_cache[name] = (None, model.get('bounds'), reason)
                    warnings.append('Model unavailable: %s (%s)' % (name, reason))
                    raise ModelUnavailable(reason, model.get('bounds'))
                model_id = len(model_rows)
                url = 'models/%d.bin' % model_id
                resources[url] = binary
                row = dict(id=model_id, model=name, url=url,
                           triangles=len(binary) // 36, bounds=model['bounds'])
                appearance.attach(row, model)
                model_rows.append(row)
                model_cache[name] = (model_id, model['bounds'], None)
                for warning in model.get('warnings', []):
                    warnings.append('%s: %s' % (name, warning))
            except ModelUnavailable as exc:
                model_cache[name] = (None, exc.bounds, str(exc))
            except (ValueError, OSError, struct.error, NotImplementedError) as exc:
                reason = str(exc) or type(exc).__name__
                model_cache[name] = (None, None, reason)
                warnings.append('Model unavailable: %s (%s)' % (name, reason))
        model_id, bounds, reason = model_cache[name]
        instance = dict(model_id=model_id, model=name,
                        origin=list(prop['origin']), angles=list(prop.get('angles', [0, 0, 0])),
                        scale=prop.get('scale', 1.0), kind=prop.get('kind', 'static'))
        if bounds:
            instance['bounds'] = bounds
        if reason:
            instance['missing'] = reason
        instances.append(instance)
    warnings.extend(getattr(store, 'warnings', []))
    wp = (parse_waypoints(waypoint_data, str(waypoint_path)) if waypoint_data is not None
          else read_waypoints(waypoint_path))
    stats = dict(bsp.get('stats', {}))
    stats.update(world_triangles=len(resources['world.bin']) // 36,
                 model_count=len(model_rows), model_triangles=sum(row['triangles'] for row in model_rows),
                 prop_count=len(instances), loaded_props=sum(p['model_id'] is not None for p in instances),
                 missing_props=sum(p['model_id'] is None for p in instances),
                 missing_models=sum(value[0] is None for value in model_cache.values()),
                 material_count=len(appearance.materials), texture_count=len(appearance.textures),
                 missing_materials=sum(row['texture_id'] is None for row in appearance.materials))
    manifest = dict(map=dict(name=bsp.get('name', Path(bsp_path).stem), version=bsp['version'],
                            bounds=bsp['bounds'], play_bounds=bsp.get('play_bounds'),
                            stats=stats, warnings=bsp.get('warnings', [])),
                    world=world, models=model_rows, props=instances, waypoints=wp,
                    materials=appearance.materials, textures=appearance.textures,
                    warnings=list(dict.fromkeys(warnings)))
    manifest['scene_id'] = secrets.token_hex(12)
    refresh_waypoints(manifest, waypoint_path, waypoint_data)
    return manifest, resources


def refresh_waypoints(manifest, waypoint_path, waypoint_data=None):
    # Retain the exact source bytes, not a reconstruction of the display model.
    raw = bytes(waypoint_data) if waypoint_data is not None else Path(waypoint_path).read_bytes()
    name = str(waypoint_path) if waypoint_data is not None else str(Path(waypoint_path).resolve())
    document = AgpwDocument(raw, name)
    wp = parse_waypoints(raw, name)
    wp['source_kind'] = 'upload' if waypoint_data is not None else 'path'
    warnings = list(wp.pop('warnings', []))
    if wp['map_name'].lower() != manifest['map']['name'].lower():
        warnings.append('Map name mismatch: BSP=%s, waypoints=%s.' %
                        (manifest['map']['name'], wp['map_name']))
    manifest['waypoints'] = wp
    manifest['waypoint_warnings'] = warnings
    manifest['editor'] = dict(document.schema(), source_hash=document.source_hash)
    return document


def print_stats(manifest, resources):
    stats = manifest['map']['stats']
    points = manifest['waypoints']['points']
    directed = sum(len(point['links']) for point in points)
    print('map          : %s (BSP v%d)' % (manifest['map']['name'], manifest['map']['version']))
    print('waypoints    : %d, directed links: %d' % (len(points), directed))
    print('world        : %d triangles' % stats['world_triangles'])
    print('models       : %d unique, %d triangles' % (stats['model_count'], stats['model_triangles']))
    print('props        : %d loaded / %d total; %d missing models' %
          (stats['loaded_props'], stats['prop_count'], stats['missing_models']))
    print('textures     : %d base textures, %d materials (%d gray fallbacks)' %
          (stats.get('texture_count', 0), stats.get('material_count', 0), stats.get('missing_materials', 0)))
    print('scene payload: %.2f MiB' % (sum(map(len, resources.values())) / (1024 * 1024)))
    for warning in manifest['warnings'] + manifest.get('waypoint_warnings', []):
        print('warning      : %s' % warning)


def map_resource_name(map_name):
    if (not map_name or map_name in ('.', '..') or Path(map_name).name != map_name
            or any(char in map_name for char in '/\\:')):
        raise ValueError('Invalid map name in .agpw header: %r' % map_name)
    return 'maps/' + map_name + '.bsp'


def matching_bsp(game_dir, map_name):
    resource = map_resource_name(map_name)
    try:
        data = ResourceStore(Path(game_dir)).read(resource)
    except FileNotFoundError as exc:
        raise ValueError('Matching BSP not found: %s (gameinfo.txt GAME SearchPaths)' % resource) from exc
    return Path(game_dir) / resource, data


class ViewerServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port, manifest=None, resources=None, waypoint_path=None, game_dir=None):
        self.manifest = manifest
        self.resources = resources or {}
        self.waypoint_path = waypoint_path
        self.waypoint_data = None
        self.waypoint_name = None
        self.game_dir = Path(game_dir) if game_dir is not None else deployment_dir()
        self.token = secrets.token_urlsafe(24)
        self.scene_lock = threading.Lock()
        self.import_lock = threading.Lock()
        self.document = None
        if manifest is not None and waypoint_path is not None:
            self.document = refresh_waypoints(manifest, waypoint_path)
            manifest.setdefault('scene_id', secrets.token_hex(12))
        super().__init__(('127.0.0.1', port), ViewerHandler)

    def import_waypoints(self, data, name):
        if self.game_dir is None:
            raise ValueError('Game directory is unknown; restart with --game-dir')
        with self.import_lock:
            parsed = parse_waypoints(data, name)
            bsp_path, bsp_data = matching_bsp(self.game_dir, parsed['map_name'])
            manifest, resources = build_scene(bsp_path, name, self.game_dir,
                                             waypoint_data=data, bsp_data=bsp_data)
            prefix = 'scenes/' + manifest['scene_id'] + '/'
            # Every import owns fresh mesh URLs, including same-map reimports.
            for row in [manifest['world'], *manifest['models']]:
                for key in ('url', 'uv_url'):
                    if key in row:
                        row[key] = prefix + row[key]
            for row in manifest.get('textures', []):
                row['url'] = prefix + row['url']
            resources = {prefix + key: value for key, value in resources.items()}
            document = AgpwDocument(data, name)
            manifest['editor'] = dict(document.schema(), source_hash=document.source_hash)
            with self.scene_lock:
                self.manifest = manifest
                self.resources = resources
                self.waypoint_path = None
                self.waypoint_name = name
                self.waypoint_data = data
                self.document = document
            return manifest


    def export_waypoints(self, payload):
        """Produce an edited copy; this endpoint never accepts filesystem paths."""
        required = {'scene_id', 'source_hash', 'changes'}
        if not isinstance(payload, dict) or set(payload) != required:
            raise ValueError('Export requires only scene_id, source_hash and changes')
        if (not isinstance(payload['scene_id'], str) or not payload['scene_id']
                or not isinstance(payload['source_hash'], str)
                or not re.fullmatch('[0-9a-f]{64}', payload['source_hash'])):
            raise ValueError('Invalid export scene ID or source hash')
        with self.scene_lock:
            if self.manifest is None or self.document is None:
                raise DocumentConflict('No editable AGPW document is open')
            if payload['scene_id'] != self.manifest.get('scene_id'):
                raise DocumentConflict('The open scene changed; reload before exporting')
            data = self.document.export_changes(payload['changes'], payload['source_hash'])
            name = self.document.source_name.replace('\\', '/').rsplit('/', 1)[-1]
            if not name.lower().endswith('.agpw'):
                name = 'waypoints.agpw'
            return data, name


class ViewerHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def route(self):
        request_path = unquote(urlsplit(self.path).path)
        prefix = '/' + self.server.token + '/'
        return request_path[len(prefix):] if request_path.startswith(prefix) else None

    def respond(self, data, mime, status=200, headers=None):
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def json_response(self, value, status=200):
        data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode('utf-8')
        self.respond(data, 'application/json; charset=utf-8', status)

    def do_GET(self):
        name = self.route()
        if name is None:
            self.send_error(404)
            return
        name = name or 'index.html'
        try:
            if name == 'scene.json':
                with self.server.scene_lock:
                    if self.server.manifest is None:
                        value = dict(empty=True, game_dir=str(self.server.game_dir or ''))
                    else:
                        self.server.document = refresh_waypoints(self.server.manifest,
                                          self.server.waypoint_path or self.server.waypoint_name,
                                          self.server.waypoint_data)
                        value = self.server.manifest
                    self.json_response(value)
                return
            with self.server.scene_lock:
                data = self.server.resources.get(name)
            if data is not None:
                self.respond(data, 'image/png' if name.endswith('.png') else 'application/octet-stream')
            elif name in {'index.html', 'viewer.js', 'viewer.css', 'waypoint_editor.js',
                          'vendor/three.module.js', 'vendor/OrbitControls.js'}:
                data = (WEB_ROOT / name).read_bytes()
                mime = ('text/html; charset=utf-8' if name.endswith('.html') else
                        'text/css; charset=utf-8' if name.endswith('.css') else
                        'text/javascript; charset=utf-8')
                self.respond(data, mime)
            else:
                self.send_error(404)
        except (ValueError, OSError, struct.error, NotImplementedError) as exc:
            self.json_response({'error': str(exc)}, 422)

    def do_POST(self):
        route = self.route()
        if route is None:
            self.send_error(404)
            return
        if route not in {'import.agpw', 'export.agpw'}:
            self.send_error(501, 'Only waypoint import and export are supported')
            return
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if length <= 0:
                raise ValueError('No .agpw file content was received')
            if length > 2 * 1024 * 1024:
                self.json_response({'error': '.agpw file exceeds the 2 MiB preview limit'}, 413)
                return
            if route == 'export.agpw':
                content_type = self.headers.get('Content-Type', '').split(';', 1)[0].strip().lower()
                if content_type != 'application/json':
                    raise ValueError('Export requires application/json')
                self.connection.settimeout(30)
                body = self.rfile.read(length)
                if len(body) != length:
                    raise ValueError('Incomplete export request')
                payload = json.loads(body.decode('utf-8'))
                exported, filename = self.server.export_waypoints(payload)
                disposition = "attachment; filename=\"waypoints.agpw\"; filename*=UTF-8''" + quote(filename, safe='')
                self.respond(exported, 'application/octet-stream', headers={'Content-Disposition': disposition})
                return
            name = unquote(self.headers.get('X-AGPW-Name', 'imported.agpw'))
            name = name.replace('\\', '/').rsplit('/', 1)[-1]
            if not name.lower().endswith('.agpw'):
                raise ValueError('Choose an .agpw waypoint file')
            self.connection.settimeout(30)
            data = self.rfile.read(length)
            if len(data) != length:
                raise ValueError('Incomplete .agpw file upload')
            manifest = self.server.import_waypoints(data, name)
            self.json_response(manifest)
        except DocumentConflict as exc:
            self.json_response({'error': str(exc)}, 409)
        except (ValueError, OSError, struct.error, NotImplementedError) as exc:
            self.json_response({'error': str(exc)}, 422)


def choose_waypoint(initial=None):
    import tkinter as tk
    from tkinter import filedialog
    root = tk.Tk()
    root.withdraw()
    try:
        value = filedialog.askopenfilename(title='选择用于 3D 预览的 .agpw 路点文件',
                                          initialdir=str(initial) if initial else None,
                                          filetypes=[('AgPB waypoints', '*.agpw')])
    finally:
        root.destroy()
    return Path(value) if value else None


def resolve_inputs(args):
    deploy = deployment_dir()
    game_dir = Path(args.game_dir).resolve() if args.game_dir else deploy
    waypoint = Path(args.file).resolve() if args.file else None
    if waypoint is None and args.stats:
        raise ValueError('Pass an .agpw file explicitly with --stats')
    bsp = Path(args.bsp).resolve() if args.bsp else None
    if args.game_dir is None and bsp and bsp.parent.name.lower() == 'maps':
        game_dir = bsp.parent.parent
    if game_dir is None:
        if waypoint and waypoint.parent.name.lower() == 'waypoints' and len(waypoint.parents) >= 4:
            game_dir = waypoint.parents[3]
        else:
            raise ValueError('Game directory is unknown; pass --game-dir <game folder>')
    if not game_dir.is_dir():
        raise ValueError('Game directory not found: %s' % game_dir)
    if waypoint is not None:
        wp = read_waypoints(waypoint)
        if bsp is None:
            # Resolution and missing-map errors use GAME SearchPaths in main().
            bsp = game_dir / map_resource_name(wp['map_name'])
        elif not bsp.is_file():
            raise ValueError('Map not found: %s' % bsp)
    return bsp, waypoint, game_dir


def main(argv=None):
    parser = argparse.ArgumentParser(description='Standalone 3D BSP + .agpw waypoint editor')
    parser.add_argument('file', nargs='?', help='.agpw waypoint file')
    parser.add_argument('--bsp', help='explicit BSP map file')
    parser.add_argument('--game-dir', help='game content folder (for maps and model resources)')
    parser.add_argument('--stats', action='store_true', help='load and validate geometry, print stats, exit')
    parser.add_argument('--json', action='store_true', help='print the scene manifest with --stats')
    parser.add_argument('--no-browser', action='store_true', help='print the local URL without opening a browser')
    parser.add_argument('--port', type=int, default=0, help='local port (default: choose a free port)')
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535:
        parser.error('--port must be between 0 and 65535')
    try:
        paths = resolve_inputs(args)
        if paths is None:
            return 0
        bsp, waypoint, game_dir = paths
        manifest, resources = None, {}
        if waypoint is not None:
            progress = None if args.stats else (lambda message: print(message, flush=True))
            bsp_data = None
            if not args.bsp:
                bsp, bsp_data = matching_bsp(game_dir, read_waypoints(waypoint)['map_name'])
            manifest, resources = build_scene(bsp, waypoint, game_dir, progress, bsp_data=bsp_data)
        if args.stats:
            if args.json:
                print(json.dumps(manifest, ensure_ascii=True, allow_nan=False))
            else:
                print_stats(manifest, resources)
            return 0
        if manifest is not None:
            print_stats(manifest, resources)
        else:
            print('Choose Import AGPW in the viewer to load a waypoint file.', flush=True)
        with ViewerServer(args.port, manifest, resources, waypoint, game_dir=game_dir) as server:
            url = 'http://127.0.0.1:%d/%s/' % (server.server_address[1], server.token)
            print('Viewer URL: %s' % url, flush=True)
            print('Waypoint editor. Keep this console open; press Ctrl+C to stop.', flush=True)
            if not args.no_browser:
                webbrowser.open(url, new=2)
            try:
                server.serve_forever(poll_interval=0.3)
            except KeyboardInterrupt:
                print('\nViewer stopped.')
        return 0
    except (OSError, ValueError, struct.error, NotImplementedError) as exc:
        print('Viewer error: %s' % exc, file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())