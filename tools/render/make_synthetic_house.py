#!/usr/bin/env python3
# T-038 House 门槛的合成测试曲（设计规格-曲风与交付 第 7 节第 4 条；设计规格-曲风-House学习方式 第 1、2 节）。
#
# 合成信号证明不了「好听」，但能把 House 的段落结构摆得清清楚楚：前奏没有 bass、breakdown 没有 kick / bass，
# 用来跑通渲染工具的逐段统计（--segments-csv）与 tools/render/house_gate.py，也给门控阈值一个已知答案的对照。
# 真正的门槛与阈值标定要用 Max 的 10～20 首 House（规格第 3.2、6 节），这两首只是脚本的阳性对照。
#
# 用法：make_synthetic_house.py <输出目录> [--bpm 124] [--rate 48000] [--seed 20261001] [--peak-dbfs -3]
# 产出（都在输出目录第一层：celadon_render --in 直接读；标注 .json 不是音频，渲染工具会跳过）：
#   synthetic-house-124.wav / .json  32 小节纯鼓前奏（kick + hat）→ 32 小节 drop（kick + hat + clap + 沙锤 + offbeat bass + pad）
#                                    → 16 小节 breakdown（只有 pad）→ 32 小节 drop
#   synthetic-tech-124.wav  / .json  同样的段落，但前奏就有 16 分音符滚动 bass（Tech House：前奏已接近满编）
# 配比按「前奏 → drop 响 4～5 LU、breakdown 比 drop 轻 8 LU 左右」调（底鼓衰减 110 ms、bass 持续、pad 0.06）：底鼓太长时
# 前奏与 drop 的段响度只差 1 LU、低频占比也一样，门控分不开，起不到对照作用（2026-10-01 第一版实测）。
# 标注 JSON：{"song", "file", "bpm", "meter", "sample_rate", "bars", "sections": [{"name", "kind", "start_bar", "end_bar"}]}，
#   小节从 1 起、两端都含；kind ∈ intro / drop / breakdown（house_gate.py 按它认段落）。最后一小节之后补 0.5 s 静音让鼓尾放完（不算小节）。
# 24 bit 立体声 PCM（标准库 wave 写），每首各自把采样峰值归一到 --peak-dbfs。依赖 numpy（109 上用 ~/.venvs/audio/bin/python）。
# 同一组参数每次生成逐位相同（固定种子；只用 numpy 的确定性运算）。
import argparse
import json
import os
import sys
import wave

import numpy as np

INTRO_BARS, DROP_BARS, BREAKDOWN_BARS = 32, 32, 16
BEATS_PER_BAR = 4
TAIL_SECONDS = 0.5

# 四小节一循环的和声（Am – F – C – G）：bass 根音（Hz，都在 100 Hz 以下）与 pad 三和弦（中音区）。
BASS_ROOTS = [55.0, 43.654, 65.406, 48.999]
PAD_CHORDS = [
    [220.0, 261.626, 329.628],
    [174.614, 220.0, 261.626],
    [261.626, 329.628, 391.995],
    [195.998, 246.942, 293.665],
]


def sections():
    """段落表：(名字, kind, 起始小节, 结束小节)，小节从 1 起、两端都含。"""
    a = INTRO_BARS
    b = a + DROP_BARS
    c = b + BREAKDOWN_BARS
    d = c + DROP_BARS
    return [
        ('intro', 'intro', 1, a),
        ('drop1', 'drop', a + 1, b),
        ('breakdown', 'breakdown', b + 1, c),
        ('drop2', 'drop', c + 1, d),
    ]


def kind_of_bar(bar):
    for _, kind, start, end in sections():
        if start <= bar <= end:
            return kind
    return None


def add_at(buffer, start, sound):
    """把 sound（n × 2）加到 buffer 的 start 处（越界部分截掉）。"""
    end = min(buffer.shape[0], start + sound.shape[0])
    if start < end:
        buffer[start:end] += sound[: end - start]


def stereo(mono, left=1.0, right=1.0):
    return np.stack([mono * left, mono * right], axis=1)


