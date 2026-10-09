#!/usr/bin/env python3
"""Rebuild numeric Markdown tables from T-085 CSV; no audio or DSP operations."""
import csv
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent


def read(name):
    with (ROOT / name).open() as stream:
        rows = list(csv.DictReader(stream))
    for row in rows:
        for key, value in row.items():
            if key not in ('file', 'encoder'):
                row[key] = float(value)
    return rows


def med(rows, key):
    return statistics.median(r[key] for r in rows)


def table(headers, rows):
    out = '| ' + ' | '.join(headers) + ' |\n'
    out += '| ' + ' | '.join('---' for _ in headers) + ' |\n'
    for row in rows:
        out += '| ' + ' | '.join(f'{x:.2f}' if isinstance(x, float) else str(x) for x in row) + ' |\n'
    return out + '\n'


def main():
    masters, codecs = read('masters.csv'), read('codec_roundtrips.csv')
    assert len(masters) == 39 and len(codecs) == 234, (len(masters), len(codecs))
    out = '## 逐曲成品（未经等响；TP 为 8×）\n\n'
    out += table(['曲目', '目标', 'LUFS-I', 'PSR 中位', 'PLR', '峰值因数', 'TP', '最坏编码后 TP'],
                 [(r['file'].split('__')[0], r['target_lufs'], r['lufs_i'], r['psr_median_db'],
                   r['plr_db'], r['crest_db'], r['tp8_dbtp'],
                   max(c['post_tp8_dbtp'] for c in codecs if c['file'] == r['file'] and c['target_lufs'] == r['target_lufs']))
                  for r in masters])
    out += '## 真歌流媒体目标对比（9 首；各指标先逐曲测，再跨曲取中位）\n\n'
    aggregate = []
    normal = []
    for target in (-10, -12, -14):
        m = [r for r in masters if r['target_lufs'] == target and not r['file'].startswith('stress_')]
        c = [r for r in codecs if r['target_lufs'] == target and not r['file'].startswith('stress_')]
        aggregate.append((target, med(m, 'lufs_i'), med(m, 'psr_median_db'), med(m, 'plr_db'), med(m, 'crest_db'),
                          max(r['post_tp8_dbtp'] for r in c),
                          int(sum(r['post_sample_clip_count'] for r in c)),
                          int(sum(r['post_intersample_clip8_count'] for r in c))))
        for platform in ('spotify', 'youtube', 'apple'):
            normal.append((target, platform, med(c, platform + '_attenuation_db'),
                           med(c, platform + '_playback_tp_dbtp'), max(r[platform + '_playback_tp_dbtp'] for r in c)))
    out += table(['目标', 'LUFS-I', 'PSR 中位', 'PLR 中位', '峰值因数中位', '最坏编码 TP', '采样超限数', '8× 样本间超限数'], aggregate)
    out += '## 归一化模型（9 首 × 6 编码；只衰减，不加限幅）\n\n'
    out += table(['目标', '平台模型', '衰减中位 dB', '回放 TP 中位 dBTP', '回放 TP 最大 dBTP'], normal)
    out += '## 各编码最坏过冲（全目标；4× 和 8× 独立取最大）\n\n'
    worst = []
    for encoder, bitrate in (('aac', 256), ('aac', 128), ('libopus', 160), ('libopus', 96), ('libmp3lame', 320), ('libmp3lame', 128)):
        for corpus in ('真歌', '压力'):
            c = [r for r in codecs if r['encoder'] == encoder and r['bitrate_kbps'] == bitrate
                 and r['file'].startswith('stress_') == (corpus == '压力')]
            w = max(c, key=lambda r: r['overshoot8_db'])
            worst.append((encoder, bitrate, corpus, max(r['overshoot4_db'] for r in c), w['overshoot8_db'],
                          w['file'].split('__')[0], w['target_lufs'], w['pre_tp8_dbtp'], w['post_tp8_dbtp'],
                          max(r['post_tp8_dbtp'] for r in c)))
    out += table(['编码器', 'kbps', '语料', '4× 最坏过冲 dB', '8× 最坏过冲 dB', '8× 对应曲', '目标', '编码前 TP', '编码后 TP', '该组最大编码 TP'], worst)
    out += '## 余量计算（全语料、全目标；编码后 −1 dBTP + 0.2 dB 预留）\n\n'
    margin = []
    for encoder, bitrate in (('aac', 256), ('aac', 128), ('libopus', 160), ('libopus', 96), ('libmp3lame', 320), ('libmp3lame', 128)):
        c = [r for r in codecs if r['encoder'] == encoder and r['bitrate_kbps'] == bitrate]
        worst_overshoot = max(max(r['overshoot4_db'], r['overshoot8_db']) for r in c)
        cap = -1.2 - worst_overshoot
        import math
        rounded = math.floor(cap * 2) / 2
        margin.append((encoder, bitrate, worst_overshoot, cap, rounded))
    out += table(['编码器', 'kbps', '最坏过冲（4/8× 最大）', '推导 PCM TP 上限', '向下取 0.5 dB 起点'], margin)
    for corpus in ('真歌', '压力'):
        c = [r for r in codecs if r['file'].startswith('stress_') == (corpus == '压力')]
        print(corpus, 'worst overshoot', max(r['overshoot8_db'] for r in c), 'max post TP', max(r['post_tp8_dbtp'] for r in c),
              'max 4/8 delta', max(abs(r['post_tp8_dbtp'] - r['post_tp4_dbtp']) for r in c))
    (ROOT / 'RESULTS.md').write_text(out.rstrip() + '\n', encoding='utf-8')
    print(table(['目标', 'LUFS-I', 'PSR', 'PLR', 'Crest', 'post TP', 'sample clips', 'ISP clips'], aggregate))


if __name__ == '__main__':
    main()
