#!/usr/bin/env python3
"""Merge independently measured target batches after all 234 conditions finish."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--target12', type=Path, required=True)
parser.add_argument('--target14', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parent
batches = [(-10, root), (-12, args.target12), (-14, args.target14)]
environments = [json.loads((folder / 'environment.json').read_text()) for _, folder in batches]
inputs = []
for _, folder in batches:
    with (folder / 'input_manifest.csv').open() as stream:
        inputs.append(list(csv.DictReader(stream)))
assert inputs[0] == inputs[1] == inputs[2], 'input manifests differ'
for field in ('render_sha256', 'ffmpeg_version', 'packages', 'pyloudnorm_version'):
    assert environments[0][field] == environments[1][field] == environments[2][field], field
for filename, expected, keys in (
        ('masters.csv', 39, ('file', 'target_lufs')),
        ('codec_roundtrips.csv', 234, ('file', 'target_lufs', 'encoder', 'bitrate_kbps'))):
    all_rows = []
    for target, folder in batches:
        with (folder / filename).open() as stream:
            rows = [r for r in csv.DictReader(stream) if float(r['target_lufs']) == target]
        all_rows.extend(rows)
    assert len(all_rows) == expected, (filename, len(all_rows))
    assert len({tuple(r[k] for k in keys) for r in all_rows}) == expected, 'duplicate conditions'
    with (root / filename).open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=all_rows[0], lineterminator="\n")
        writer.writeheader()
        writer.writerows(all_rows)
environment = dict(environments[0])
environment['measurement_commands'] = [e['command'] for e in environments]
environment['render_commands'] = []
for data in environments:
    for command in data['render_commands']:
        if command not in environment['render_commands']:
            environment['render_commands'].append(command)
environment['script_sha256'] = hashlib.sha256((root.parents[2] / 'tools/render/codec_tp_check.py').read_bytes()).hexdigest()
environment['source_commit'] = '5a2142572df76ef30bcd66786d68011cba1ffd7e'
environment['linked_packages'] = {'libopus0': '1.5.2-2', 'libmp3lame0': '3.100-6+b3'}
(root / 'environment.json').write_text(json.dumps(environment, indent=2, ensure_ascii=False) + '\n')
print('Merged 39 masters / 234 codec round trips')