def kick(rate):
    """底鼓：158 Hz 扫到 48 Hz 的正弦 + 很短的起音，110 ms 衰减（再长底鼓就占掉大半能量，前奏与 drop 的响度拉不开）。"""
    n = int(0.4 * rate)
    t = np.arange(n) / rate
    freq = 48.0 + 110.0 * np.exp(-t / 0.035)
    phase = 2.0 * np.pi * np.cumsum(freq) / rate
    env = np.exp(-t / 0.11) * np.minimum(1.0, t / 0.002)
    return 0.8 * np.sin(phase) * env


def noise_burst(rate, rng, seconds, decay, gain, order):
    """高通过的噪声短音：白噪声做 order 次一阶差分（每次 +6 dB / 倍频程），指数衰减。"""
    n = int(seconds * rate)
    t = np.arange(n) / rate
    x = rng.standard_normal(n)
    for _ in range(order):
        x = np.diff(x, prepend=0.0)
    return gain * x * np.exp(-t / decay) * np.minimum(1.0, t / 0.0005)


def bass_note(freq, seconds, rate, gain, drive):
    """一个 bass 音：基音 + 两个泛音，4 ms 起音、按音长缓慢衰减（持续的低音线），末尾 5 ms 收到 0；drive > 0 时过一点 tanh（Tech 的粗一点）。"""
    n = int(seconds * rate)
    t = np.arange(n) / rate
    x = np.sin(2.0 * np.pi * freq * t) + 0.25 * np.sin(4.0 * np.pi * freq * t) + 0.1 * np.sin(6.0 * np.pi * freq * t)
    if drive > 0.0:
        x = np.tanh(drive * x) / np.tanh(drive)
    env = np.minimum(1.0, t / 0.004) * np.exp(-t / (1.2 * seconds))
    release = int(0.005 * rate)
    if n > release:
        env[-release:] *= np.linspace(1.0, 0.0, release)
    return gain * x * env


def pad_bar(chord, start, length, rate, xfade, gain):
    """一小节的 pad：三个音各 6 个泛音（1 / h，带限的锯齿感），左右各自轻微失谐（宽）；两头各 xfade 样本升余弦交叉淡化。
    返回 (起点样本, n × 2)。"""
    begin = start - xfade
    n = length + 2 * xfade
    t = (np.arange(n) + begin) / rate
    out = np.zeros((n, 2))
    for voice, freq in enumerate(chord):
        for ch, detune in enumerate((0.997, 1.003)):
            f = freq * detune
            phase_offset = 0.37 * (voice + 1) + 1.7 * ch
            for h in range(1, 7):
                out[:, ch] += np.sin(2.0 * np.pi * f * h * t + phase_offset * h) / h
    env = np.ones(n)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(2 * xfade) / (2 * xfade))
    env[: 2 * xfade] = ramp
    env[-2 * xfade:] = ramp[::-1]
    return begin, gain * out * env[:, None]


