#!/usr/bin/env python3
# T-058 交叉核对的统计：读带探针的 spectrum-peak-bench 输出（probe_peak_rebuilds.py 打的 PROBE 行与基准自己的逐秒行），
# 逐帧把事件归到那一帧（两条 PROBE frame 之间的事件属于后一帧），再按基准的逐秒段、按组合汇总：
#   - 「曲线帧」（脏区够到底边）与「setSpectrum 判变了 / 整块重画」逐帧是否一一对应；buildCurvePaths 次数 = 判变了 + paint 里重建；
#   - 判变了的帧按第一处判变化的曲线分（inputReady / output / input / average / peak）；first = peak 即「只因峰值线」；
#   - 只因峰值线的帧这一帧重画前后频谱仪那一块变了几个设备像素（PEAK 关时若是 0 = 屏上什么都没变）；同一帧还有读数框 / 响度小窗
#     文字重画的（with text）单列、不比像素（文字本来就变）；
#   - setSpectrum 的耗时（探针墙钟，单核）：判变了的帧与没判变的帧各取中位数 / 平均。
# 用法：count_probe.py <带探针的基准输出> [--frames]（--frames 另打 PEAK 关、只因峰值线、同一帧没有文字重画、屏上却有变化的那些帧）
import re
import statistics
import sys

path = sys.argv[1]
show_frames = '--frames' in sys.argv

seg_re = re.compile(r'^  (\S.*?)\s+\| fr\s+(\d+) dirty\s+(\d+) curves\s+(\d+) \|')
kv_re = re.compile(r'(\w+)=(\S+)')


def new_stats():
    return dict(frames=0, sets=0, rebuilds=0, first={}, curves=0, dirty=0, whole=0, mismatch=0, dirty_unexplained=0,
                paint_rebuilds=0, builds=0, peak_only=0, peak_only_unchanged=0, peak_only_changed=0, peak_only_max_px=0,
                peak_only_max_worst=0, peak_only_with_text=0, curves_with_text=0, t_rebuild=[], t_build=[], t_quiet=[], seg_frames_check=None)


def add(a, b):
    for k, v in b.items():
        if k == 'first':
            for f, n in v.items():
                a['first'][f] = a['first'].get(f, 0) + n
        elif isinstance(v, list):
            a[k].extend(v)
        elif k in ('seg_frames_check', 'bench'):
            continue
        elif k in ('peak_only_max_px', 'peak_only_max_worst'):
            a[k] = max(a[k], v)
        else:
            a[k] += v


def fmt_t(xs):
    if not xs:
        return '-'
    return f'med {statistics.median(xs) * 1000:.1f} / avg {statistics.mean(xs) * 1000:.1f} us'


def line(label, s, width=22):
    firsts = ' '.join(f'{k}:{s["first"][k]}' for k in ('inputReady', 'output', 'input', 'average', 'peak') if s['first'].get(k))
    return (f'  {label:<{width}} fr {s["frames"]:4d} | rebuilds {s["rebuilds"]:4d} ({firsts or "-"}) | curves {s["curves"]:4d} '
            f'(with text {s["curves_with_text"]}) whole {s["whole"]} mismatch {s["mismatch"]} | builds {s["builds"]:4d} = rebuilds + paint {s["paint_rebuilds"]} | '
            f'peak-only {s["peak_only"]:4d}: alone {s["peak_only"] - s["peak_only_with_text"]} -> screen unchanged {s["peak_only_unchanged"]}, '
            f'changed {s["peak_only_changed"]} (max {s["peak_only_max_px"]} px, {s["peak_only_max_worst"]} LSB); '
            f'with readout / HUD repaint {s["peak_only_with_text"]} | dirty w/o event {s["dirty_unexplained"]} | '
            f'setSpectrum rebuild fr {fmt_t(s["t_rebuild"])} (build {fmt_t(s["t_build"])}), quiet fr {fmt_t(s["t_quiet"])}')


cases = []
case = None
seg = None
pending = []          # 这一帧到目前为止的事件
last_builds = None
latest_builds = None   # 任何一行 PROBE 里最新的 builds=（含开场设置时 paint 里的那次重建，它在组合标题之前）
out = []
frames_detail = []
seg_details = []       # 这一段里待报的帧（段名在段末那一行才知道）

