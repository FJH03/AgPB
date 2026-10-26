#!/usr/bin/env python3
"""Read-only append-only AGPW and current-session incremental walking audit.

Old nodes/edges are a frozen baseline, never this session's coverage targets.
Reuse verify_routes.py records and audit_waypoints' route/asset/stop checks.
Ladder nodes are traversed continuously to an ordinary grounded exit, rather
than requested as grounded endpoints. No graph or game state is modified.
"""
import argparse
import json
import math
from pathlib import Path
import re

import audit_waypoints as audit
from export_manual_plan import HEADER, NODES, integer

WP_LADDER = 1 << 5
MOVETYPE_LADDER = 9
FIELDS = re.compile(r'([A-Za-z_][A-Za-z_0-9]*)=(\([^)]*\)|[^\s]+)')
PROBE = re.compile(r'^\[AGPB_JUMP_SAMPLE\] (.+)$', re.M)
CORE = re.compile(r'^\[AgPB lifecycle\] v=1 name=(.+?) edict=(.+)$', re.M)


def require(value, message):
    if not value:
        raise ValueError(message)


def additions(value):
    require(isinstance(value, dict) and value.get('schema_version') == 1,
            'additions manifest requires schema_version=1')
    rows = value.get('additions')
    require(isinstance(rows, list), 'additions must be an explicit list')
    result = set()
    for row in rows:
        require(isinstance(row, dict) and set(row) == {'from', 'to', 'slot'},
                'each addition requires exactly from/to/slot')
        source, target, slot = (integer(row[key], key) for key in ('from', 'to', 'slot'))
        require(source >= 0 and target >= 0 and source != target and 0 <= slot < 8,
                'invalid explicit addition')
        require((source, target, slot) not in result, 'duplicate explicit addition')
        result.add((source, target, slot))
    return result


def edge_slots(decoded):
    result = {}
    for source, node in enumerate(decoded['nodes']):
        seen = set()
        for slot in range(8):
            target = node['index[%d]' % slot]
            require(target == -1 or 0 <= target < decoded['count'] and target != source,
                    'invalid link target at #%d slot %d' % (source, slot))
            if target != -1:
                require(target not in seen, 'duplicate link target at #%d' % source)
                seen.add(target)
                result[source, target, slot] = node['connectionFlags[%d]' % slot]
    return result


def compare_incremental(original, candidate, manifest, allow_author_change=False):
    before, after = audit.decode_agpw(original), audit.decode_agpw(candidate)
    allowed = additions(manifest)
    old_edges, final_edges = edge_slots(before), edge_slots(after)
    errors, differences = [], []
    if before['version'] != after['version']:
        errors.append('disk version changed; old record bytes cannot be preserved')
    if after['count'] < before['count']:
        errors.append('old nodes were removed')
    if before['map_raw'] != after['map_raw']:
        errors.append('map header bytes changed')
    author_changed = before['author_raw'] != after['author_raw']
    if author_changed and not allow_author_change:
        errors.append('author header bytes changed without explicit permission')
    found = set(final_edges) - set(old_edges)
    missing = sorted(allowed - found)
    unexpected = sorted(found - allowed)
    if missing:
        errors.append('declared additions missing from candidate')
    if unexpected:
        errors.append('candidate has undeclared additions')
    if set(old_edges) - set(final_edges):
        errors.append('existing directed edge or slot was removed/replaced')
    if before['version'] == after['version']:
        size = NODES[before['version']].size
        for index in range(min(before['count'], after['count'])):
            start = HEADER.size + index * size
            old = original[start:start + size]
            new = candidate[start:start + size]
            editable = set()
            for source, target, slot in allowed & found:
                if source != index:
                    continue
                if before['nodes'][index]['index[%d]' % slot] != -1:
                    errors.append('addition overwrites old occupied slot at #%d/%d' % (index, slot))
                    continue
                editable.update(range(18 + slot * 2, 20 + slot * 2))
                editable.update(range(34 + slot * 2, 36 + slot * 2))
                if before['version'] == 2:
                    editable.update(range(54 + slot * 12, 66 + slot * 12))
            changed = [offset for offset, (a, b) in enumerate(zip(old, new)) if a != b and offset not in editable]
            if changed:
                differences.append({'node': index, 'record_byte_offsets': changed})
        if differences:
            errors.append('frozen old node bytes changed outside declared empty connection slots')
    return {'audit': 'incremental_compare', 'accepted': not errors,
            'original_sha256': audit.digest(original), 'candidate_sha256': audit.digest(candidate),
            'map': after['map_raw'].split(b'\0')[0].decode('ascii'),
            'baseline_nodes': before['count'], 'candidate_nodes': after['count'],
            'new_node_ids': list(range(before['count'], after['count'])),
            'new_directed_edges': [{'from': source, 'to': target, 'slot': slot,
                                    'flags': final_edges[source, target, slot]}
                                   for source, target, slot in sorted(found)],
            'missing_declared_additions': missing, 'undeclared_additions': unexpected,
            'frozen_byte_differences': differences, 'author_changed': author_changed,
            'old_record_bytes_preserved': not differences and before['version'] == after['version'] and after['count'] >= before['count'],
            'errors': errors}