def render(style, bpm, rate, seed):
    """style = 'house'（前奏只有鼓）或 'tech'（前奏就有滚动 bass）。返回 (n × 2 浮点, 小节数)。"""
    rng = np.random.default_rng(seed + (0 if style == 'house' else 1))
    beat = 60.0 / bpm * rate                      # 每拍样本数（小数）
    bars = sections()[-1][3]
    total = int(round(bars * BEATS_PER_BAR * beat + TAIL_SECONDS * rate))
    mix = np.zeros((total, 2))

    kick_sound = stereo(kick(rate))
    hats = [noise_burst(rate, rng, 0.05, 0.012, 0.06, 2) for _ in range(8)]
    claps = [noise_burst(rate, rng, 0.12, 0.03, 0.15, 1) for _ in range(4)]
    shakers = [noise_burst(rate, rng, 0.03, 0.008, 0.03, 2) for _ in range(8)]
    xfade = int(0.03 * rate)

    def at(position_beats):
        return int(round(position_beats * beat))

    for bar in range(1, bars + 1):
        kind = kind_of_bar(bar)
        bar_beat = (bar - 1) * BEATS_PER_BAR
        progression = (bar - 1) % 4
        drums = kind in ('intro', 'drop')

        for b in range(BEATS_PER_BAR):
            if drums:
                add_at(mix, at(bar_beat + b), kick_sound)
                hat = hats[(bar * 4 + b) % len(hats)]
                pan = 0.65 if b % 2 == 0 else 0.45          # 开镲在 offbeat，左右轻微交替
                add_at(mix, at(bar_beat + b + 0.5), stereo(hat, pan, 1.1 - pan))

            if kind == 'drop' and b in (1, 3):
                add_at(mix, at(bar_beat + b), stereo(claps[(bar + b) % len(claps)], 0.9, 0.9))

            # drop 另加 16 分音符沙锤（左右交替），中高频更满。
            if kind == 'drop':
                for s in range(4):
                    pan = 0.7 if s % 2 == 0 else 0.4
                    add_at(mix, at(bar_beat + b + 0.25 * s), stereo(shakers[(bar * 16 + b * 4 + s) % len(shakers)], pan, 1.1 - pan))

            root = BASS_ROOTS[progression]

            # Deep 式 offbeat bass：只在 drop；八分音符长。
            if style == 'house' and kind == 'drop':
                add_at(mix, at(bar_beat + b + 0.5), stereo(bass_note(root, 0.45 * beat / rate, rate, 0.7, 0.0)))

            # Tech 式滚动 bass：前奏与 drop 都有；每拍后三个 16 分音符（正拍让给底鼓），第三个跳八度。
            if style == 'tech' and kind in ('intro', 'drop'):
                for s in (1, 2, 3):
                    freq = root * (2.0 if s == 3 else 1.0)
                    add_at(mix, at(bar_beat + b + 0.25 * s), stereo(bass_note(freq, 0.22 * beat / rate, rate, 0.55, 1.5)))

        if kind in ('drop', 'breakdown'):
            start = at(bar_beat)
            length = at(bar_beat + BEATS_PER_BAR) - start
            begin, sound = pad_bar(PAD_CHORDS[progression], start, length, rate, xfade, 0.06)
            add_at(mix, max(0, begin), sound[max(0, -begin):])

    return mix, bars


def write_wav24(path, audio, rate):
    """24 bit 立体声 PCM：四舍五入到 24 bit 整数、截到范围内，小端三字节交错写。"""
    ints = np.clip(np.round(audio * 8388607.0), -8388608, 8388607).astype('<i4')
    raw = ints.reshape(-1).view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(rate)
        w.writeframes(raw)


def main(argv):
    parser = argparse.ArgumentParser(description='T-038 House 门槛的合成测试曲与段落标注')
    parser.add_argument('out_dir')
    parser.add_argument('--bpm', type=float, default=124.0)
    parser.add_argument('--rate', type=int, default=48000)
    parser.add_argument('--seed', type=int, default=20261001)
    parser.add_argument('--peak-dbfs', type=float, default=-3.0)
    args = parser.parse_args(argv)

    if not (60.0 <= args.bpm <= 200.0) or args.rate < 8000 or args.peak_dbfs > 0.0:
        parser.error('--bpm 要 60 ~ 200，--rate ≥ 8000，--peak-dbfs ≤ 0')

    os.makedirs(args.out_dir, exist_ok=True)
    peak_target = 10.0 ** (args.peak_dbfs / 20.0)
    tempo = int(round(args.bpm)) if float(args.bpm).is_integer() else args.bpm

    for style in ('house', 'tech'):
        mix, bars = render(style, args.bpm, args.rate, args.seed)
        mix *= peak_target / float(np.max(np.abs(mix)))
        song = f'synthetic-{style}-{tempo}'
        wav_path = os.path.join(args.out_dir, song + '.wav')
        write_wav24(wav_path, mix, args.rate)

        annotation = {
            'song': song,
            'file': song + '.wav',
            'bpm': args.bpm,
            'meter': '4/4',
            'sample_rate': args.rate,
            'bars': bars,
            'style': 'Tech House（前奏就有滚动 bass）' if style == 'tech' else 'Deep / 通用 House（前奏只有鼓）',
            'sections': [{'name': n, 'kind': k, 'start_bar': s, 'end_bar': e} for n, k, s, e in sections()],
        }
        with open(os.path.join(args.out_dir, song + '.json'), 'w', encoding='utf-8') as f:
            json.dump(annotation, f, ensure_ascii=False, indent=2)
            f.write('\n')

        seconds = mix.shape[0] / args.rate
        print(f'{wav_path}：{bars} 小节 + {TAIL_SECONDS} s 尾巴，{seconds:.2f} s，采样峰值 {args.peak_dbfs:.1f} dBFS', file=sys.stderr)

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