with open(path, encoding='utf-8') as f:
    for raw in f:
        row = raw.rstrip('\n')
        if row.startswith('PROBE ') and 'builds=' in row:
            latest_builds = int(dict(kv_re.findall(row))['builds'])
        if row.startswith('---- '):
            case = dict(title=row[5:], segments=[], total=new_stats())
            cases.append(case)
            seg = new_stats()
            pending = []
            last_builds = latest_builds
            continue
        if case is None:
            continue
        if row.startswith('PROBE '):
            kind = row.split()[1]
            kv = dict(kv_re.findall(row))
            if kind == 'frame':
                ev = pending
                pending = []
                st = seg
                st['frames'] += 1
                sets = [e for e in ev if e[0] == 'set']
                whole = [e for e in ev if e[0] == 'wholeRepaint']
                paint_rb = [e for e in ev if e[0] == 'paintRebuild']
                small = [e for e in ev if e[0] in ('readoutRepaint', 'hudRepaint')]
                rebuilt = any(e[1]['first'] != 'none' for e in sets)
                st['sets'] += len(sets)
                st['paint_rebuilds'] += len(paint_rb)
                st['whole'] += 1 if whole else 0
                for e in sets:
                    first = e[1]['first']
                    if first != 'none':
                        st['rebuilds'] += 1
                        st['first'][first] = st['first'].get(first, 0) + 1
                        st['t_rebuild'].append(float(e[1]['curvesMs']))
                        st['t_build'].append(float(e[1]['buildMs']))
                    else:
                        st['t_quiet'].append(float(e[1]['curvesMs']))
                # buildCurvePaths 计数：取这一帧里最后一个带 builds= 的事件
                for e in ev:
                    if 'builds' in e[1]:
                        b = int(e[1]['builds'])
                        if last_builds is not None:
                            st['builds'] += b - last_builds
                        last_builds = b
                curves = kv['curves'] == '1'
                dirty = kv['dirty'] == '1'
                st['curves'] += 1 if curves else 0
                st['curves_with_text'] += 1 if curves and small else 0   # 曲线帧里同时有读数框 / 响度小窗文字重画的（重画耗时里混着文字）
                st['dirty'] += 1 if dirty else 0
                expected_curves = rebuilt or bool(whole)
                if curves != expected_curves:
                    st['mismatch'] += 1
                if dirty and not (expected_curves or small):
                    st['dirty_unexplained'] += 1
                peak_only = any(e[1]['first'] == 'peak' for e in sets)
                if peak_only:
                    st['peak_only'] += 1
                if peak_only and small:
                    st['peak_only_with_text'] += 1   # 同一帧还有读数框 / 响度小窗的文字重画：变了的像素分不开，单列
                elif peak_only:
                    px = int(kv['changedPx'])
                    worst = int(kv['worst'])
                    if px == 0:
                        st['peak_only_unchanged'] += 1
                    else:
                        st['peak_only_changed'] += 1
                        st['peak_only_max_px'] = max(st['peak_only_max_px'], px)
                        st['peak_only_max_worst'] = max(st['peak_only_max_worst'], worst)
                        if show_frames and 'PEAK off' in case['title']:
                            seg_details.append(f'frame {st["frames"]} | changedPx {px} worst {worst}')
            else:
                if kind == 'set' or kind == 'paintRebuild':
                    pass
                pending.append((kind, kv))
            continue
        m = seg_re.match(row)
        if m:
            label = m.group(1)
            if label.startswith('~'):
                continue          # 中段合计行：不切段
            if label.startswith('='):
                case['total'] = seg_total = new_stats()
                for _, s in case['segments']:
                    add(seg_total, s)
                seg_total['bench'] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
                case = None
                continue
            seg['bench'] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
            case['segments'].append((label, seg))
            seg = new_stats()
            frames_detail.extend(f'{case["title"]} | {label} | {d}' for d in seg_details)
            seg_details.clear()

# 段级：基准自己数的 帧 / 脏 / 曲线 与探针逐帧数的对上没有
for c in cases:
    out.append(f'---- {c["title"]}')
    for label, s in c['segments']:
        bf, bd, bc = s['bench']
        agree = 'ok' if (bf, bd, bc) == (s['frames'], s['dirty'], s['curves']) else f'BENCH {bf}/{bd}/{bc} != PROBE {s["frames"]}/{s["dirty"]}/{s["curves"]}'
        out.append(line(label, s) + f' | bench fr/dirty/curves {agree}')
    t = c['total']
    out.append(line('= total', t))

print('\n'.join(out))
print('==== summary (per case)')
for c in cases:
    print(line(c['title'], c['total'], 34))

if show_frames:
    print('==== PEAK off, peak-only rebuild frames whose repaint changed pixels')
    print('\n'.join(frames_detail) if frames_detail else '(none)')
