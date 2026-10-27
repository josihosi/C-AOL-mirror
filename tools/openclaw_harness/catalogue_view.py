"""Presentation of explicit scenario catalogue metadata; never selection authority."""
from __future__ import annotations

from typing import Any, Mapping, Sequence
import json
import sqlite3
from pathlib import Path


def attach_registry_status(rows: Sequence[dict[str, Any]], registry: str | None = None) -> None:
    """Attach current source-bound registry facts without creating or changing an index."""
    from scenario_registry_store import open_registry, build_registry_query_candidate_snapshot, ScenarioRegistryStoreError

    unknown = {'lifecycle': 'unknown', 'token_eligible': None,
               'availability': 'Registry status unknown: index unavailable'}
    for row in rows:
        row['registry_status'] = dict(unknown)
    ids = tuple(row['scenario_id'] for row in rows if row.get('scenario_id'))
    if not ids:
        return
    try:
        connection = open_registry(registry, writable=False)
        try:
            snapshots = build_registry_query_candidate_snapshot(
                connection, include_lifecycle_states=('absent', 'quarantined', 'retired'), manifest_ids=ids)
        finally:
            connection.close()
    except (OSError, sqlite3.Error, ValueError, ScenarioRegistryStoreError):
        return
    by_id = {snapshot.scenario_id: snapshot for snapshot in snapshots}
    for row in rows:
        snapshot = by_id.get(row.get('scenario_id'))
        if snapshot is None:
            row['registry_status']['availability'] = 'Registry status unknown: scenario not in registry'
            continue
        manifest = snapshot.explanation.get('manifest', {})
        source = row.get('scenario_manifest', {}).get('source', {})
        if not source.get('sha256') or not manifest.get('present') or \
                manifest.get('source_path') != source.get('path') or manifest.get('sha256') != source.get('sha256'):
            row['registry_status']['availability'] = 'Registry status unknown: source not current in registry'
            continue
        lifecycle = snapshot.lifecycle_state
        eligible = bool(snapshot.token_eligible)
        availability = ('Unavailable: ' + lifecycle if lifecycle != 'active' else
                        'Registry eligible; qualification unknown' if eligible else 'Unavailable: not eligible')
        row['registry_status'] = {'lifecycle': lifecycle, 'token_eligible': eligible, 'availability': availability}


def recipe_families(root: Path) -> list[dict[str, Any]]:
    """Read family-only declarations from the owning recipes, not a second index."""
    marker = '<!-- caol-catalogue-family: '
    result = []
    if not root.is_dir():
        return result
    for path in sorted(root.glob('*.md')):
        with path.open(encoding='utf-8') as stream:
            for _, line in zip(range(12), stream):
                if not line.startswith(marker):
                    continue
                if not line.rstrip().endswith(' -->'):
                    raise ValueError('invalid recipe family metadata: ' + str(path))
                value = json.loads(line[len(marker):].rstrip()[:-4])
                fields = ('family_id', 'title', 'purpose', 'kind', 'starting_save')
                if not isinstance(value, dict) or any(not isinstance(value.get(k), str) or not value[k].strip() for k in fields) or value['kind'] != 'maintained':
                    raise ValueError('invalid recipe family metadata: ' + str(path))
                result.append({'id': value['family_id'], 'title': value['title'], 'purpose': value['purpose'],
                               'kind': value['kind'], 'starting_save': value['starting_save'],
                               'recipe': str(path), 'variants': [], 'variant_count': 0})
    return result


def family_view(rows: Sequence[Mapping[str, Any]], *, kind: str = 'maintained',
                family: str = '', search: str = '', recipes: Sequence[Mapping[str, Any]] = ()) -> dict[str, Any]:
    """Group source-bound rows without inferring membership or qualification."""
    if kind not in ('maintained', 'diagnostic', 'all'):
        raise ValueError('invalid catalogue view')
    families: dict[str, dict[str, Any]] = {}
    others = []
    query = search.strip().casefold()
    for source in rows:
        row = dict(source)
        metadata = row.get('catalogue')
        membership = metadata.get('kind', 'unknown') if isinstance(metadata, dict) else 'unknown'
        if membership not in ('maintained', 'diagnostic', 'historical', 'continuation'):
            membership = 'unknown'
        if membership in ('maintained', 'diagnostic') and any(
                not isinstance(metadata.get(k), str) or not metadata[k].strip()
                for k in ('family_id', 'family_title', 'family_purpose', 'variant')):
            membership = 'unknown'
        if kind != 'all' and membership != kind:
            continue
        if query and query not in ' '.join(str(row.get(k, '')) for k in
                ('name', 'description', 'scenario_id')).casefold() and not (
                isinstance(metadata, dict) and query in ' '.join(str(v) for v in metadata.values()).casefold()):
            continue
        if membership not in ('maintained', 'diagnostic'):
            if not family:
                row['membership'] = membership
                others.append(row)
            continue
        fid = metadata['family_id']
        if family and fid != family:
            continue
        descriptor = (metadata['family_title'], metadata['family_purpose'], membership)
        entry = families.setdefault(fid, {'id': fid, 'title': descriptor[0], 'purpose': descriptor[1],
                                          'kind': membership, 'variants': []})
        if (entry['title'], entry['purpose'], entry['kind']) != descriptor:
            raise ValueError('inconsistent catalogue family: ' + fid)
        row['membership'] = membership
        row['variant'] = metadata['variant']
        row.pop('catalogue', None)
        binding = row.get('scenario_manifest')
        if isinstance(binding, dict):
            row['scenario_manifest'] = {key: binding[key] for key in ('source', 'validation') if key in binding}
        if 'status' in row:
            row['declared_status'] = row.pop('status')
        entry['variants'].append(row)
    for recipe in recipes:
        fid = recipe['id']
        if fid in families:
            raise ValueError('duplicate recipe and scenario family: ' + fid)
        if kind == 'diagnostic' or (family and family != fid):
            continue
        if query and query not in ' '.join(str(recipe.get(k, '')) for k in ('id', 'title', 'purpose', 'starting_save')).casefold():
            continue
        families[fid] = dict(recipe)
    result = []
    for key in sorted(families):
        entry = families[key]
        entry['variant_count'] = len(entry['variants'])
        if not family and not query:
            entry['variants'] = [{**{k: row[k] for k in ('scenario_id', 'name', 'variant')},
                                  'qualification': 'unknown', 'registry_status': row.get('registry_status',
                                  {'lifecycle': 'unknown', 'token_eligible': None, 'availability': 'Registry status unknown'})} for row in entry['variants']]
        result.append(entry)
    return {'families': result, 'other_scenarios': others}
