#!/usr/bin/env python3
"""Optional render-CLI invariance probe; requires an unmodified baseline binary."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--baseline', required=True)
parser.add_argument('--render', required=True)
parser.add_argument('--input', required=True, help='directory containing just stress_dense_hats.wav')
parser.add_argument('--work', required=True)
parser.add_argument('--reuse', action='store_true')
args = parser.parse_args()
runs = {}
for name, exe, extra in (
        ('baseline', args.baseline, []), ('default', args.render, []),
        ('unaligned', args.render, ['--no-align']),
        ('override10', args.render, ['--no-align', '--streaming-target-lufs', '-10']),
        ('override14', args.render, ['--no-align', '--streaming-target-lufs', '-14'])):
    dest = Path(args.work + '-' + name)
    if not args.reuse:
        subprocess.run([exe, '--in', args.input, '--out', str(dest), '--glaze', '100',
                        '--delivery', 'streaming', '--no-replay'] + extra,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
    path = next(dest.glob('*.wav'))
    with (dest / 'metrics.csv').open() as stream:
        row = next(r for r in csv.DictReader(stream) if r['kind'] == 'render')
    runs[name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                  **{k: row[k] for k in ('lufs', 'align_gain_db', 'effective_ceiling_dbtp',
                                         'plan_requested_target_lufs', 'plan_effective_target_lufs')}}
assert runs['baseline']['sha256'] == runs['default']['sha256']
assert runs['unaligned']['sha256'] == runs['override10']['sha256']
assert runs['unaligned']['align_gain_db'] == ''
assert runs['override14']['effective_ceiling_dbtp'] == runs['unaligned']['effective_ceiling_dbtp']
assert abs(float(runs['override14']['plan_requested_target_lufs'])
           - float(runs['unaligned']['plan_requested_target_lufs']) + 4) < .01
assert runs['override14']['sha256'] != runs['unaligned']['sha256']
print(json.dumps(runs, indent=2))
