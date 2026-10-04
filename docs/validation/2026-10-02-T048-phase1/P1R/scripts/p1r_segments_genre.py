#!/usr/bin/env python3
# T-048 阶段 1 · P1R：同一份二进制、两个曲风的追踪（例如 4/4 型的 p1r-default 与响度型的 p1r-general），只比实时段落账交来的段自己的读数
# （起止、已听、有效时长、段响度、低频占比、实时试算块数、连续时间段号）。B2 把实时段落账的三道相对门与学习脱钩之后，这些读数应与曲风无关
# （曲风只改学习，不该改实时段落账）；改前（5a67051）门参考学习区间，通用档学得短、House 档学到锁定，读数随曲风变。
#
#   p1r_segments_genre.py <工作目录> <标签 A> <标签 B>

import csv
import os
import sys

COLUMNS = ('start_sample', 'end_sample', 'heard', 'effective_s', 'loudness_lufs', 'low_share', 'live_trial_blocks', 'epoch')


def load(work, tag, track):
    folder = os.path.join(work, 'live', tag, 'traces')
    name = next(n for n in sorted(os.listdir(folder)) if n.startswith(track + '__') and n.endswith('.csv')
                and not n.endswith('__final.csv') and not n.endswith('__judgements.csv'))
    with open(os.path.join(folder, name), encoding='utf-8') as f:
        return list(csv.DictReader(f))


def main(argv):
    work, tag_a, tag_b = os.path.expanduser(argv[0]), argv[1], argv[2]
    with open(os.path.join(work, 'manifest.tsv'), encoding='utf-8') as f:
        manifest = list(csv.DictReader(f, delimiter='\t'))
    same = 0
    for r in manifest:
        a, b = load(work, tag_a, r['track']), load(work, tag_b, r['track'])
        diff = [(x['sequence'], [c for c in COLUMNS if x[c] != y[c]]) for x, y in zip(a, b) if any(x[c] != y[c] for c in COLUMNS)]
        if len(a) == len(b) and not diff:
            same += 1
        else:
            print(f"  {r['class']} {r['track']}：{len(a)} / {len(b)} 段，{len(diff)} 段不同，前两处 {diff[:2]}")
    print(f'{tag_a} vs {tag_b}（{work}）：段读数相同 {same} / {len(manifest)} 首')
    return 0 if same == len(manifest) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
