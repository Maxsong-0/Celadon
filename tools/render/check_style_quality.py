#!/usr/bin/env python3
"""等响鼓点与持续和弦切换：通过真实渲染入口检查自动风格的 B4 泵感和 B7b 声像。

用法：python check_style_quality.py <旧版 celadon_render> <待测 celadon_render>
依赖见 requirements.txt。素材和渲染输出在临时目录内生成。
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

import numpy as np
import pyloudnorm
import soundfile as sf

import quality_gate as gate
import quality_metrics as metrics


RATE = 48000


def make_input(directory):
    t = np.arange(2 * RATE, dtype=np.float64) / RATE
    phase = 2.0 * np.pi * t
    beat, half = t % 0.5, t % 0.25
    chord = (0.15 * np.sin(220 * phase) + 0.1 * np.sin(261.5 * phase)
             + 0.09 * np.sin(329.5 * phase) + 0.035 * np.sin(880 * phase))
    side = 0.035 * np.sin(440 * phase) + 0.02 * np.sin(523 * phase)
    pad = np.column_stack((chord + side, chord - side))
    kick = 0.7 * np.exp(-beat / 0.065) * np.sin(2 * np.pi * (55 * beat + 2.2 * (1 - np.exp(-beat / 0.03))))
    hat = 0.07 * np.exp(-half / 0.023) * (np.sin(6573 * phase) + 0.4 * np.sin(9037 * phase))
    drums = 0.58 * pad + (kick + hat)[:, None]
    meter = pyloudnorm.Meter(RATE, filter_class='DeMan')

    def match(signal):
        level = meter.integrated_loudness(signal)
        scaled = np.rint(signal * (32768 * 10 ** ((-18 - level) / 20)))
        if scaled.min() < -32768 or scaled.max() > 32767:
            raise ValueError('合成素材量化时削波')
        return scaled.astype(np.int16)

    pad, drums = match(pad), match(drums)
    # 32 s 建立鼓点参考，16 s 等响 breakdown，最后 16 s 鼓点重入。
    sf.write(directory / 'contrast.wav', np.concatenate([drums] * 16 + [pad] * 8 + [drums] * 8), RATE)
    sf.write(directory / 'steady_pad.wav', np.tile(pad, (16, 1)), RATE)


def measure(renderer, variant, inputs, output, reference):
    subprocess.run([str(renderer), '--in', str(inputs), '--out', str(output), '--candidate', variant,
                    '--glaze', '35', '--clay', 'auto', '--style', 'auto', '--block', '512', '--bpm', '120',
                    '--no-align', '--no-replay', '--trace'], check=True)
    tasks = metrics.collect_tasks([output], [inputs], {}, [reference])
    result = []
    for task in tasks:
        rows, errors = metrics.process_source(task)
        if errors:
            raise metrics.InfraError('\n'.join(errors))
        result.extend(rows)
    if sorted(row['song'] for row in result) != ['contrast', 'steady_pad']:
        raise metrics.InfraError('应测得鼓点切换和持续和弦各一行')
    csv_path = output / 'quality.csv'
    metrics.write_quality_csv(csv_path, result)
    return gate.read_table(csv_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('renderer', type=Path)
    args = parser.parse_args()
    passed = True
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        inputs = root / 'input'
        inputs.mkdir()
        make_input(inputs)
        ref = root / 'baseline'
        baseline = measure(args.baseline.resolve(), 'current', inputs, ref, ref)
        for variant in ('current', 'rhythm-main'):
            rows = measure(args.renderer.resolve(), variant, inputs, root / variant, ref)
            verdicts, _, _, _, _ = gate.evaluate(baseline, rows)
            checks = dict(verdicts)
            for row in rows:
                key = metrics.pair_key_of(row)
                print(f"{variant} {row['song']}  主段评测 {row['evaluation_main_s']} s  "
                      f"B4 {row['pump_db']} dB ({checks[key]['B4']})  "
                      f"B7b {row['side_mid_delta_p95_db']} dB ({checks[key]['B7b']})")
                # 纯持续和弦原本有已知越线；切换编曲须切实回到声像预算内。
                allowed = (gate.PASS,) if row['song'] == 'contrast' else (gate.PASS, gate.KNOWN)
                passed &= all(checks[key][rule] in allowed for rule in ('A1', 'A2', 'B4', 'B7b'))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
