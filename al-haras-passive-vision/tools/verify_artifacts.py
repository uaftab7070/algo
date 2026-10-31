#!/usr/bin/env python3
"""Independent verifier for AL-HARAS passive vision artifacts (AC-012, SEC-004).

Implemented separately from the C++ writer, with the Python standard library only.
It validates session manifests, frame records, annotations, audit logs and report
bundles against schemas/ and re-computes checksums, CRCs, chains and seals.

Usage:
  verify_artifacts.py session SESSION_DIR
  verify_artifacts.py report REPORT_DIR
  verify_artifacts.py audit AUDIT_JSONL
Exit status 0 when every check passes, 1 otherwise. Output is JSON.
"""
import csv
import hashlib
import json
import os
import re
import sys
import zlib

SCHEMAS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'schemas')


def load_schema(name):
    with open(os.path.join(SCHEMAS, name)) as f:
        return json.load(f)


def type_ok(value, expected):
    if isinstance(expected, list):
        return any(type_ok(value, e) for e in expected)
    if expected == 'integer':
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == 'number':
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return {
        'string': isinstance(value, str), 'boolean': isinstance(value, bool), 'null': value is None,
        'object': isinstance(value, dict), 'array': isinstance(value, list),
    }[expected]


def validate(value, schema, root, path='$'):
    """Validates the JSON Schema subset used in schemas/. Returns error strings."""
    errors = []
    if '$ref' in schema:
        target = root
        for part in schema['$ref'].lstrip('#/').split('/'):
            target = target[part]
        return validate(value, target, root, path)
    if 'anyOf' in schema:
        if not any(not validate(value, s, root, path) for s in schema['anyOf']):
            errors.append(f'{path}: matches no allowed alternative')
        return errors
    if 'type' in schema and not type_ok(value, schema['type']):
        return [f'{path}: expected {schema["type"]}, got {type(value).__name__}']
    if 'const' in schema and value != schema['const']:
        errors.append(f'{path}: must equal {schema["const"]!r}')
    if 'enum' in schema and value not in schema['enum']:
        errors.append(f'{path}: {value!r} not in {schema["enum"]}')
    if isinstance(value, str):
        if 'pattern' in schema and not re.search(schema['pattern'], value):
            errors.append(f'{path}: {value!r} does not match {schema["pattern"]}')
        if 'minLength' in schema and len(value) < schema['minLength']:
            errors.append(f'{path}: shorter than {schema["minLength"]}')
        if 'maxLength' in schema and len(value) > schema['maxLength']:
            errors.append(f'{path}: longer than {schema["maxLength"]}')
    if type_ok(value, 'number'):
        if 'minimum' in schema and value < schema['minimum']:
            errors.append(f'{path}: below minimum {schema["minimum"]}')
        if 'maximum' in schema and value > schema['maximum']:
            errors.append(f'{path}: above maximum {schema["maximum"]}')
    if isinstance(value, list):
        if 'minItems' in schema and len(value) < schema['minItems']:
            errors.append(f'{path}: fewer than {schema["minItems"]} items')
        if 'maxItems' in schema and len(value) > schema['maxItems']:
            errors.append(f'{path}: more than {schema["maxItems"]} items')
        if 'items' in schema:
            for i, item in enumerate(value):
                errors += validate(item, schema['items'], root, f'{path}[{i}]')
    if isinstance(value, dict):
        for key in schema.get('required', []):
            if key not in value:
                errors.append(f'{path}: missing required property {key!r}')
        props = schema.get('properties', {})
        extra = schema.get('additionalProperties', True)
        for key, item in value.items():
            if key in props:
                errors += validate(item, props[key], root, f'{path}.{key}')
            elif extra is False:
                errors.append(f'{path}: property {key!r} is not permitted')
            elif isinstance(extra, dict):
                errors += validate(item, extra, root, f'{path}.{key}')
    return errors


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def canonical(obj):
    return json.dumps(obj, separators=(',', ':'), ensure_ascii=False)


def seal_ok(record):
    if not isinstance(record, dict) or list(record)[-1:] != ['record_sha256']:
        return False
    body = {k: v for k, v in record.items() if k != 'record_sha256'}
    return hashlib.sha256(canonical(body).encode()).hexdigest() == record['record_sha256']


class Result:
    def __init__(self):
        self.checks = []

    def add(self, name, ok, detail=''):
        self.checks.append({'name': name, 'ok': bool(ok), 'detail': detail})

    def finish(self):
        ok = all(c['ok'] for c in self.checks)
        print(json.dumps({'ok': ok, 'checks': self.checks}, indent=2))
        return 0 if ok else 1


def check_chain(lines, previous_key, result, label):
    expected = '0' * 64
    for number, line in enumerate(lines, 1):
        try:
            record = json.loads(line)
        except ValueError as error:
            result.add(f'{label}_chain', False, f'line {number}: {error}')
            return None
        if record.get(previous_key) != expected or not seal_ok(record):
            result.add(f'{label}_chain', False, f'line {number} breaks the chain or its seal')
            return None
        expected = hashlib.sha256(line.encode()).hexdigest()
    result.add(f'{label}_chain', True, f'{len(lines)} records chained and sealed')
    return True


