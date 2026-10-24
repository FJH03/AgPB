"""Read-only, standard-library audits of explicitly authored AGPW evidence.

Exit codes: 0 accepted (possibly explicitly legacy), 1 audit rejected, 2 bad input.
JSON is written only to stdout. No game commands, inferred links or file writes.
"""
import argparse
import hashlib
import json
import math
import re
import struct
from pathlib import Path, PureWindowsPath

from export_manual_plan import HEADER, NODES, MAGIC, export, integer, jump_velocity
import stop_policy

# src/waypoint.h and waypoint.cpp: IsWaypointPassable/GetLinkCost/FindPath.
WP_DJUMP, WP_AVOID = 1 << 9, 1 << 11
WP_T, WP_CT, PATH_DOUBLE = 1 << 29, 1 << 30, 1 << 1
WALK = re.compile(r'^\[AgPB\] (?P<bot>.+?): walking to #(?P<target>\d+), (?P<count>\d+) hop\(s\):(?P<route>(?: +\d+)+)\s*$', re.M)
ARRIVE = re.compile(r'^\[AgPB\] (?P<bot>.+?): arrived at waypoint #(?P<target>\d+) \((?P<count>\d+) hop\(s\) walked\)\s*$', re.M)
FAILURE = re.compile(r'^\[AgPB\] (?P<bot>.+?): .*?(?:route aborted|route stopped|stuck at|obstacle break failed)', re.M)
SHA256 = re.compile(r'^[0-9a-fA-F]{64}$')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def passable(flags, team):
    return not (flags & (WP_DJUMP | WP_AVOID) or
                flags & WP_T and team not in (0, 2) or
                flags & WP_CT and team not in (0, 3))


def graph(plan):
    labels = {node['label']: index for index, node in enumerate(plan['nodes'])}
    return {(labels[e['from']], labels[e['to']]): e.get('flags', 0) for e in plan['directed_edges']}


def reachability(plan):
    """Allowed-node reachability, not physical traversability or spawn inference."""
    edges = graph(plan)
    result = {}
    for team, name in ((2, 'T'), (3, 'CT')):
        allowed = {i for i, n in enumerate(plan['nodes']) if passable(n.get('flags', 0), team)}
        adjacency = {i: [] for i in allowed}
        for (source, target), flags in edges.items():
            if source in allowed and target in allowed and not flags & PATH_DOUBLE:
                adjacency[source].append(target)
        unreachable = []
        for source in sorted(allowed):
            visited, pending = {source}, [source]
            while pending:
                for target in adjacency[pending.pop()]:
                    if target not in visited:
                        visited.add(target)
                        pending.append(target)
            missing = sorted(allowed - visited)
            if missing:
                unreachable.append({'from': source, 'to': missing})
        result[name] = {'team': team, 'allowed_nodes': len(allowed),
                        'blocked_nodes': sorted(set(range(len(plan['nodes']))) - allowed),
                        'strongly_connected': bool(allowed) and not unreachable,
                        'unreachable': unreachable}
    result['semantics'] = ('Matches waypoint.cpp target-node filtering: AVOID/DJUMP blocked; '
                           'T/CT bits restrict team 2/3; PATH_DOUBLE blocked. Checks allowed nodes only. '
                           'FindPath may leave a blocked start and returns start==goal before filtering. '
                           'No spawn meaning is assigned to team flags; geometry is not certified.')
    return result


