"""Read-only Source BSP geometry for the standalone AGPW 3D viewer.

Binary layouts were checked against the local Source SDK's public/bspfile.h
and public/gamebspfile.h. This is an independent Python implementation; no
engine code, DLL, renderer, or AgPB plugin module is loaded.

Coordinates remain Source XYZ (Z up). Geometry is an unindexed triangle list.
The preview uses map-authored initial transforms, without game simulation.
"""

from __future__ import annotations

import lzma
import math
from pathlib import Path
import re
import struct


class BSPError(ValueError):
    """Unsupported or invalid BSP data, safe to present to a tool user."""


_MAX_LUMP_SIZE = 512 * 1024 * 1024
_HEADER_SIZE = 8 + 64 * 16 + 4
_SPRP = int.from_bytes(b"sprp", "big")
_HIDDEN_FLAGS = 0x0002 | 0x0004 | 0x0080 | 0x0100 | 0x0200
_HIDDEN_MATERIALS = {
    "tools/toolsclip", "tools/toolsplayerclip", "tools/toolsnpcclip",
    "tools/toolsnodraw", "tools/toolsskybox", "tools/toolsskybox2d",
    "tools/toolstrigger", "tools/toolsareaportal", "tools/toolsareaportalwindow",
    "tools/toolshint", "tools/toolsskip", "tools/toolsorigin",
    "tools/toolsinvisible", "tools/toolsinvisibleladder", "tools/toolsblocklight",
    "tools/toolsoccluder", "tools/toolsskyfog", "tools/toolsgrenadeclip",
}


def _slice(data, offset, size, label):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise BSPError(f"{label}: byte range {offset}+{size} exceeds {len(data)} bytes")
    return data[offset:offset + size]


def _records(data, fmt, label):
    size = struct.calcsize(fmt)
    if len(data) % size:
        raise BSPError(f"{label}: {len(data)} bytes is not a multiple of {size}")
    return list(struct.iter_unpack(fmt, data))


def _finite(values, label):
    if not all(math.isfinite(x) for x in values):
        raise BSPError(f"{label}: non-finite numeric value")
    return values