def vector(value):
    result = [float(part) for part in re.split(r'[,\s]+', value.strip('()'))]
    require(len(result) == 3 and all(map(math.isfinite, result)), 'invalid observed vector')
    return result


def record_log(record):
    # These are complete raw windows, not reconstructed status claims. Repeated
    # sample lines from overlapping windows are deduplicated by epoch/gen/seq.
    return '\n'.join(record.get(key, '') for key in ('log', 'observed_log', 'ladder_log')
                     if isinstance(record.get(key), str))


def ladder_evidence(record, metadata, plan, route, expected_probe_hash):
    result = {'accepted': False, 'ladder_node_ids': [], 'errors': []}
    try:
        require(expected_probe_hash and metadata.get('jump_probe_sha256') == expected_probe_hash,
                'missing/stale current jump_probe_sha256 binding')
        require(not plan['nodes'][route[-1]]['flags'] & WP_LADDER,
                'ladder route must stop at an ordinary exit')
        text = record_log(record)
        matches = audit.route_log_matches(audit.normalized_log(text), route, route[-1])
        require(matches and all(row['bot'] == metadata.get('bot_name') for row in matches),
                'ladder observations lack this bot walking/arrival ownership window')
        observed, seen = [], {}
        identity = None
        for match in PROBE.finditer(text):
            values = dict(FIELDS.findall(match[1]))
            key = values.get('epoch'), values.get('generation'), values.get('seq')
            if key in seen:
                require(seen[key] == match[1], 'conflicting repeated probe sample')
                continue
            seen[key] = match[1]
            integers = ('generation', 'map_serial', 'tick', 'sample', 'bot_slot', 'bot_userid',
                        'bot_serial', 'human_slot', 'human_userid', 'human_serial', 'team', 'movetype',
                        'ground_known', 'ground_ent', 'bot_fake', 'bot_alive', 'human_fake', 'human_alive')
            sample = {key: int(values[key], 0) for key in integers}
            sample.update(time=float(values['t']), position=vector(values['pos']),
                          velocity=vector(values['velocity']), flags=int(values['flags'], 0),
                          epoch=values['epoch'], map=values['map'])
            require(math.isfinite(sample['time']) and sample['map'] == plan['map'], 'probe map/time mismatch')
            # Arm may precede goto. Only this request's actual sampled interval
            # can certify it; an earlier climb to the same goal is not evidence.
            if not record['before']['time'] <= sample['time'] <= record['after']['time']:
                continue
            require(all(sample[key] > 0 for key in ('generation', 'bot_slot', 'bot_userid', 'bot_serial',
                                                   'human_slot', 'human_userid', 'human_serial')) and
                    sample['human_slot'] < sample['bot_slot'], 'probe identity/serial is unknown')
            require(sample['bot_slot'] == metadata['bot_slot'] and sample['bot_userid'] == record['before']['userid'] and
                    sample['human_slot'] == metadata['human_slot'] and
                    sample['bot_fake'] == sample['bot_alive'] == sample['human_alive'] == 1 and sample['human_fake'] == 0 and
                    sample['team'] == record['before']['team'], 'probe player/team binding mismatch')
            bound = tuple(sample[key] for key in ('epoch', 'generation', 'map_serial', 'bot_slot', 'bot_userid',
                                                 'bot_serial', 'human_slot', 'human_userid', 'human_serial', 'team'))
            if identity is None:
                identity = bound
            require(identity == bound, 'mixed ladder observation generations/identities')
            if observed:
                previous = observed[-1]
                require(sample['tick'] > previous['tick'] and sample['sample'] == previous['sample'] + 1 and
                        0 < sample['time'] - previous['time'] <= 0.2,
                        'ladder samples are not a continuous ordered tick window')
            observed.append(sample)
        require(len(observed) >= 3, 'actual continuous ladder samples missing')
        # A lifecycle snapshot binds the samples to a real active task token.
        # All active snapshots inside this observation must retain that token.
        owners, owner_times = set(), []
        for match in CORE.finditer(text):
            values = dict(FIELDS.findall('edict=' + match[2]))
            stamp = float(values['time'])
            if not observed[0]['time'] <= stamp <= observed[-1]['time']:
                continue
            require(match[1] == metadata['bot_name'] and int(values['userid']) == observed[0]['bot_userid'] and
                    values['map'] == plan['map'], 'core ladder owner identity mismatch')
            if int(values['route']) > 0:
                require(int(values['goal']) == route[-1] and int(values['owner']) > 0,
                        'ladder active route goal/owner changed')
                owners.add(int(values['owner']))
                owner_times.append(stamp)
        require(len(owners) == 1, 'missing or changing active route ownership snapshot')
        ladder_offsets = [i for i, index in enumerate(route) if plan['nodes'][index]['flags'] & WP_LADDER]
        require(len(ladder_offsets) >= 2, 'ladder route must contain both ladder endpoints')
        cursor, visits = 0, []
        for offset in ladder_offsets:
            node = plan['nodes'][route[offset]]
            visit = next((j for j in range(cursor, len(observed))
                          if observed[j]['movetype'] == MOVETYPE_LADDER and
                          math.hypot(observed[j]['position'][0] - node['x'], observed[j]['position'][1] - node['y']) <= max(node['radius'], 24) and
                          abs(observed[j]['position'][2] - node['z']) <= 32), None)
            require(visit is not None, 'missing actual ladder visit to #%d' % route[offset])
            visits.append((offset, visit))
            cursor = visit + 1
        progresses = []
        for (a, start), (b, end) in zip(visits, visits[1:]):
            if b != a + 1:
                continue
            require(all(sample['movetype'] == MOVETYPE_LADDER for sample in observed[start:end + 1]),
                    'lower/upper ladder traversal left actual MOVETYPE_LADDER between endpoints')
            dz = plan['nodes'][route[b]]['z'] - plan['nodes'][route[a]]['z']
            actual = observed[end]['position'][2] - observed[start]['position'][2]
            require(abs(dz) >= 8 and actual * (1 if dz > 0 else -1) >= max(8, abs(dz) - 64),
                    'actual ladder height/direction progress missing')
            progresses.append({'from': route[a], 'to': route[b], 'planned_dz': dz, 'observed_dz': actual})
        require(progresses, 'no consecutive lower/upper ladder pair with height progress')
        require(any(abs(stamp - observed[visit]['time']) <= 1.5
                    for stamp in owner_times for _, visit in visits),
                'active route ownership was not sampled near the actual ladder traversal')
        exit_sample = next((sample for sample in observed[visits[-1][1] + 1:]
                            if sample['movetype'] != MOVETYPE_LADDER and sample['ground_known'] == 1 and
                            sample['ground_ent'] >= 0 and sample['flags'] & 1), None)
        require(exit_sample is not None, 'actual dismount and ground handle/flag confirmation missing')
        require(exit_sample['time'] <= record['after']['time'], 'dismount is later than endpoint window')
        result.update(accepted=True, ladder_node_ids=[route[i] for i in ladder_offsets],
                      samples=len(observed), owner=next(iter(owners)), height_progress=progresses,
                      dismount_time=exit_sample['time'], node_visit_tolerance={'xy_floor': 24, 'z': 32})
    except (ValueError, TypeError, KeyError, IndexError) as exc:
        result['errors'].append(str(exc))
    return result


