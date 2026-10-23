"""Serialize an explicitly authored waypoint plan; never infer nodes or connections."""
import argparse
import json
import math
import struct
from pathlib import Path

HEADER = struct.Struct('<III32s32s')
NODE = struct.Struct('<3fIBB8h8Hf')
MAGIC = 0x50424741
VERSION = 1


def integer(value, field):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(field + ' must be an integer')
    return value


def number(value, field):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(field + ' must be a finite number')
    value = float(value)
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


def export(plan):
    """Return AGPW v1 bytes, rejecting ambiguity before any output is written.

    Radius is mandatory. Zero flags, mesh=0 and gravity=1 retain the existing
    explicit-plan format defaults; none of these defaults creates graph edges.
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
        adjacency[pair[0]].append((pair[1], flags))
    if any(len(row) > 8 for row in adjacency):
        raise ValueError('More than eight outgoing edges')
    name = header_string(plan.get('map'), 'map')
    author = header_string(plan.get('author', 'AgPB manual waypoints'), 'author')
    data = bytearray(HEADER.pack(MAGIC, VERSION, len(nodes), name, author))
    for index, (node, row) in enumerate(zip(nodes, adjacency)):
        xyz = [number(node.get(key), 'node %d %s' % (index, key)) for key in ('x', 'y', 'z')]
        flags = integer(node.get('flags', 0), 'node flags')
        radius = integer(node.get('radius'), 'radius (must be explicit)')
        mesh = integer(node.get('mesh', 0), 'mesh')
        gravity = number(node.get('gravity', 1.0), 'gravity')
        if not 0 <= radius <= 255 or not 0 <= flags <= 0xffffffff or not 0 <= mesh <= 255:
            raise ValueError('Invalid node fields')
        targets = [target for target, _ in row] + [-1] * (8 - len(row))
        link_flags = [value for _, value in row] + [0] * (8 - len(row))
        data.extend(NODE.pack(*xyz, flags, radius, mesh, *targets, *link_flags, gravity))
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
                      'bytes': len(data), 'output': str(args.output)}))


if __name__ == '__main__':
    main()