def verify_session(directory):
    result = Result()
    manifest = json.load(open(os.path.join(directory, 'session.json')))
    errors = validate(manifest, load_schema('session-manifest.schema.json'), load_schema('session-manifest.schema.json'))
    result.add('manifest_schema', not errors, '; '.join(errors[:5]))
    checksum_path = os.path.join(directory, 'checksums.json')
    if os.path.exists(checksum_path):
        sums = json.load(open(checksum_path))['files']
        bad = [name for name, item in sums.items()
               if sha256_file(os.path.join(directory, name)) != item['sha256'] or os.path.getsize(os.path.join(directory, name)) != item['size']]
        result.add('file_checksums', not bad, 'mismatch: ' + ', '.join(bad) if bad else f'{len(sums)} files match')
    else:
        result.add('file_checksums', False, 'checksums.json missing')
    frame_schema = load_schema('frame-record.schema.json')
    frame_bytes = manifest['recording']['frame_bytes']
    errors, ids, crc_bad = [], [], 0
    with open(os.path.join(directory, 'video.y4m'), 'rb') as video:
        with open(os.path.join(directory, 'frames.jsonl')) as index:
            for number, line in enumerate(index, 1):
                if not line.endswith('\n'):
                    if manifest['status'] != 'interrupted':
                        errors.append(f'line {number}: incomplete')
                    continue
                record = json.loads(line)
                errors += [f'line {number}: {e}' for e in validate(record, frame_schema, frame_schema)]
                ids.append(record['frame_id'])
                video.seek(record['offset'] - 6)
                if video.read(6) != b'FRAME\n' or record['size'] != frame_bytes:
                    errors.append(f'line {number}: payload placement')
                    continue
                payload = video.read(record['size'])
                if record['crc32'] is not None and format(zlib.crc32(payload) & 0xFFFFFFFF, '08x') != record['crc32']:
                    crc_bad += 1
    result.add('frame_records_schema', not errors, '; '.join(errors[:5]) or f'{len(ids)} records')
    result.add('frame_crc32', crc_bad == 0, f'{crc_bad} payload CRC mismatches')
    events = [json.loads(l) for l in open(os.path.join(directory, 'events.jsonl')) if l.endswith('\n')]
    accounted = ids + [e['frame_id'] for e in events if e.get('event') in ('queue_drop', 'rejected_frame')]
    unique = sorted(set(accounted))
    complete = len(unique) == len(accounted) and (not unique or unique[-1] - unique[0] + 1 == len(unique))
    if manifest['status'] == 'interrupted' and len(unique) == len(accounted):
        complete = True
    result.add('frame_accounting', complete, f'{len(accounted)} IDs accounted')
    result.add('ids_strictly_increasing', all(a < b for a, b in zip(ids, ids[1:])), '')
    annotation_path = os.path.join(directory, 'annotations.jsonl')
    if os.path.exists(annotation_path):
        lines = [l.rstrip('\n') for l in open(annotation_path) if l.strip()]
        if check_chain(lines, 'previous_event_sha256', result, 'annotation_log'):
            schema = load_schema('annotation.schema.json')
            current = {}
            for line in lines:
                event = json.loads(line)
                current[event['annotation_id']] = event['annotation']
            errors = []
            for record in current.values():
                errors += validate(record, schema, schema)
                x, y, w, h = record['region_xywh_pixels']
                width, height = record['image_size_pixels']
                if x + w > width or y + h > height or w < 1 or h < 1:
                    errors.append(f'{record["annotation_id"]}: region outside image')
            result.add('annotations_schema', not errors, '; '.join(errors[:5]) or f'{len(current)} annotations')
    return result.finish()


def verify_report(directory):
    result = Result()
    report = json.load(open(os.path.join(directory, 'qa-report.json')))
    schema = load_schema('qa-report.schema.json')
    errors = validate(report, schema, schema)
    result.add('qa_report_schema', not errors, '; '.join(errors[:5]))
    columns = json.load(open(os.path.join(SCHEMAS, 'csv-columns.json')))
    for name, expected in columns.items():
        with open(os.path.join(directory, name), newline='') as f:
            rows = list(csv.reader(f))
        ok = rows and rows[0] == expected and all(len(r) == len(expected) for r in rows[1:])
        result.add(f'csv_{name}', ok, f'{len(rows) - 1} rows' if ok else f'header/row width mismatch in {name}')
    with open(os.path.join(directory, 'frames.csv'), newline='') as f:
        frame_rows = len(list(csv.reader(f))) - 1
    result.add('frames_csv_matches_report', frame_rows == report['deterministic']['frames']['stored'], f'{frame_rows} rows')
    sums = json.load(open(os.path.join(directory, 'checksums.json')))['files']
    bad = [n for n, digest in sums.items() if sha256_file(os.path.join(directory, n)) != digest]
    result.add('bundle_checksums', not bad, ', '.join(bad))
    return result.finish()


def verify_audit(path):
    result = Result()
    lines = [l.rstrip('\n') for l in open(path) if l.strip()]
    check_chain(lines, 'previous_sha256', result, 'audit')
    return result.finish()


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('session', 'report', 'audit'):
        sys.exit(__doc__)
    return {'session': verify_session, 'report': verify_report, 'audit': verify_audit}[sys.argv[1]](sys.argv[2])


if __name__ == '__main__':
    sys.exit(main())