def coverage_incremental(original, candidate, manifest, plan, plan_hash, sources,
                         assets, jump_probe_hash=None, allow_author_change=False):
    sources = list(sources)
    delta = compare_incremental(original, candidate, manifest, allow_author_change)
    decoded = audit.decode_agpw(candidate)
    checked = audit.coverage(plan, plan_hash, sources, assets=assets)
    new_nodes = set(delta['new_node_ids'])
    new_edges = {(row['from'], row['to']) for row in delta['new_directed_edges']}
    normal_nodes = {index for index in new_nodes if not decoded['nodes'][index]['flags'] & WP_LADDER}
    ladder_nodes = new_nodes - normal_nodes
    accepted_nodes, accepted_edges, endpoint_nodes = set(), set(), set()
    stops = {(row['file'], row['record']): row for row in checked['stop_measurements']}
    source_map = {str(name): document for name, document in sources}
    records = []
    for row in checked['records']:
        name, ordinal = row['file'], row['record']
        document = source_map[name]
        record, metadata = document['records'][ordinal], document['metadata']
        route = row.get('route', [])
        edges = set(zip(route, route[1:]))
        relevant = bool(new_nodes.intersection(route) or new_edges.intersection(edges))
        if not relevant:
            continue
        errors = list(row['errors'])
        if not stops.get((name, ordinal), {}).get('precise_stop_passed'):
            errors.append('incremental route lacks strict ordinary endpoint stop')
        ladder = None
        if any(plan['nodes'][index]['flags'] & WP_LADDER for index in route):
            ladder = ladder_evidence(record, metadata, plan, route, jump_probe_hash)
            errors.extend(ladder['errors'])
        if not errors:
            accepted_edges.update(edges & new_edges)
            endpoint_nodes.add(route[-1])
            if ladder:
                accepted_nodes.update(set(ladder['ladder_node_ids']) & ladder_nodes)
        records.append({'file': name, 'record': ordinal, 'accepted': not errors, 'route': route,
                        'new_edges': sorted(edges & new_edges), 'ladder': ladder, 'errors': errors})
    missing_normal = sorted(normal_nodes - endpoint_nodes)
    missing_ladder = sorted(ladder_nodes - accepted_nodes)
    missing_edges = sorted(new_edges - accepted_edges)
    errors = list(delta['errors'])
    if missing_normal or missing_ladder or missing_edges:
        errors.append('new nodes or new directed edges lack current-session acceptance')
    return {'audit': 'incremental_coverage', 'accepted': not errors,
            'scope': 'new_nodes_and_new_directed_edges_only', 'delta': delta,
            'new_normal_node_ids': sorted(normal_nodes), 'new_ladder_node_ids': sorted(ladder_nodes),
            'precise_new_normal_endpoint_ids': sorted(normal_nodes & endpoint_nodes),
            'accepted_new_ladder_node_ids': sorted(accepted_nodes),
            'accepted_new_directed_edges': sorted(accepted_edges),
            'missing_new_normal_endpoint_ids': missing_normal, 'missing_new_ladder_node_ids': missing_ladder,
            'missing_new_directed_edges': missing_edges, 'records': records, 'errors': errors,
            'binding_files': checked['files'],
            'note': 'Frozen baseline nodes and edges are excluded from this round. This does not certify spatial map coverage.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('compare', 'coverage'))
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--allowed-additions', type=Path, required=True)
    parser.add_argument('--allow-author-change', action='store_true')
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--records', type=Path, nargs='+')
    parser.add_argument('--bsp', type=Path)
    parser.add_argument('--dll', type=Path)
    parser.add_argument('--jump-probe', type=Path)
    args = parser.parse_args()
    try:
        original, candidate = args.original.read_bytes(), args.candidate.read_bytes()
        manifest = audit.read_json(args.allowed_additions)
        if args.command == 'compare':
            result = compare_incremental(original, candidate, manifest, args.allow_author_change)
        else:
            require(args.plan and args.records and args.bsp and args.dll,
                    'coverage requires --plan --records --bsp --dll')
            plan_data = args.plan.read_bytes()
            sources = [(str(path), audit.read_json(path)) for path in args.records]
            result = coverage_incremental(original, candidate, manifest, json.loads(plan_data.decode('utf-8-sig')),
                                          audit.digest(plan_data), sources,
                                          {'agpw': args.candidate, 'bsp': args.bsp, 'dll': args.dll},
                                          audit.digest(args.jump_probe.read_bytes()) if args.jump_probe else None,
                                          args.allow_author_change)
    except (OSError, ValueError, TypeError) as exc:
        parser.exit(2, 'error: ' + str(exc) + '\n')
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return 0 if result['accepted'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
