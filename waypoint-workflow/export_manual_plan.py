"""Serialize an explicitly authored waypoint plan; never infer nodes or connections."""
import argparse
import json
import math
import struct
from pathlib import Path

HEADER = struct.Struct('<III32s32s')
NODE = struct.Struct('<3fIBB8h8Hf')
NODE_V2 = struct.Struct('<3fIBB8h8Hf24f')
NODES = {1: NODE, 2: NODE_V2}
MAGIC = 0x50424741
VERSION = 1
JUMP_VELOCITY_MAX = 2000.0


def integer(value, field):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(field + ' must be an integer')
    return value


def number(value, field):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(field + ' must be a finite number')
    try:
        value = float(value)
    except OverflowError as exc:
        raise ValueError(field + ' must be a finite float32 number') from exc
    if not math.isfinite(value):
        raise ValueError(field + ' must be a finite number')
    try:
        struct.pack('<f', value)
    except (OverflowError, struct.error) as exc:
        raise ValueError(field + ' does not fit a float32') from exc
    return value


def header_string(value, field):
    if not isinstance(value, str) or not value or '\0' in value:
        raise ValueError(field + ' must be a nonempty string without NUL')
    try:
        encoded = value.encode('ascii')
    except UnicodeEncodeError as exc:
        raise ValueError(field + ' must be ASCII') from exc
    if len(encoded) > 31:
        raise ValueError(field + ' exceeds 31 bytes')
    return encoded


def jump_velocity(value):
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError('jumpVelocity must contain exactly three XYZ components')
    result = [number(component, 'jumpVelocity') for component in value]
    if any(abs(component) > JUMP_VELOCITY_MAX for component in result):
        raise ValueError('jumpVelocity components must be between -2000 and 2000 units/s')
    return [struct.unpack('<f', struct.pack('<f', component))[0] for component in result]


def export(plan):
    """Return AGPW v1/v2 bytes, validating before any output is written.

    Radius is mandatory. Zero flags, mesh=0 and gravity=1 retain the existing
    explicit-plan defaults. Optional edge.slot preserves explicitly chosen
    connection holes; each source uses either all explicit or all implicit
    slots. Missing directed-edge jumpVelocity is XYZ zero. A
    nonzero jumpVelocity or
    explicit agpw_version=2 selects v2; unchanged old plans still export v1.
    plan_version describes the JSON schema, independently of the AGPW version.
    """
    if not isinstance(plan, dict):
        raise ValueError('plan must be an object')
    if integer(plan.get('plan_version', VERSION), 'plan_version') != VERSION:
        raise ValueError('Only plan_version 1 is supported')
    nodes = plan.get('nodes')
    edges = plan.get('directed_edges')
    if not isinstance(nodes, list) or not 0 < len(nodes) <= 8192:
        raise ValueError('nodes must contain 1..8192 explicit nodes')
    if not isinstance(edges, list):
        raise ValueError('directed_edges must be an explicit list')
    file_version = integer(plan.get('agpw_version', 1), 'agpw_version')
    if file_version not in NODES:
        raise ValueError('Only agpw_version 1 or 2 is supported')
    labels = {}
    for index, node in enumerate(nodes):
        if not isinstance(node, dict):
            raise ValueError('node must be an object')
        label = node.get('label')
        if not isinstance(label, str) or not label or label in labels:
            raise ValueError('Invalid or duplicate node label')
        labels[label] = index
    adjacency = [[] for _ in nodes]
    seen = set()
    for edge in edges:
        if not isinstance(edge, dict):
            raise ValueError('edge must be an object')
        source, target = edge.get('from'), edge.get('to')
        if not isinstance(source, str) or not isinstance(target, str) or source not in labels or target not in labels:
            raise ValueError('edge references an unknown node label')
        pair = (labels[source], labels[target])
        flags = integer(edge.get('flags', 0), 'edge flags')
        if pair[0] == pair[1] or not 0 <= flags <= 65535:
            raise ValueError('Invalid explicit edge')
        if pair in seen:
            raise ValueError('Duplicate directed edge: ' + source + ' -> ' + target)
        seen.add(pair)
        velocity = jump_velocity(edge.get('jumpVelocity', [0.0, 0.0, 0.0]))
        if any(component != 0 for component in velocity):
            if not flags & 1:
                raise ValueError('Nonzero jumpVelocity requires an explicit PATH_JUMP edge')
            file_version = 2
        slot = integer(edge['slot'], 'edge slot') if 'slot' in edge else None
        if slot is not None and not 0 <= slot < 8:
            raise ValueError('edge slot must be between 0 and 7')
        adjacency[pair[0]].append((pair[1], flags, velocity, slot))
    if any(len(row) > 8 for row in adjacency):
        raise ValueError('More than eight outgoing edges')
    for row in adjacency:
        explicit = [slot for _, _, _, slot in row if slot is not None]
        if explicit and len(explicit) != len(row):
            raise ValueError('A source must use all explicit edge slots or omit all slots')
        if len(explicit) != len(set(explicit)):
            raise ValueError('Duplicate outgoing edge slot')
    name = header_string(plan.get('map'), 'map')
    author = header_string(plan.get('author', 'AgPB manual waypoints'), 'author')
    data = bytearray(HEADER.pack(MAGIC, file_version, len(nodes), name, author))
    for index, (node, row) in enumerate(zip(nodes, adjacency)):
        xyz = [number(node.get(key), 'node %d %s' % (index, key)) for key in ('x', 'y', 'z')]
        flags = integer(node.get('flags', 0), 'node flags')
        radius = integer(node.get('radius'), 'radius (must be explicit)')
        mesh = integer(node.get('mesh', 0), 'mesh')
        gravity = number(node.get('gravity', 1.0), 'gravity')
        if not 0 <= radius <= 255 or not 0 <= flags <= 0xffffffff or not 0 <= mesh <= 255:
            raise ValueError('Invalid node fields')
        targets, link_flags, velocities = [-1] * 8, [0] * 8, [[0.0, 0.0, 0.0] for _ in range(8)]
        for implicit_slot, (target, value, velocity, explicit_slot) in enumerate(row):
            slot = implicit_slot if explicit_slot is None else explicit_slot
            targets[slot], link_flags[slot], velocities[slot] = target, value, velocity
        values = (*xyz, flags, radius, mesh, *targets, *link_flags, gravity)
        if file_version == 2:
            values += tuple(component for velocity in velocities for component in velocity)
        data.extend(NODES[file_version].pack(*values))
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        plan = json.loads(args.plan.read_text(encoding='utf-8-sig'))
        data = export(plan)
        args.output.write_bytes(data)
    except (OSError, ValueError) as exc:
        parser.exit(2, 'error: ' + str(exc) + '\n')
    print(json.dumps({'map': plan['map'], 'nodes': len(plan['nodes']),
                      'directed_edges': len(plan['directed_edges']),
                      'agpw_version': HEADER.unpack_from(data)[1],
                      'bytes': len(data), 'output': str(args.output)}))


if __name__ == '__main__':
    main()
