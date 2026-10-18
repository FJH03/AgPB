"""Read-only Source model geometry and game resources for the standalone viewer.

The binary layouts follow the SDK's public/studio.h, public/optimize.h and
utils/vrad/vradstaticprops.cpp. No engine DLLs, texture decoding or native AI are
used. Static props use model-space VVD positions, as the SDK's VRAD does; BSP
instance origin/angles are applied by the caller. Other models show bind pose.
"""
from __future__ import annotations

import io
import math
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile
import zlib


_MAX_RESOURCE = 256 * 1024 * 1024


def _resource_name(name: str) -> str:
    name = name.replace("\\", "/").lower()
    parts = PurePosixPath(name).parts
    if not name or name.startswith("/") or ":" in name or ".." in parts:
        raise ValueError("Resource path must be relative to the game directory")
    return "/".join(part for part in parts if part != ".")


def _span(data: bytes, offset: int, size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"Truncated or invalid {label} at byte {offset}")


def _unpack(fmt: str, data: bytes, offset: int, label: str):
    _span(data, offset, struct.calcsize(fmt), label)
    return struct.unpack_from(fmt, data, offset)


def _count(value: int, maximum: int, label: str) -> int:
    if not 0 <= value <= maximum:
        raise ValueError(f"Invalid {label}: {value}")
    return value


class _VPK:
    """VPK version 1/2 directory; payloads remain on disk until requested."""

    def __init__(self, path: Path):
        self.path = path
        self.entries = None
        self.data_offset = 0
        self.embedded_size = None

    def _load(self) -> None:
        with self.path.open("rb") as source:
            header = source.read(12)
            signature, version, tree_size = _unpack("<III", header, 0, "VPK header")
            if signature != 0x55AA1234 or version not in (1, 2):
                raise ValueError(f"Unsupported VPK header: {self.path.name}")
            if tree_size > _MAX_RESOURCE:
                raise ValueError(f"VPK directory too large: {self.path.name}")
            header_size = 12
            if version == 2:
                extra = source.read(16)
                self.embedded_size = _unpack("<IIII", extra, 0, "VPK v2 header")[0]
                header_size = 28
            tree = source.read(tree_size)
            _span(tree, 0, tree_size, "VPK directory")
        self.data_offset = header_size + tree_size
        cursor = 0
        entries = {}

        def string() -> str:
            nonlocal cursor
            end = tree.find(b"\0", cursor)
            if end < 0:
                raise ValueError(f"Unterminated VPK string: {self.path.name}")
            value = tree[cursor:end].decode("utf-8", errors="replace")
            cursor = end + 1
            return value

        while True:
            extension = string()
            if not extension:
                break
            while True:
                directory = string()
                if not directory:
                    break
                while True:
                    filename = string()
                    if not filename:
                        break
                    crc, preload_size, archive, offset, length, terminator = _unpack(
                        "<IHHIIH", tree, cursor, "VPK entry"
                    )
                    cursor += 18
                    if terminator != 0xFFFF:
                        raise ValueError(f"Invalid VPK entry terminator: {self.path.name}")
                    _span(tree, cursor, preload_size, "VPK preload")
                    preload = tree[cursor:cursor + preload_size]
                    cursor += preload_size
                    relative = ("" if directory == " " else directory + "/") + filename
                    if extension != " ":
                        relative += "." + extension
                    key = _resource_name(relative)
                    entries[key] = (crc, archive, offset, length, preload)
        self.entries = entries

    def read(self, name: str) -> bytes | None:
        if self.entries is None:
            self._load()
        entry = self.entries.get(name)
        if entry is None:
            return None
        crc, archive, offset, length, preload = entry
        if length + len(preload) > _MAX_RESOURCE:
            raise ValueError(f"Resource too large: {name}")
        if archive == 0x7FFF:
            payload_path = self.path
            if self.embedded_size is not None and offset + length > self.embedded_size:
                raise ValueError(f"VPK embedded entry outside data section: {name}")
            payload_offset = self.data_offset + offset
        else:
            stem = self.path.stem
            if stem.endswith("_dir"):
                stem = stem[:-4]
            payload_path = self.path.with_name(f"{stem}_{archive:03d}.vpk")
            payload_offset = offset
        with payload_path.open("rb") as source:
            source.seek(payload_offset)
            payload = source.read(length)
        if len(payload) != length:
            raise ValueError(f"Truncated VPK resource: {name}")
        result = preload + payload
        if zlib.crc32(result) & 0xFFFFFFFF != crc:
            raise ValueError(f"VPK resource CRC mismatch: {name}")
        return result