def decode_agpw(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated AGPW header')
    magic, version, count, map_raw, author_raw = HEADER.unpack_from(data)
    if magic != MAGIC or version not in NODES or not 0 < count <= 8192:
        raise ValueError('invalid AGPW magic/version/node count')
    record = NODES[version]
    if len(data) != HEADER.size + count * record.size:
        raise ValueError('AGPW size does not match node count')
    fields = ['x', 'y', 'z', 'flags', 'radius', 'mesh'] + ['index[%d]' % i for i in range(8)] + ['connectionFlags[%d]' % i for i in range(8)] + ['gravity']
    nodes = []
    for index in range(count):
        row = record.unpack_from(data, HEADER.size + index * record.size)
        node = dict(zip(fields, row[:23]))
        if not all(math.isfinite(node[key]) for key in ('x', 'y', 'z', 'gravity')):
            raise ValueError('non-finite AGPW coordinate/gravity')
        node['jumpVelocity'] = []
        for slot in range(8):
            velocity = jump_velocity(list(row[23 + 3 * slot:26 + 3 * slot]) if version == 2 else [0.0, 0.0, 0.0])
            target = node['index[%d]' % slot]
            if any(component != 0 for component in velocity) and not (0 <= target < count and target != index and node['connectionFlags[%d]' % slot] & 1):
                raise ValueError('nonzero AGPW jumpVelocity requires an existing PATH_JUMP edge')
            node['jumpVelocity'].append(velocity)
        nodes.append(node)
    return {'magic': magic, 'version': version, 'count': count,
            'map_raw': map_raw, 'author_raw': author_raw, 'nodes': nodes}


def compare_agpw(expected_data, data, allow_author_change=False, labels=None):
    """Compare every serialized field; only version and allowed author may differ.

    Float32 bit identity retains the old disk check's signed-zero distinction.
    Missing v1 jumpVelocity is represented by eight XYZ-zero vectors. No padding, link slots,
    unknown flags, mesh or gravity fields are skipped during normalization.
    """
    expected = decode_agpw(expected_data)
    actual = decode_agpw(data)
    differences = []
    for field in ('magic', 'count', 'map_raw'):
        if expected[field] != actual[field]:
            differences.append({'field': field, 'expected': str(expected[field]), 'actual': str(actual[field])})
    author_change = None
    if expected['author_raw'] != actual['author_raw']:
        author_change = {'expected': expected['author_raw'].split(b'\0')[0].decode('ascii', 'replace'),
                         'actual': actual['author_raw'].split(b'\0')[0].decode('ascii', 'replace'),
                         'allowed': allow_author_change}
    for index, (wanted, got) in enumerate(zip(expected['nodes'], actual['nodes'])):
        for field in wanted:
            if field == 'jumpVelocity':
                for slot in range(8):
                    for axis in range(3):
                        before, after = wanted[field][slot][axis], got[field][slot][axis]
                        if struct.pack('<f', before) != struct.pack('<f', after):
                            differences.append({'node': index, 'label': labels[index] if labels else str(index),
                                                'field': 'jumpVelocity[%d][%d]' % (slot, axis),
                                                'expected': before, 'actual': after})
                continue
            same = (struct.pack('<f', wanted[field]) == struct.pack('<f', got[field])
                    if field in ('x', 'y', 'z', 'gravity') else wanted[field] == got[field])
            if not same:
                differences.append({'node': index, 'label': labels[index] if labels else str(index), 'field': field,
                                    'expected': wanted[field], 'actual': got[field]})
    matching = not differences and (author_change is None or allow_author_change)
    return {'fields_match': matching, 'agpw_sha256': digest(data), 'author_change': author_change,
            'differences': differences, 'expected_agpw_version': expected['version'],
            'actual_agpw_version': actual['version']}


def compare(plan, data, allow_author_change=False):
    fields = compare_agpw(export(plan), data, allow_author_change, [node['label'] for node in plan['nodes']])
    teams = reachability(plan)
    connected = all(teams[t]['strongly_connected'] for t in ('T', 'CT'))
    return {'audit': 'compare', 'accepted': fields['fields_match'] and connected, **fields,
            'map': plan['map'], 'nodes': len(plan['nodes']), 'directed_edges': len(plan['directed_edges']),
            'reachability': teams,
            'note': 'All float32 bits and link slots/flags are compared; v1 jumpVelocity defaults to eight XYZ-zero vectors. Header versions are reported separately.'}


def route_indices(value):
    if isinstance(value, str):
        if not re.fullmatch(r'\s*\d+(?:\s+\d+)*\s*', value):
            raise ValueError('route must contain integer indices')
        return [int(part) for part in value.split()]
    if not isinstance(value, list) or not value:
        raise ValueError('route must be a nonempty string or list')
    return [integer(index, 'route index') for index in value]


def normalized_log(value):
    return value.replace('\r\n', '\n').replace('\r', '\n')


def route_log_matches(log, route, target):
    """Pair a walking line with the same bot's next completion, never status alone."""
    events = [(m.start(), 'walk', m) for m in WALK.finditer(log)]
    events += [(m.start(), 'arrive', m) for m in ARRIVE.finditer(log)]
    events += [(m.start(), 'failure', m) for m in FAILURE.finditer(log)]
    pending, matches = {}, []
    for _, kind, event in sorted(events, key=lambda item: item[0]):
        bot = event['bot']
        if kind == 'walk':
            pending[bot] = event
        elif kind == 'failure':
            pending.pop(bot, None)
        else:
            walking = pending.pop(bot, None)
            if walking is not None and int(walking['target']) == int(event['target']) == target:
                logged_route = [int(i) for i in walking['route'].split()]
                if logged_route == route and int(walking['count']) == len(route) and int(event['count']) == len(route) - 1:
                    matches.append({'bot': bot, 'start': walking.start(), 'end': event.end()})
    return matches


def verify_route_log(record, route, target, console_log, expected_bot=None):
    embedded = normalized_log(record.get('log', ''))
    if not embedded.strip():
        return None, 'missing embedded route log'
    matches = route_log_matches(embedded, route, target)
    if len(matches) == 1:
        if expected_bot is not None and matches[0]['bot'] != expected_bot:
            return None, 'route log bot differs from metadata.bot_name'
        return 'embedded', None
    if len(matches) > 1:
        return None, 'ambiguous repeated walking/arrived events'
    if not console_log:
        return None, 'walking/arrived pair missing; truncated legacy logs require --console-log'
    # Recover only a uniquely anchored exact excerpt. A random matching route in
    # a full console log cannot authenticate an empty or unrelated record.
    console_log = normalized_log(console_log)
    offset = console_log.find(embedded)
    if offset < 0 or console_log.find(embedded, offset + 1) >= 0:
        return None, 'embedded excerpt has no unique exact match in console log'
    end = offset + len(embedded)
    candidates = [m for m in route_log_matches(console_log[:end], route, target)
                  if m['end'] > offset and m['end'] <= end]
    if len(candidates) != 1:
        return None, 'no unique walking/arrived pair bound to console excerpt'
    if expected_bot is not None and candidates[0]['bot'] != expected_bot:
        return None, 'route log bot differs from metadata.bot_name'
    return 'console-log-excerpt', None


def finite_vector(value):
    return (isinstance(value, list) and len(value) == 3 and
            all(isinstance(v, (float, int)) and not isinstance(v, bool) and math.isfinite(v) for v in value))


def coverage(plan, plan_sha256, sources, console_log=None, allow_legacy=False, assets=None):
    """sources is [(display_name, decoded JSON), ...]; no filesystem mutation."""
    export(plan)
    edges = graph(plan)
    # Materialize once: version conflicts must invalidate earlier source files too.
    sources = list(sources)
    asset_kinds = ('agpw', 'bsp', 'dll')
    observed_assets = {kind: set() for kind in asset_kinds}
    for _, payload in sources:
        metadata = payload.get('metadata', {}) if isinstance(payload, dict) else {}
        if isinstance(metadata, dict):
            for kind in asset_kinds:
                claimed = metadata.get(kind + '_sha256')
                if isinstance(claimed, str) and SHA256.fullmatch(claimed):
                    observed_assets[kind].add(claimed.lower())
    version_conflicts = [kind for kind, hashes in observed_assets.items() if len(hashes) > 1]
    expected_assets, current_assets, current_asset_errors = {}, [], []
    for kind, path in (assets or {}).items():
        if kind not in asset_kinds:
            raise ValueError('unknown current asset kind: ' + kind)
        raw = Path(path).read_bytes()
        expected_assets[kind] = digest(raw)
        current_assets.append({'kind': kind, 'path': str(path), 'sha256': expected_assets[kind]})
        if kind == 'agpw':
            explicit = export(plan)
            try:
                matching = compare_agpw(explicit, raw, allow_author_change=True)['fields_match']
            except ValueError:
                matching = False
            if not matching:
                current_asset_errors.append('current AGPW data/map differs from explicit plan')
    explicit_asset_binding = bool(assets)
    asset_legacy_seen = False
    missing_assets_seen = False
    requested_endpoints = set()
    covered_nodes, covered_edges, unknown_nodes, unknown_edges = set(), set(), set(), set()
    files, records, failures, stop_errors = [], [], [], []
    legacy_seen = False
    for filename, payload in sources:
        metadata = payload.get('metadata', {}) if isinstance(payload, dict) else {}
        items = payload.get('records') if isinstance(payload, dict) else payload
        if not isinstance(items, list) or not isinstance(metadata, dict):
            raise ValueError(filename + ': expected records list or wrapper with metadata and records')
        file_errors = list(current_asset_errors)
        file_errors.extend(kind + '_sha256 contradicts other source metadata (mixed asset versions)' for kind in version_conflicts)
        missing_asset_hashes = []
        for kind in asset_kinds:
            claimed = metadata.get(kind + '_sha256')
            if claimed is None:
                missing_asset_hashes.append(kind)
            elif not isinstance(claimed, str) or not SHA256.fullmatch(claimed):
                file_errors.append('invalid ' + kind + '_sha256')
            elif kind in expected_assets and claimed.lower() != expected_assets[kind]:
                file_errors.append(kind + '_sha256 differs from current asset')
        missing_assets_seen |= bool(missing_asset_hashes)
        asset_legacy = explicit_asset_binding and bool(missing_asset_hashes)
        asset_legacy_seen |= asset_legacy
        if asset_legacy and not allow_legacy:
            file_errors.append('missing asset hashes in explicit binding mode: ' + ', '.join(missing_asset_hashes))
        if isinstance(payload, dict) and payload.get('schema_version') != 1:
            file_errors.append('unsupported or missing schema_version')
        claimed_hash, claimed_version = metadata.get('plan_sha256'), metadata.get('plan_version')
        legacy = claimed_hash is None or claimed_version is None or asset_legacy
        legacy_seen |= legacy
        if claimed_hash is not None and claimed_hash != plan_sha256:
            file_errors.append('plan_sha256 mismatch (stale evidence)')
        if claimed_version is not None and (type(claimed_version) is not int or claimed_version != plan.get('plan_version', 1)):
            file_errors.append('plan_version mismatch')
        if metadata.get('map', plan['map']) != plan['map']:
            file_errors.append('map mismatch')
        if (claimed_hash is None or claimed_version is None) and not allow_legacy:
            file_errors.append('missing plan hash/version; use --allow-legacy only for a downgraded historical audit')
        files.append({'file': filename, 'records': len(items), 'binding': 'legacy_unbound' if legacy else 'plan_hash_and_version',
                      'asset_binding': 'legacy_assets_unbound' if asset_legacy else ('missing_hashes' if missing_asset_hashes else 'consistent_metadata'),
                      'asset_sha256': {kind: metadata.get(kind + '_sha256') for kind in asset_kinds},
                      'missing_asset_hashes': missing_asset_hashes, 'errors': file_errors})
        for ordinal, record in enumerate(items):
            if not isinstance(record, dict):
                raise ValueError(filename + ': record must be an object')
            result = {'file': filename, 'record': ordinal, 'target': record.get('target'), 'errors': list(file_errors)}
            records.append(result)
            stop = None
            if record.get('status') != 'arrived':
                result['errors'].append('status is not arrived: ' + str(record.get('status')))
                failures.append({'file': filename, 'record': ordinal, 'status': record.get('status')})
            try:
                route = route_indices(record.get('route'))
                target = integer(record.get('index'), 'target index')
                if 0 <= target < len(plan['nodes']):
                    requested_endpoints.add(target)
                result['route'] = route
                bad_nodes = {i for i in route + [target] if not 0 <= i < len(plan['nodes'])}
                bad_edges = set(zip(route, route[1:])) - edges.keys()
                unknown_nodes.update(bad_nodes)
                unknown_edges.update(bad_edges)
                if bad_nodes or bad_edges:
                    result['errors'].append('route contains unknown nodes or directed edges')
                if not route or route[-1] != target:
                    result['errors'].append('route endpoint differs from target index')
                if 0 <= target < len(plan['nodes']) and record.get('target') != plan['nodes'][target]['label']:
                    result['errors'].append('target label/index mismatch')
                declared_team = metadata.get('team')
                declared_team = {'T': 2, 'CT': 3}.get(declared_team, declared_team) if isinstance(declared_team, (str, int)) else declared_team
                team = record.get('before', {}).get('team', declared_team)
                if team not in (2, 3):
                    result['errors'].append('missing or invalid actual team')
                elif not bad_nodes:
                    if (declared_team is not None and declared_team != team) or record.get('after', {}).get('team', team) != team:
                        result['errors'].append('actual team changed or differs from metadata')
                    if any(not passable(plan['nodes'][i].get('flags', 0), team) for i in route[1:]):
                        result['errors'].append('route enters a node blocked for the actual team')
                if any(edges.get(pair, 0) & PATH_DOUBLE for pair in zip(route, route[1:])):
                    result['errors'].append('route uses blocked PATH_DOUBLE edge')
                log_source, log_error = verify_route_log(record, route, target, console_log, metadata.get('bot_name'))
                result['log_evidence'] = log_source
                if log_error:
                    result['errors'].append(log_error)
                after = record.get('after', {})
                if 0 <= target < len(plan['nodes']) and finite_vector(after.get('position')):
                    node = plan['nodes'][target]
                    distance = math.dist(after['position'], [node[key] for key in ('x', 'y', 'z')])
                    speed = math.sqrt(sum(v * v for v in after['velocity'])) if finite_vector(after.get('velocity')) else None
                    stop = {'file': filename, 'record': ordinal, 'target_id': target, 'target': plan['nodes'][target]['label'], 'distance': round(distance, 4),
                            'radius': node['radius'], 'outside_radius': distance > node['radius'],
                            'radius_semantics': 'legacy_3d_raw_radius_diagnostic_only',
                            'sample_speed': speed, 'driver_settled': record.get('settled'),
                            'driver_stop_status': record.get('stop_status'), 'driver_within_radius': record.get('within_radius'),
                            'policy_classification': 'legacy_unversioned_policy', 'precise_stop_passed': False}
                    policy = metadata.get('stop_policy')
                    version = record.get('stop_policy_version')
                    if policy is not None and version is not None:
                        if policy != stop_policy.description() or type(version) is not int or version != stop_policy.VERSION:
                            stop.update(policy_classification='unsupported_policy', precision_reasons=['unsupported or mismatched stop policy'])
                        elif legacy:
                            stop.update(policy_classification='legacy_unbound', precision_reasons=['stop evidence is not bound to the current plan'])
                        else:
                            measured_stop = stop_policy.evaluate(node, record.get('stop_samples'), after)
                            stop.update(measured_stop)
                            stop.update(policy_classification='policy_v1', radius_semantics='xy_effective_radius_with_independent_vertical_bound',
                                        outside_radius=not measured_stop['within_radius'], outside_vertical=not measured_stop['within_vertical'])
                            for field in ('raw_radius', 'effective_radius', 'xy_distance', 'vertical_error'):
                                value, expected = record.get(field), measured_stop[field]
                                if not stop_policy.finite_number(value) or not stop_policy.finite_number(expected) or abs(value - expected) > 0.05:
                                    stop['precise_stop_passed'] = False
                                    stop['precision_reasons'].append(field + ' missing or inconsistent with endpoint samples')
                            if metadata.get('bot_slot') != after.get('slot') or record.get('before', {}).get('userid') != after.get('userid'):
                                stop['precise_stop_passed'] = False
                                stop['precision_reasons'].append('endpoint bot identity does not match metadata/before')
                            if record.get('settled') is not measured_stop['settled'] or record.get('precise_stop_passed') is not measured_stop['precise_stop_passed']:
                                stop['precise_stop_passed'] = False
                                stop['precision_reasons'].append('driver precision claims disagree with recomputed endpoint window')
                    stop_errors.append(stop)
                    measured = record.get('stopped_distance')
                    if measured is not None and (isinstance(measured, bool) or not isinstance(measured, (int, float)) or
                                                 not math.isfinite(measured) or abs(measured - distance) > 0.05):
                        result['errors'].append('stopped_distance disagrees with recorded position and current plan')
                if record.get('target_radius', plan['nodes'][target]['radius'] if 0 <= target < len(plan['nodes']) else None) != (plan['nodes'][target]['radius'] if 0 <= target < len(plan['nodes']) else None):
                    result['errors'].append('target_radius differs from current plan')
                if not result['errors']:
                    covered_nodes.update(route)
                    covered_edges.update(zip(route, route[1:]))
            except (ValueError, TypeError, AttributeError) as exc:
                result['errors'].append(str(exc))
            result['accepted'] = not result['errors']
            if stop is not None:
                stop['route_accepted'] = result['accepted']
                if not result['accepted']:
                    stop['precise_stop_passed'] = False
                    stop.setdefault('precision_reasons', []).append('route evidence was rejected')
    missing_nodes = sorted(set(range(len(plan['nodes']))) - covered_nodes)
    missing_edges = sorted(edges.keys() - covered_edges)
    rejected = sum(not record['accepted'] for record in records)
    accepted = bool(records) and not rejected and not missing_nodes and not missing_edges
    precise_endpoints = {row['target_id'] for row in stop_errors if row['precise_stop_passed']}
    all_endpoints = set(range(len(plan['nodes'])))
    endpoint_summary = {
        'requested_endpoint_ids': sorted(requested_endpoints), 'unique_requested_endpoints': len(requested_endpoints),
        'precise_endpoint_ids': sorted(precise_endpoints), 'unique_precise_endpoints': len(precise_endpoints),
        'requested_missing_precise_endpoint_ids': sorted(requested_endpoints - precise_endpoints),
        'unrequested_endpoint_ids': sorted(all_endpoints - requested_endpoints),
        'missing_plan_precise_endpoint_ids': sorted(all_endpoints - precise_endpoints),
        'all_plan_endpoints_precisely_verified': bool(all_endpoints) and precise_endpoints == all_endpoints}
    return {'audit': 'coverage', 'accepted': accepted, 'classification': 'legacy_unbound' if legacy_seen else 'plan_bound',
            'asset_binding': ('legacy_assets_unbound' if asset_legacy_seen else 'current_assets_bound' if explicit_asset_binding else
                              'metadata_incomplete' if missing_assets_seen else 'consistent_metadata_only'),
            'asset_version_conflicts': version_conflicts,
            'observed_asset_sha256': {kind: sorted(hashes) for kind, hashes in observed_assets.items()},
            'current_assets': current_assets, 'unchecked_current_assets': sorted(set(asset_kinds) - expected_assets.keys()),
            'endpoint_coverage': endpoint_summary,
            'current_plan_sha256': plan_sha256, 'plan_version': plan.get('plan_version', 1), 'map': plan['map'],
            'total_nodes': len(plan['nodes']), 'total_directed_edges': len(edges),
            'covered_nodes': len(covered_nodes), 'covered_directed_edges': len(covered_edges),
            'missing_nodes': missing_nodes, 'missing_directed_edges': missing_edges,
            'unknown_nodes': sorted(unknown_nodes), 'unknown_directed_edges': sorted(unknown_edges),
            'accepted_records': len(records) - rejected, 'rejected_records': rejected,
            'failed_records': failures, 'files': files, 'records': records,
            'stop_measurements': stop_errors,
            'max_stop_distance': max((r['distance'] for r in stop_errors), default=None),
            'outside_radius_count': sum(r['outside_radius'] for r in stop_errors),
            'outside_effective_xy_radius_count': sum(r['policy_classification'] == 'policy_v1' and r['outside_radius'] for r in stop_errors),
            'outside_vertical_tolerance_count': sum(r.get('outside_vertical', False) for r in stop_errors),
            'legacy_outside_raw_radius_count': sum(r['policy_classification'] != 'policy_v1' and r['outside_radius'] for r in stop_errors),
            'missing_stop_measurements': len(records) - len(stop_errors),
            'stop_policy': stop_policy.description(),
            'precise_stop_certified': bool(records) and len(stop_errors) == len(records) and all(r['precise_stop_passed'] for r in stop_errors),
            'precise_stop_summary': {
                'scope': 'sampled_requested_endpoints_only',
                'passed_endpoints': sum(r['precise_stop_passed'] for r in stop_errors),
                'policy_v1_endpoints': sum(r['policy_classification'] == 'policy_v1' for r in stop_errors),
                'legacy_or_unsupported_endpoints': sum(r['policy_classification'] != 'policy_v1' for r in stop_errors),
                'total_records': len(records), **endpoint_summary},
            'limitations': ['Walking/arrived events establish traversal; intermediate route nodes do not require stopping.',
                           'Precision applies only to versioned, final continuous endpoint sample windows; old records are not re-certified.',
                           'Legacy outside-radius counts retain their old 3D/raw-radius diagnostic meaning, not the new policy.',
                           'Logs and JSON are not cryptographically authenticated.',
                           'Current asset identity is checked only for --agpw/--bsp/--dll files supplied to this audit.',
                           'Legacy records without a plan hash/version cannot certify which plan was sampled.']}


def contained_path(root, relative):
    if not isinstance(relative, str) or not relative or ':' in relative or PureWindowsPath(relative).is_absolute():
        raise ValueError('unsafe manifest path: ' + str(relative))
    parts = relative.replace('\\', '/').split('/')
    if any(part in ('', '.', '..') for part in parts):
        raise ValueError('unsafe manifest path: ' + relative)
    root = Path(root).resolve()
    path = root.joinpath(*parts).resolve()
    if not path.is_relative_to(root):
        raise ValueError('manifest path escapes bundle: ' + relative)
    return path


def verify_bundle(manifest_path, assets=None):
    manifest_path = Path(manifest_path)
    manifest = read_json(manifest_path)
    if not isinstance(manifest.get('files'), dict) or not manifest['files']:
        raise ValueError('manifest.files must be a nonempty path-to-SHA256 object')
    checked, errors, seen = [], [], set()
    for relative, expected in manifest['files'].items():
        entry = {'path': relative, 'expected': expected}
        try:
            path = contained_path(manifest_path.parent, relative)
            identity = str(path).casefold()
            if identity in seen:
                raise ValueError('duplicate resolved manifest path')
            seen.add(identity)
            if not isinstance(expected, str) or not SHA256.fullmatch(expected):
                raise ValueError('invalid SHA256')
            actual = digest(path.read_bytes())
            entry.update({'actual': actual, 'matches': actual.lower() == expected.lower()})
            if not entry['matches']:
                raise ValueError('SHA256 mismatch')
        except (ValueError, OSError) as exc:
            entry['error'] = str(exc)
            errors.append({'path': relative, 'error': str(exc)})
        checked.append(entry)
    agpw_name = str(manifest.get('map', '')) + '.agpw'
    if manifest.get('agpw_sha256') != manifest['files'].get(agpw_name):
        errors.append({'path': agpw_name, 'error': 'manifest agpw_sha256 differs from files entry'})
    current_assets = []
    for kind, path in (assets or {}).items():
        expected = manifest.get(kind + '_sha256')
        try:
            actual = digest(Path(path).read_bytes())
            matches = isinstance(expected, str) and actual.lower() == expected.lower()
            current_assets.append({'kind': kind, 'path': str(path), 'expected': expected, 'actual': actual, 'matches': matches})
            if not matches:
                errors.append({'path': str(path), 'error': kind + ' current asset hash mismatch'})
        except OSError as exc:
            errors.append({'path': str(path), 'error': str(exc)})
    return {'audit': 'verify-bundle', 'accepted': not errors, 'manifest': str(manifest_path),
            'manifest_sha256': digest(manifest_path.read_bytes()), 'declared_files': len(manifest['files']),
            'verified_files': sum(entry.get('matches', False) for entry in checked), 'files': checked,
            'current_assets': current_assets, 'errors': errors,
            'limitations': ['Hash integrity only; historical success claims and physical traversal require separate audits.',
                           'Unprovided current AGPW/BSP/DLL assets are not checked.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    cmp_parser = commands.add_parser('compare', help='Compare every plan/AGPW field and team-directed reachability')
    cmp_parser.add_argument('plan', type=Path)
    cmp_parser.add_argument('agpw', type=Path)
    cmp_parser.add_argument('--allow-author-change', action='store_true')
    cov = commands.add_parser('coverage', help='Cross-check record routes against walking/arrived events')
    cov.add_argument('plan', type=Path)
    cov.add_argument('records', type=Path, nargs='+')
    cov.add_argument('--console-log', type=Path)
    cov.add_argument('--allow-legacy', action='store_true')
    cov.add_argument('--require-precise-stop', action='store_true', help='Also require certified samples at the requested endpoints')
    for asset in ('agpw', 'bsp', 'dll'):
        cov.add_argument('--' + asset, type=Path, help='Bind record metadata to this current asset; explicit mode requires all three asset hashes')
    bundle = commands.add_parser('verify-bundle', help='Verify manifest path containment and all declared hashes')
    bundle.add_argument('manifest', type=Path)
    for asset in ('agpw', 'bsp', 'dll'):
        bundle.add_argument('--' + asset, type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'compare':
            report = compare(read_json(args.plan), args.agpw.read_bytes(), args.allow_author_change)
        elif args.command == 'coverage':
            report = coverage(read_json(args.plan), digest(args.plan.read_bytes()),
                              [(str(p), read_json(p)) for p in args.records],
                              args.console_log.read_text(encoding='utf-8-sig', errors='replace') if args.console_log else None,
                              args.allow_legacy,
                              {key: getattr(args, key) for key in ('agpw', 'bsp', 'dll') if getattr(args, key)})
            if args.require_precise_stop:
                report['accepted'] = report['accepted'] and report['precise_stop_certified']
        else:
            report = verify_bundle(args.manifest, {key: getattr(args, key) for key in ('agpw', 'bsp', 'dll') if getattr(args, key)})
        print(json.dumps(report, ensure_ascii=True, indent=2, allow_nan=False))
        return 0 if report['accepted'] else 1
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(json.dumps({'accepted': False, 'input_error': str(exc)}))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