def _lzma(data, expected, label):
    if data[:4] != b"LZMA" or len(data) < 17:
        raise BSPError(f"{label}: invalid Source LZMA header")
    actual, packed = struct.unpack_from("<II", data, 4)
    if actual > _MAX_LUMP_SIZE or (expected and actual != expected):
        raise BSPError(f"{label}: invalid decompressed size {actual} (expected {expected})")
    compressed = _slice(data, 17, packed, label + " LZMA stream")
    prop = data[12]
    if prop >= 9 * 5 * 5:
        raise BSPError(f"{label}: invalid LZMA properties")
    dictionary = struct.unpack_from("<I", data, 13)[0]
    if dictionary > _MAX_LUMP_SIZE:
        raise BSPError(f"{label}: LZMA dictionary exceeds preview memory limit")
    filters = [{"id": lzma.FILTER_LZMA1, "dict_size": max(dictionary, 4096),
                "lc": prop % 9, "lp": (prop // 9) % 5, "pb": prop // 45}]
    try:
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
        result = decoder.decompress(compressed, max_length=actual + 1)
    except (lzma.LZMAError, ValueError) as exc:
        raise BSPError(f"{label}: LZMA decompression failed: {exc}") from exc
    if len(result) != actual:
        raise BSPError(f"{label}: decompressed length {len(result)} != {actual}")
    return result


class _BSP:
    def __init__(self, path, data: bytes | None = None):
        self.path = Path(path)
        if data is None:
            if self.path.stat().st_size > 1024 * 1024 * 1024:
                raise BSPError("BSP exceeds the 1 GiB preview input limit")
            data = self.path.read_bytes()
        if not isinstance(data, bytes):
            raise BSPError("BSP input data must be bytes")
        if len(data) > 1024 * 1024 * 1024:
            raise BSPError("BSP exceeds the 1 GiB preview input limit")
        self.data = data
        if len(self.data) < _HEADER_SIZE or self.data[:4] != b"VBSP":
            raise BSPError("Not a complete little-endian Source VBSP file")
        self.version = struct.unpack_from("<i", self.data, 4)[0]
        if self.version not in (19, 20, 21):
            raise BSPError(f"Unsupported BSP version {self.version}; expected 19, 20, or 21")
        self.lumps = [struct.unpack_from("<4i", self.data, 8 + i * 16) for i in range(64)]
        self.cache = {}
        for i, (offset, size, version, unpacked) in enumerate(self.lumps):
            if size < 0 or unpacked < 0 or size > _MAX_LUMP_SIZE or unpacked > _MAX_LUMP_SIZE:
                raise BSPError(f"Lump {i}: invalid or excessive byte count")
            if size:
                _slice(self.data, offset, size, f"Lump {i}")

    def lump(self, index, versions=None):
        offset, size, version, unpacked = self.lumps[index]
        if versions is not None and size and version not in versions:
            raise BSPError(f"Lump {index}: unsupported layout version {version}")
        if index not in self.cache:
            data = _slice(self.data, offset, size, f"Lump {index}") if size else b""
            if unpacked:
                data = _lzma(data, unpacked, f"Lump {index}")
            elif data[:4] == b"LZMA":
                raise BSPError(f"Lump {index}: LZMA data lacks an uncompressed-size declaration")
            self.cache[index] = data
        return self.cache[index]


def _entities(data):
    # Entity-lump grammar is quoted key/value pairs enclosed by braces.
    text = data.rstrip(b"\0").decode("utf-8", errors="replace")
    token_re = re.compile(r'//[^\n]*|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+')
    tokens = [m.group() for m in token_re.finditer(text) if not m.group().startswith("//")]

    def unquote(token):
        if len(token) < 2 or token[0] != '"' or token[-1] != '"':
            raise BSPError("Entities: expected a quoted string")
        return re.sub(r'\\(["\\])', r'\1', token[1:-1])

    result = []
    index = 0
    while index < len(tokens):
        if tokens[index] != "{":
            raise BSPError("Entities: expected opening brace")
        index += 1
        entity = {}
        while index < len(tokens) and tokens[index] != "}":
            if index + 1 >= len(tokens):
                raise BSPError("Entities: missing value")
            key = unquote(tokens[index]).lower()
            value = unquote(tokens[index + 1])
            entity[key] = value
            index += 2
        if index >= len(tokens):
            raise BSPError("Entities: missing closing brace")
        index += 1
        result.append(entity)
    return result


def _vector(text, label):
    try:
        values = [float(x) for x in text.split()]
    except ValueError as exc:
        raise BSPError(f"{label}: invalid vector {text!r}") from exc
    if len(values) != 3:
        raise BSPError(f"{label}: expected three components")
    return _finite(values, label)


def _angles(entity):
    if "angles" in entity:
        return _vector(entity["angles"], "Entity angles")
    try:
        angle = float(entity.get("angle", "0"))
    except ValueError as exc:
        raise BSPError("Entity angle: invalid scalar") from exc
    _finite([angle], "Entity angle")
    return [-90.0, 0.0, 0.0] if angle == -1 else ([90.0, 0.0, 0.0] if angle == -2 else [0.0, angle, 0.0])


def _matrix(angles, origin):
    # Source's QAngle convention: yaw around Z, pitch around Y, roll around X.
    pitch, yaw, roll = (math.radians(x) for x in angles)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    cr, sr = math.cos(roll), math.sin(roll)
    return ((cp * cy, sp * sr * cy - cr * sy, sp * cr * cy + sr * sy, origin[0]),
            (cp * sy, sp * sr * sy + cr * cy, sp * cr * sy - sr * cy, origin[1]),
            (-sp, sr * cp, cr * cp, origin[2]))


def _transform(point, matrix):
    return tuple(row[0] * point[0] + row[1] * point[1] + row[2] * point[2] + row[3]
                 for row in matrix)


def _surface_flags(bsp):
    info = _records(bsp.lump(6, (0,)), "<16fii", "Texinfo")
    texdata = _records(bsp.lump(2, (0,)), "<3f5i", "Texdata")
    strings = bsp.lump(43, (0,))
    table = _records(bsp.lump(44, (0,)), "<i", "Texture string table")
    result = []
    for index, item in enumerate(info):
        flags, texture = item[-2:]
        if texture < 0 or texture >= len(texdata):
            raise BSPError(f"Texinfo {index}: invalid texdata index {texture}")
        name_id = texdata[texture][3]
        if name_id < 0 or name_id >= len(table):
            raise BSPError(f"Texdata {texture}: invalid material string index")
        start = table[name_id][0]
        if start < 0 or start >= len(strings):
            raise BSPError(f"Material {name_id}: invalid string offset")
        end = strings.find(b"\0", start)
        if end < 0:
            raise BSPError(f"Material {name_id}: unterminated name")
        name = strings[start:end].decode("ascii", errors="replace").replace("\\", "/").lower()
        is_hidden = bool(flags & _HIDDEN_FLAGS) or name in _HIDDEN_MATERIALS
        if name.startswith("materials/"):
            name = name[len("materials/"):]
        if name.endswith(".vmt"):
            name = name[:-4]
        axes = _finite(item[:8], f"Texinfo {index} texture axes")
        width, height = texdata[texture][4:6]
        result.append({"hidden": is_hidden, "material": name, "axes": axes,
                       "width": max(width, 1), "height": max(height, 1)})
    return result


def _static_props(bsp, warnings):
    data = bsp.lump(35, (0,))
    if not data:
        return [], None
    if len(data) < 4:
        raise BSPError("Game lump: missing directory header")
    count = struct.unpack_from("<i", data)[0]
    if count < 0 or count > (len(data) - 4) // 16:
        raise BSPError("Game lump: invalid directory count")
    entries = [struct.unpack_from("<IHHii", data, 4 + i * 16) for i in range(count)]
    matches = [e for e in entries if e[0] == _SPRP]
    if not matches:
        return [], None
    if len(matches) != 1:
        raise BSPError("Game lump: duplicate static prop entries")
    _, flags, version, offset, size = matches[0]
    if flags & ~1:
        warnings.append(f"Static props omitted: unsupported game-lump flags 0x{flags:x}")
        return [], version
    # These are format versions, not runtime guesses about a game.
    strides = {4: 56, 5: 60, 6: 64, 7: 68, 8: 68, 9: 72,
               10: 76 if bsp.version == 21 else 72, 11: 80}
    if version not in strides:
        warnings.append(f"Static props omitted: unsupported sprp version {version}")
        return [], version
    if size < 0 or size > _MAX_LUMP_SIZE:
        raise BSPError("Static props: invalid byte count")
    if flags & 1:
        # Compressed game-lump filelen stores the original byte count. A terminal
        # directory entry (or the enclosing lump's end) bounds the packed stream.
        following = [entry[3] for entry in entries if entry[3] > offset]
        enclosing_end = bsp.lumps[35][0] + bsp.lumps[35][1]
        end = min(following) if following else enclosing_end
        raw = _slice(bsp.data, offset, end - offset, "Static props compressed data")
        raw = _lzma(raw, size, "Static props")
    else:
        raw = _slice(bsp.data, offset, size, "Static props")
    cursor = 0

    def read_count(label):
        nonlocal cursor
        result = struct.unpack("<i", _slice(raw, cursor, 4, label))[0]
        cursor += 4
        if result < 0:
            raise BSPError(f"{label}: negative count")
        return result

    dictionary_count = read_count("Static prop model dictionary")
    dictionary_data = _slice(raw, cursor, dictionary_count * 128, "Static prop model dictionary")
    dictionary = [dictionary_data[i * 128:(i + 1) * 128].split(b"\0", 1)[0].decode("ascii", errors="replace")
                  .replace("\\", "/") for i in range(dictionary_count)]
    cursor += dictionary_count * 128
    leaf_count = read_count("Static prop leaf table")
    _slice(raw, cursor, leaf_count * 2, "Static prop leaf table")
    cursor += leaf_count * 2
    prop_count = read_count("Static props")
    stride = strides[version]
    if len(raw) - cursor != prop_count * stride:
        raise BSPError(f"Static props v{version}: record layout mismatch; "
                       f"{prop_count} records require {prop_count * stride} bytes, found {len(raw) - cursor}")
    props = []
    for i in range(prop_count):
        base = cursor + i * stride
        values = _finite(struct.unpack_from("<6f", raw, base), f"Static prop {i}")
        model_index, first_leaf, num_leaves = struct.unpack_from("<3H", raw, base + 24)
        if model_index >= len(dictionary) or first_leaf + num_leaves > leaf_count:
            raise BSPError(f"Static prop {i}: invalid model/leaf reference")
        scale = struct.unpack_from("<f", raw, base + 76)[0] if version == 11 else 1.0
        _finite([scale], f"Static prop {i} scale")
        props.append({"model": dictionary[model_index], "origin": list(values[:3]),
                      "angles": list(values[3:]), "scale": scale, "kind": "static"})
    return props, version


def load_bsp(path, data: bytes | None = None):
    """Return gray-map triangle positions, initial props, bounds and diagnostics.

    If data is supplied, path is a logical map name and is never read from disk.
    `pak_bytes` is the decompressed BSP pakfile ZIP for model-resource lookup.
    Missing textures are irrelevant; invalid geometry is rejected, and unknown
    static-prop layouts are reported instead of inferred from record lengths.
    """
    bsp = _BSP(path, data)
    warnings = []
    entities = _entities(bsp.lump(0, (0,)))
    vertices = _records(bsp.lump(3, (0,)), "<3f", "Vertices")
    for i, vertex in enumerate(vertices):
        _finite(vertex, f"Vertex {i}")
    edges = _records(bsp.lump(12, (0,)), "<2H", "Edges")
    surfedges = [v[0] for v in _records(bsp.lump(13, (0,)), "<i", "Surfedges")]
    face_data = bsp.lump(7, (0, 1))
    if not face_data:
        face_data = bsp.lump(58, (0, 1))
    faces = _records(face_data, "<HBBihhhh4Bif4iiHHI", "Faces")
    models = _records(bsp.lump(14, (0,)), "<9f3i", "Brush models")
    surface_info = _surface_flags(bsp)
    disp_data = bsp.lump(26, (0,))
    if len(disp_data) % 176:
        raise BSPError("Displacements: expected 176-byte ddispinfo records")
    disp_count = len(disp_data) // 176
    disp_verts = _records(bsp.lump(33, (0,)), "<5f", "Displacement vertices")
    disp_tags = [x[0] for x in _records(bsp.lump(48, (0,)), "<H", "Displacement triangles")]
    props, prop_version = _static_props(bsp, warnings)
    static_count = len(props)
    brush_entities = {}
    for i, entity in enumerate(entities):
        model = entity.get("model", "")
        if model.startswith("*"):
            try:
                model_index = int(model[1:])
            except ValueError as exc:
                raise BSPError(f"Entity {i}: invalid brush model reference {model!r}") from exc
            if model_index <= 0 or model_index >= len(models):
                raise BSPError(f"Entity {i}: missing brush model {model!r}")
            brush_entities.setdefault(model_index, []).append(entity)
        elif model.lower().endswith(".mdl"):
            try:
                origin = _vector(entity.get("origin", "0 0 0"), "Model entity origin")
                angles = _angles(entity)
                scale = float(entity.get("modelscale", "1"))
                _finite([scale], "Model entity scale")
            except (ValueError, BSPError) as exc:
                warnings.append(f"Entity model {model} omitted: {exc}")
                continue
            props.append({"model": model.replace("\\", "/"), "origin": origin,
                          "angles": angles, "scale": scale, "kind": entity.get("classname", "entity")})
    positions, uvs, materials, groups = [], [], [], []
    material_ids = {}
    low, high = [math.inf] * 3, [-math.inf] * 3
    counts = {"world_triangles": 0, "brush_triangles": 0, "displacement_triangles": 0,
              "displacement_surfaces": 0, "visible_faces": 0, "hidden_faces": 0,
              "degenerate_triangles": 0}

    def texture_uv(point, surface):
        axes = surface["axes"]
        return ((sum(point[j] * axes[j] for j in range(3)) + axes[3]) / surface["width"],
                (sum(point[j] * axes[j + 4] for j in range(3)) + axes[7]) / surface["height"])

    def emit(a, b, c, matrix, category, surface, triangle_uvs=None):
        # Brush texture axes are local to the compiled model. Evaluate before
        # applying the entity transform so moving/rotating a brush keeps its UVs.
        if triangle_uvs is None:
            triangle_uvs = [texture_uv(point, surface) for point in (a, b, c)]
        if matrix is not None:
            a, b, c = (_transform(v, matrix) for v in (a, b, c))
        ab = [b[j] - a[j] for j in range(3)]
        ac = [c[j] - a[j] for j in range(3)]
        cross = (ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0])
        if sum(x * x for x in cross) < 1e-12:
            counts["degenerate_triangles"] += 1
            return
        for point in (a, b, c):
            _finite(point, "Transformed vertex")
            positions.extend(point)
            for axis in range(3):
                low[axis] = min(low[axis], point[axis])
                high[axis] = max(high[axis], point[axis])
        for uv in triangle_uvs:
            uvs.extend(_finite(uv, "Texture coordinates"))
        name = surface["material"]
        if name not in material_ids:
            material_ids[name] = len(materials)
            materials.append(name)
        material = material_ids[name]
        start = len(positions) // 3 - 3
        if groups and groups[-1]["material"] == material and groups[-1]["start"] + groups[-1]["count"] == start:
            groups[-1]["count"] += 3
        else:
            groups.append({"start": start, "count": 3, "material": material})
        counts[category] += 1

    def polygon(face_id):
        face = faces[face_id]
        first, count = face[3:5]
        if count < 3 or first < 0 or first + count > len(surfedges):
            raise BSPError(f"Face {face_id}: invalid surfedge range")
        points = []
        for edge_id in surfedges[first:first + count]:
            if abs(edge_id) >= len(edges):
                raise BSPError(f"Face {face_id}: missing edge {edge_id}")
            vertex_id = edges[abs(edge_id)][1 if edge_id < 0 else 0]
            if vertex_id >= len(vertices):
                raise BSPError(f"Face {face_id}: missing vertex {vertex_id}")
            points.append(vertices[vertex_id])
        return points

    def displacement(face_id, points, matrix, surface):
        disp_id = faces[face_id][6]
        if disp_id >= disp_count:
            raise BSPError(f"Face {face_id}: missing displacement {disp_id}")
        base = disp_id * 176
        start = _finite(struct.unpack_from("<3f", disp_data, base), "Displacement start")
        vert_start, tri_start, power = struct.unpack_from("<3i", disp_data, base + 12)
        map_face = struct.unpack_from("<H", disp_data, base + 36)[0]
        if len(points) != 4 or power not in (2, 3, 4) or map_face != face_id:
            raise BSPError(f"Displacement {disp_id}: invalid quad, power, or parent face")
        width = (1 << power) + 1
        total_tris = (width - 1) ** 2 * 2
        if vert_start < 0 or vert_start + width * width > len(disp_verts):
            raise BSPError(f"Displacement {disp_id}: invalid vertex range")
        if tri_start < 0 or tri_start + total_tris > len(disp_tags):
            raise BSPError(f"Displacement {disp_id}: invalid triangle tag range")
        corner = min(range(4), key=lambda k: sum((points[k][j] - start[j]) ** 2 for j in range(3)))
        p = points[corner:] + points[:corner]
        corner_uvs = [texture_uv(point, surface) for point in p]
        grid, grid_uvs = [], []
        for row in range(width):
            v = row / (width - 1)
            edge_a = [p[0][j] + (p[1][j] - p[0][j]) * v for j in range(3)]
            edge_b = [p[3][j] + (p[2][j] - p[3][j]) * v for j in range(3)]
            uv_a = [corner_uvs[0][j] + (corner_uvs[1][j] - corner_uvs[0][j]) * v for j in range(2)]
            uv_b = [corner_uvs[3][j] + (corner_uvs[2][j] - corner_uvs[3][j]) * v for j in range(2)]
            for col in range(width):
                u = col / (width - 1)
                field = _finite(disp_verts[vert_start + row * width + col], "Displacement field")
                grid.append(tuple(edge_a[j] + (edge_b[j] - edge_a[j]) * u + field[j] * field[3] for j in range(3)))
                # Source interpolates the base quad UVs; the displacement vector
                # changes geometry without reprojecting the texture coordinates.
                grid_uvs.append(tuple(uv_a[j] + (uv_b[j] - uv_a[j]) * u for j in range(2)))
        triangle_id = tri_start
        for row in range(width - 1):
            for col in range(width - 1):
                k = row * width + col
                triangles = ((k, k + width, k + 1), (k + 1, k + width, k + width + 1)) if k % 2 else (
                    (k, k + width, k + width + 1), (k, k + width + 1, k + 1))
                for indices in triangles:
                    if not disp_tags[triangle_id] & 0x20:
                        emit(*(grid[j] for j in indices), matrix, "displacement_triangles",
                             surface, [grid_uvs[j] for j in indices])
                    triangle_id += 1
        counts["displacement_surfaces"] += 1

    if not models:
        raise BSPError("BSP has no world brush model")
    for model_id, model in enumerate(models):
        first, count = model[-2:]
        if first < 0 or count < 0 or first + count > len(faces):
            raise BSPError(f"Brush model {model_id}: invalid face range")
        instances = [None] if model_id == 0 else brush_entities.get(model_id, [])
        for entity in instances:
            matrix = None if entity is None else _matrix(_angles(entity), _vector(entity.get("origin", "0 0 0"), "Brush origin"))
            # Trigger/volume models do not have a visible rendered surface.
            classname = "worldspawn" if entity is None else entity.get("classname", "")
            if classname.startswith("trigger_") or classname in ("func_buyzone", "func_hostage_rescue", "func_areaportal", "func_occluder"):
                continue
            category = "world_triangles" if model_id == 0 else "brush_triangles"
            for face_id in range(first, first + count):
                face = faces[face_id]
                texture = face[5]
                if texture == -1:
                    counts["hidden_faces"] += 1
                    continue
                if texture < 0 or texture >= len(surface_info):
                    raise BSPError(f"Face {face_id}: invalid texinfo {texture}")
                surface = surface_info[texture]
                if surface["hidden"]:
                    counts["hidden_faces"] += 1
                    continue
                points = polygon(face_id)
                counts["visible_faces"] += 1
                if face[6] >= 0:
                    displacement(face_id, points, matrix, surface)
                else:
                    for j in range(1, len(points) - 1):
                        emit(points[0], points[j], points[j + 1], matrix, category, surface)
    if not positions:
        raise BSPError("BSP contains no supported visible geometry")
    play_bounds = None
    if entities and "world_mins" in entities[0] and "world_maxs" in entities[0]:
        play_min = _vector(entities[0]["world_mins"], "World bounds")
        play_max = _vector(entities[0]["world_maxs"], "World bounds")
        if all(play_min[i] < play_max[i] for i in range(3)):
            play_bounds = {"min": play_min, "max": play_max}
    counts.update({"faces": len(faces), "triangles": len(positions) // 9, "vertices": len(vertices),
                   "brush_models": len(models), "entities": len(entities), "static_props": static_count,
                   "entity_props": len(props) - static_count, "prop_models": len({p["model"].lower() for p in props}),
                   "static_prop_version": prop_version})
    return {"name": bsp.path.stem, "version": bsp.version, "positions": positions,
            "uvs": uvs, "materials": materials, "groups": groups,
            "props": props, "bounds": {"min": low, "max": high}, "play_bounds": play_bounds,
            "stats": counts, "warnings": warnings, "pak_bytes": bsp.lump(40, (0,))}