def _game_search_values(source: str) -> list[str]:
    """Parse the ordered GAME entries inside GameInfo/FileSystem/SearchPaths."""
    token_re = re.compile(r'\s+|//[^\r\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+')
    tokens = []
    cursor = 0
    while cursor < len(source):
        match = token_re.match(source, cursor)
        if not match:
            raise ValueError('Malformed gameinfo.txt token at byte %d' % cursor)
        token = match.group()
        cursor = match.end()
        if token.isspace() or token.startswith(('//', '/*')):
            continue
        if token.startswith('"'):
            token = re.sub(r'\\(["\\])', r'\1', token[1:-1])
        tokens.append(token)
    cursor = 0

    def parse_block(nested=False):
        nonlocal cursor
        pairs = []
        while cursor < len(tokens):
            key = tokens[cursor]
            cursor += 1
            if key == '}':
                if nested:
                    return pairs
                raise ValueError('Unexpected closing brace in gameinfo.txt')
            if key == '{' or cursor >= len(tokens):
                raise ValueError('Missing key/value in gameinfo.txt')
            if key.startswith('#'):
                raise ValueError('Unsupported gameinfo.txt directive: %s' % key)
            value = tokens[cursor]
            cursor += 1
            if value == '{':
                value = parse_block(True)
            elif value == '}':
                raise ValueError('Missing value in gameinfo.txt')
            pairs.append((key, value))
        if nested:
            raise ValueError('Missing closing brace in gameinfo.txt')
        return pairs

    def object_value(pairs, name):
        matches = [value for key, value in pairs if key.lower() == name.lower()]
        if len(matches) != 1 or not isinstance(matches[0], list):
            raise ValueError('gameinfo.txt must contain one %s block' % name)
        return matches[0]

    root = object_value(parse_block(), 'GameInfo')
    filesystem = object_value(root, 'FileSystem')
    searchpaths = object_value(filesystem, 'SearchPaths')
    values = []
    for key, value in searchpaths:
        # MDLCache loads MDL/VVD/VTX through GAME, not the independent MOD ID.
        if 'game' not in {part.lower() for part in key.split('+')}:
            continue
        if not isinstance(value, str) or not value.strip():
            raise ValueError('Invalid GAME SearchPaths entry in gameinfo.txt')
        values.append(value)
    if not values:
        raise ValueError('gameinfo.txt has no GAME SearchPaths entries')
    return values


def _search_paths(game_dir: Path) -> list[Path]:
    """Expand configured GAME paths in place, keeping folders and VPKs mixed."""
    import glob

    gameinfo = game_dir / 'gameinfo.txt'
    if not gameinfo.is_file():
        raise ValueError('gameinfo.txt not found: %s' % gameinfo)
    source = gameinfo.read_text(encoding='utf-8-sig')
    result = []
    for value in _game_search_values(source):
        normalized = value.replace('\\', '/')
        base = game_dir.parent
        if normalized.lower().startswith('|gameinfo_path|'):
            normalized = normalized[len('|gameinfo_path|'):]
            base = game_dir
        elif normalized.lower().startswith('|all_source_engine_paths|'):
            normalized = normalized[len('|all_source_engine_paths|'):]
        if '|' in normalized:
            raise ValueError('Unsupported gameinfo.txt path token: %s' % value)
        path = Path(normalized)
        if not path.is_absolute():
            path = base / path
        if '*' not in str(path) and '?' not in str(path):
            result.append(path.resolve())
            continue
        # Source sorts wildcard folders and VPKs together, then removes split
        # payload files belonging to an index in the same wildcard expansion.
        matches = sorted((Path(item) for item in glob.glob(str(path))),
                         key=lambda item: str(item).lower())
        indexes = {item.name[:-8].lower() for item in matches
                   if item.is_file() and item.name.lower().endswith('_dir.vpk')}
        for item in matches:
            if not item.is_dir() and item.suffix.lower() != '.vpk':
                continue
            chunk = re.fullmatch(r'(.+)_\d+\.vpk', item.name, re.I)
            if chunk and chunk.group(1).lower() in indexes:
                continue
            result.append(item.resolve())
    return result


def _vpk_index(path: Path) -> Path | None:
    """Source packedstore resolves the _dir index before a single-file VPK."""
    stem = path.stem
    if stem.lower().endswith('_dir'):
        stem = stem[:-4]
    for candidate in (path.with_name(stem + '_dir.vpk'), path.with_name(stem + '.vpk')):
        if candidate.is_file():
            return candidate
    return None


class ResourceStore:
    """Read the map pak, then GAME mounts in gameinfo.txt SearchPaths order.

    Map BSP paks are mounted at the GAME head by Source's map loader. External
    folders/VPKs remain in one ordered list, with no implicit directories,
    guessed packs, or MOD fallback. VPK directories are loaded lazily.
    """

    def __init__(self, game_dir: Path, pak_bytes: bytes | None = None):
        self.game_dir = Path(game_dir).resolve()
        self.warnings: list[str] = []
        paths = _search_paths(self.game_dir)
        self._pak = None
        self._pak_names = {}
        if pak_bytes:
            try:
                self._pak = zipfile.ZipFile(io.BytesIO(pak_bytes))
            except zipfile.BadZipFile as error:
                raise ValueError(f'Invalid BSP pak ZIP: {error}') from error
            for info in self._pak.infolist():
                if not info.is_dir():
                    self._pak_names[_resource_name(info.filename)] = info
        self._mounts = []
        seen = set()
        for path in paths:
            if path.suffix.lower() == '.vpk':
                index = _vpk_index(path)
                if index is None:
                    continue
                kind, item, key = 'vpk', _VPK(index), str(index.resolve()).lower()
            elif path.is_dir():
                kind, item, key = 'directory', path, str(path.resolve()).lower()
            else:
                continue
            identity = (kind, key)
            if identity not in seen:
                self._mounts.append((kind, item))
                seen.add(identity)

    def read(self, path: str) -> bytes:
        name = _resource_name(path)
        if name in self._pak_names:
            info = self._pak_names[name]
            if info.file_size > _MAX_RESOURCE:
                raise ValueError(f'BSP resource too large: {name}')
            try:
                return self._pak.read(info)
            except (zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
                raise ValueError(f'Cannot read BSP pak resource {name}: {error}') from error
        for kind, mount in self._mounts:
            if kind == 'vpk':
                result = mount.read(name)
                if result is not None:
                    return result
            else:
                candidate = mount.joinpath(*name.split('/'))
                if candidate.is_file():
                    if candidate.stat().st_size > _MAX_RESOURCE:
                        raise ValueError(f'Loose resource too large: {name}')
                    return candidate.read_bytes()
        raise FileNotFoundError(f'Game resource not found in configured GAME SearchPaths: {name}')


def _bounds(positions: list[float]) -> dict:
    return {"min": [min(positions[axis::3]) for axis in range(3)],
            "max": [max(positions[axis::3]) for axis in range(3)]}


def _vertices(data: bytes, checksum: int, *, uvs: list | None = None) -> list[tuple[float, float, float]]:
    signature, version, actual, lods = _unpack("<4siii", data, 0, "VVD header")
    if signature != b"IDSV" or version != 4:
        raise ValueError("Unsupported VVD signature/version (expected IDSV v4)")
    if actual != checksum:
        raise ValueError("MDL/VVD checksum mismatch")
    if not 1 <= lods <= 8:
        raise ValueError("Invalid VVD LOD count")
    counts = _unpack("<8i", data, 16, "VVD vertex counts")
    vertex_count = _count(counts[0], 4_000_000, "VVD vertex count")
    fixups, fixup_start, vertex_start, _ = _unpack("<4i", data, 48, "VVD data offsets")
    _count(fixups, vertex_count, "VVD fixup count")
    _span(data, vertex_start, vertex_count * 48, "VVD vertices")
    raw = [struct.unpack_from("<3f", data, vertex_start + index * 48 + 16)
           for index in range(vertex_count)]
    if any(not all(math.isfinite(value) for value in point) for point in raw):
        raise ValueError("Non-finite VVD vertex position")
    raw_uvs = ([struct.unpack_from("<2f", data, vertex_start + index * 48 + 40)
                for index in range(vertex_count)] if uvs is not None else None)
    if not fixups:
        if uvs is not None:
            uvs.extend(raw_uvs)
        return raw
    _span(data, fixup_start, fixups * 12, "VVD fixup table")
    result = []
    for index in range(fixups):
        lod, start, count = struct.unpack_from("<3i", data, fixup_start + index * 12)
        if not 0 <= lod < lods or start < 0 or count < 0 or start + count > vertex_count:
            raise ValueError("Invalid VVD fixup range")
        result.extend(raw[start:start + count])
        if uvs is not None:
            uvs.extend(raw_uvs[start:start + count])
    if len(result) != vertex_count:
        raise ValueError("VVD LOD0 fixups do not reconstruct the declared vertex count")
    return result


def _geometry(mdl: bytes, vtx: bytes, vertices: list, checksum: int, *,
              vertex_uvs: list | None = None, uvs: list | None = None,
              groups: list | None = None) -> list[float]:
    if uvs is not None and (vertex_uvs is None or len(vertex_uvs) != len(vertices)):
        raise ValueError("VVD texture coordinate count does not match vertices")
    version, = _unpack("<i", vtx, 0, "VTX version")
    if version != 7:
        raise ValueError(f"Unsupported VTX version: {version}")
    actual, lod_count, _, body_count, body_offset = _unpack("<5i", vtx, 16, "VTX header")
    if actual != checksum:
        raise ValueError("MDL/VTX checksum mismatch")
    if not 1 <= lod_count <= 8:
        raise ValueError("Invalid VTX LOD count")
    mdl_body_count, mdl_body_offset = _unpack("<2i", mdl, 232, "MDL body parts")
    if body_count != mdl_body_count:
        raise ValueError("MDL/VTX body part count mismatch")
    _count(body_count, 1024, "body part count")
    _span(mdl, mdl_body_offset, body_count * 16, "MDL body parts")
    _span(vtx, body_offset, body_count * 8, "VTX body parts")
    result = []
    for body_index in range(body_count):
        mdl_body = mdl_body_offset + body_index * 16
        _, models, _, model_offset = struct.unpack_from("<4i", mdl, mdl_body)
        vtx_body = body_offset + body_index * 8
        vtx_models, vtx_model_offset = struct.unpack_from("<2i", vtx, vtx_body)
        if models != vtx_models:
            raise ValueError("MDL/VTX body model count mismatch")
        _count(models, 4096, "body model count")
        if not models:
            continue
        # Body value zero selects the first model in each body part.
        model = mdl_body + model_offset
        _span(mdl, model, 108, "MDL model")
        meshes, mesh_offset, model_vertices, vertex_offset = struct.unpack_from("<4i", mdl, model + 72)
        _count(meshes, 65536, "model mesh count")
        _count(model_vertices, len(vertices), "model vertex count")
        if vertex_offset < 0 or vertex_offset % 48:
            raise ValueError("Invalid MDL model vertex byte offset")
        vertex_base = vertex_offset // 48
        if vertex_base + model_vertices > len(vertices):
            raise ValueError("MDL model vertices exceed VVD data")
        vtx_model = vtx_body + vtx_model_offset
        lods, lod_offset = _unpack("<2i", vtx, vtx_model, "VTX model")
        if lods != lod_count:
            raise ValueError("VTX model/file LOD count mismatch")
        lod = vtx_model + lod_offset
        vtx_mesh_count, vtx_mesh_offset, _ = _unpack("<iif", vtx, lod, "VTX LOD0")
        if vtx_mesh_count != meshes:
            raise ValueError("MDL/VTX mesh count mismatch")
        _span(mdl, model + mesh_offset, meshes * 116, "MDL meshes")
        _span(vtx, lod + vtx_mesh_offset, meshes * 9, "VTX meshes")
        for mesh_index in range(meshes):
            mesh = model + mesh_offset + mesh_index * 116
            material, = struct.unpack_from("<i", mdl, mesh)
            mesh_vertices, mesh_vertex_offset = struct.unpack_from("<2i", mdl, mesh + 8)
            if mesh_vertices < 0 or mesh_vertex_offset < 0 or mesh_vertex_offset + mesh_vertices > model_vertices:
                raise ValueError("Invalid MDL mesh vertex range")
            vtx_mesh = lod + vtx_mesh_offset + mesh_index * 9
            strip_groups, group_offset, mesh_flags = struct.unpack_from("<iiB", vtx, vtx_mesh)
            _count(strip_groups, 65536, "VTX strip group count")
            group_size = 33 if mesh_flags & 0x80 else 25
            _span(vtx, vtx_mesh + group_offset, strip_groups * group_size, "VTX strip groups")
            for group_index in range(strip_groups):
                group = vtx_mesh + group_offset + group_index * group_size
                vert_count, vert_offset, index_count, index_offset, strips, strip_offset, flags = struct.unpack_from("<6iB", vtx, group)
                _count(vert_count, 65536, "VTX strip group vertices")
                _count(index_count, 12_000_000, "VTX strip group indices")
                _count(strips, 1_000_000, "VTX strip count")
                _span(vtx, group + vert_offset, vert_count * 9, "VTX vertex remap")
                _span(vtx, group + index_offset, index_count * 2, "VTX indices")
                strip_size = 35 if flags & 0x80 else 27
                _span(vtx, group + strip_offset, strips * strip_size, "VTX strips")
                remap = [struct.unpack_from("<H", vtx, group + vert_offset + index * 9 + 4)[0]
                         for index in range(vert_count)]
                if any(index >= mesh_vertices for index in remap):
                    raise ValueError("VTX vertex remap exceeds MDL mesh vertices")
                indices = struct.unpack_from(f"<{index_count}H", vtx, group + index_offset)
                for strip_index in range(strips):
                    strip = group + strip_offset + strip_index * strip_size
                    count, start, num_verts, start_vert, _, strip_flags = struct.unpack_from("<4ihB", vtx, strip)
                    if count < 0 or start < 0 or start + count > index_count or num_verts < 0 or start_vert < 0 or start_vert + num_verts > vert_count:
                        raise ValueError("Invalid VTX strip range")
                    selected = indices[start:start + count]
                    if any(index >= vert_count for index in selected):
                        raise ValueError("VTX strip index exceeds strip group vertices")
                    if strip_flags & 3 == 1:
                        if count % 3:
                            raise ValueError("VTX triangle list index count is not divisible by 3")
                        triangles = (selected[index:index + 3] for index in range(0, count, 3))
                    elif strip_flags & 3 == 2:
                        triangles = ((selected[index + (index & 1)], selected[index + 1 - (index & 1)], selected[index + 2])
                                     for index in range(max(0, count - 2)))
                    else:
                        raise ValueError(f"Unsupported VTX strip flags: {strip_flags}")
                    for triangle in triangles:
                        if len(set(triangle)) < 3:
                            continue
                        start = len(result) // 3
                        for index in triangle:
                            vertex = vertex_base + mesh_vertex_offset + remap[index]
                            result.extend(vertices[vertex])
                            if uvs is not None:
                                uvs.extend(vertex_uvs[vertex])
                        if groups is not None:
                            if groups and groups[-1]["material"] == material and groups[-1]["start"] + groups[-1]["count"] == start:
                                groups[-1]["count"] += 3
                            else:
                                groups.append({"start": start, "count": 3, "material": material})
    return result


def _cstring(data: bytes, offset: int, label: str) -> str:
    _span(data, offset, 1, label)
    end = data.find(b"\0", offset, min(len(data), offset + 4096))
    if end < 0:
        raise ValueError(f"Unterminated or excessive {label}")
    return data[offset:end].decode("utf-8", errors="strict").replace("\\", "/")


def _material_name(name: str) -> str:
    name = name.replace("\\", "/").lstrip("/").lower()
    if name.startswith("materials/"):
        name = name[len("materials/"):]
    if name.endswith(".vmt"):
        name = name[:-4]
    return _resource_name(name)


def _model_materials(store, mdl, vtx, warnings):
    """Resolve LOD0 material names through the MDL's own search directories.

    The caller applies skin family zero to mesh.material. Missing VMTs retain
    their authored names so a renderer can report them and use gray fallback.
    """
    texture_count, texture_start, directory_count, directory_start, skinrefs, families, skin_start = _unpack(
        "<7i", mdl, 204, "MDL material table")
    _count(texture_count, 65536, "MDL material count")
    _count(directory_count, 4096, "MDL material directory count")
    _count(skinrefs, 65536, "MDL skin reference count")
    _count(families, 4096, "MDL skin family count")
    _span(mdl, texture_start, texture_count * 64, "MDL materials")
    _span(mdl, directory_start, directory_count * 4, "MDL material directories")
    names = []
    for i in range(texture_count):
        texture = texture_start + i * 64
        relative, = _unpack("<i", mdl, texture, "MDL material name offset")
        names.append(_cstring(mdl, texture + relative, "MDL material name"))
    # VTX can rename materials even for LOD0. Its structures are packed to one
    # byte, so MaterialReplacementHeader_t is exactly six bytes.
    replacement_start, = _unpack("<i", vtx, 24, "VTX material replacements")
    if replacement_start:
        count, relative = _unpack("<2i", vtx, replacement_start, "VTX LOD0 material replacements")
        _count(count, texture_count, "VTX material replacement count")
        _span(vtx, replacement_start + relative, count * 6, "VTX replacement records")
        for i in range(count):
            record = replacement_start + relative + i * 6
            material, name_relative = _unpack("<hi", vtx, record, "VTX material replacement")
            if not 0 <= material < texture_count:
                raise ValueError("VTX material replacement index exceeds MDL materials")
            names[material] = _cstring(vtx, record + name_relative, "VTX replacement material name")
    directories = []
    for i in range(directory_count):
        offset, = _unpack("<i", mdl, directory_start + i * 4, "MDL material directory offset")
        directory = _cstring(mdl, offset, "MDL material directory").strip("/")
        if directory.lower().startswith("materials/"):
            directory = directory[len("materials/"):]
        directories.append(directory)
    if not directories:
        directories = [""]
        warnings.append("MDL has no material search directory; using authored material names")
    materials = []
    for name in names:
        candidates = list(dict.fromkeys(_material_name(directory + "/" + name.lstrip("/"))
                                        for directory in directories))
        selected = candidates[0]
        found = False
        for candidate in candidates:
            try:
                store.read("materials/" + candidate + ".vmt")
            except FileNotFoundError:
                continue
            except (OSError, ValueError, RuntimeError, NotImplementedError, zipfile.BadZipFile) as error:
                selected = candidate
                warnings.append(f"Model material unavailable: {candidate} ({error})")
                found = True
                break
            selected, found = candidate, True
            break
        if not found:
            warnings.append(f"Model material VMT not found: {selected}")
        materials.append(selected)
    if skinrefs and families:
        _span(mdl, skin_start, skinrefs * families * 2, "MDL skin family table")
        skin = list(struct.unpack_from(f"<{skinrefs}H", mdl, skin_start))
        if any(index >= texture_count for index in skin):
            raise ValueError("MDL skin reference exceeds material table")
    else:
        skin = list(range(texture_count))
    return materials, skin


def _resolved_groups(raw_groups, materials, skin, warnings):
    """Compact the used skin-zero materials while preserving triangle order."""
    used, indices, result = [], {}, []
    warned = False
    for group in raw_groups:
        mesh_material = group["material"]
        if 0 <= mesh_material < len(skin) and 0 <= skin[mesh_material] < len(materials):
            name = materials[skin[mesh_material]]
        else:
            name = ""
            if not warned:
                warnings.append("Model mesh material reference is unavailable; using gray fallback")
                warned = True
        if name not in indices:
            indices[name] = len(used)
            used.append(name)
        material = indices[name]
        if result and result[-1]["material"] == material and result[-1]["start"] + result[-1]["count"] == group["start"]:
            result[-1]["count"] += group["count"]
        else:
            result.append({"start": group["start"], "count": group["count"], "material": material})
    return used, result


def load_model(store: ResourceStore, model_path: str) -> dict:
    """Return gray triangle geometry in Source model-local XYZ, plus diagnostics.

    Missing/unsupported/corrupt models produce no triangles and a warning. A
    valid MDL's view bounds remain available for an explicit bounds placeholder.
    No guessed model format, offsets, native calls, or silent partial geometry.
    """
    result = {"positions": [], "uvs": [], "materials": [], "groups": [], "bounds": None, "warnings": []}
    try:
        name = _resource_name(model_path)
        if not name.endswith(".mdl"):
            raise ValueError("Model path must end in .mdl")
        mdl = store.read(name)
        signature, version, checksum = _unpack("<4sii", mdl, 0, "MDL header")
        # SDK Studio_ConvertStudioHdrToNewVersion keeps the geometry layouts for v44-v49.
        if signature != b"IDST" or version not in (44, 45, 46, 47, 48, 49):
            raise ValueError(f"Unsupported MDL signature/version: {signature!r} v{version}")
        length, = _unpack("<i", mdl, 76, "MDL file length")
        if length < 240 or length != len(mdl):
            raise ValueError("MDL declared file length does not match resource")
        limits = _unpack("<6f", mdl, 128, "MDL view bounds")
        if all(math.isfinite(value) for value in limits) and all(limits[axis] <= limits[axis + 3] for axis in range(3)):
            result["bounds"] = {"min": list(limits[:3]), "max": list(limits[3:])}
        flags, = _unpack("<i", mdl, 152, "MDL flags")
        vertex_uvs = []
        vertices = _vertices(store.read(name[:-4] + ".vvd"), checksum, uvs=vertex_uvs)
        valid_uvs = all(all(math.isfinite(value) for value in uv) for uv in vertex_uvs)
        if not valid_uvs:
            vertex_uvs = [(0.0, 0.0)] * len(vertices)
            result["warnings"].append("Invalid VVD texture coordinates; using gray fallback")
        vtx = None
        for suffix in (".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx"):
            try:
                vtx = store.read(name[:-4] + suffix)
                break
            except FileNotFoundError:
                continue
        if vtx is None:
            raise FileNotFoundError("No VTX geometry resource (dx90/dx80/sw) found")
        uvs, raw_groups = [], []
        positions = _geometry(mdl, vtx, vertices, checksum, vertex_uvs=vertex_uvs, uvs=uvs, groups=raw_groups)
        if not positions:
            raise ValueError("Model body 0 / LOD0 contains no triangles")
        result["positions"] = positions
        result["uvs"] = uvs
        result["bounds"] = _bounds(positions)
        try:
            if not valid_uvs:
                raise ValueError("VVD texture coordinates are not finite")
            materials, skin = _model_materials(store, mdl, vtx, result["warnings"])
            result["materials"], result["groups"] = _resolved_groups(raw_groups, materials, skin, result["warnings"])
        except (OSError, ValueError, struct.error, RuntimeError, NotImplementedError, zipfile.BadZipFile) as error:
            # Material metadata cannot make otherwise valid model geometry vanish.
            result["warnings"].append(f"Model materials unavailable; using gray fallback ({error})")
            result["materials"] = [""]
            result["groups"] = [{"start": 0, "count": len(positions) // 3, "material": 0}]
        if not flags & 0x10:
            result["warnings"].append("Animated model displayed in bind pose; entity animation/body selection is not evaluated")
    except (OSError, ValueError, struct.error, zipfile.BadZipFile) as error:
        result["warnings"].append(str(error))
    return result


