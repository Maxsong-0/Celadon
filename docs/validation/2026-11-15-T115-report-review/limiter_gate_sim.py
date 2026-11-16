"""限幅器 Auto 释放包络的只读仿真（核对报告用，一次性脚本，不进仓库）。

照 Source/dsp/LimiterModule.cpp 的 nextReleaseEnvelope / processLimiterCore 逐样本移植：
  需求增益 → 前瞻 + 保持段滑动最小（159 样本）→ 快 / 慢双包络 + 短过载门 → 两级滑动平均攻击整形。
真峰值检测用采样点绝对值近似（合成信号以低频为主，样本间峰可忽略）。
三种门：current = 现行代码；bridged = 门读「需求再保持 22 ms」后的信号（报告一 P0-2 的提议）；nogate = 不要短过载门。
"""
import math
import sys
from collections import deque

import numpy as np

FS = 48000
LOOKAHEAD = 96                 # 2 ms
HOLD = 63                      # TruePeakDetector::tapsPerPhase - 1
WINDOW = LOOKAHEAD + HOLD      # 159 样本 = 3.31 ms
SAFETY = 0.9605058184          # detectorSafetyGain
TRANSIENT_RETURN = 0.93
TRANSIENT_MAX = round(FS * 0.070)
ADAPT = 32


def coeff(ms, rate=FS):
    return math.exp(-1.0 / (ms * 0.001 * rate))


def sliding_min(values, window):
    out = np.empty_like(values)
    q = deque()
    for i, v in enumerate(values):
        while q and values[q[-1]] >= v:
            q.pop()
        q.append(i)
        if q[0] <= i - window:
            q.popleft()
        out[i] = values[q[0]]
    return out


def moving_average(values, length):
    if length <= 1:
        return values.copy()
    padded = np.concatenate([np.ones(length - 1), values])
    c = np.cumsum(padded)
    c = np.concatenate([[0.0], c])
    return (c[length:] - c[:-length]) / length


def envelope(held, gate, fast_ms):
    """返回 (包络, 慢包络被快包络顶起的样本占比, 短过载松开计时 > 0 的样本占比)。"""
    fast_c = coeff(fast_ms)
    slow_target_c = coeff(50.0)
    avg_c = coeff(500.0, FS / ADAPT)
    slow_c = coeff(100.0)
    bridged = sliding_min(held, round(FS * 0.022)) if gate == 'bridged' else held

    env_fast = env_slow = slow_target = 1.0
    deep = release = adapt = 0
    avg_red = 0.0
    out = np.empty_like(held)
    overridden = armed = 0

    for i in range(len(held)):
        t = held[i]
        env_fast = t if t <= env_fast else t + (env_fast - t) * fast_c
        slow_target = t + (slow_target - t) * slow_target_c
        env_slow = slow_target if slow_target <= env_slow else slow_target + (env_slow - slow_target) * slow_c

        if gate != 'nogate':
            g = bridged[i]
            if g < TRANSIENT_RETURN:
                deep = min(deep + 1, TRANSIENT_MAX + 1)
                if deep >= TRANSIENT_MAX:
                    release = 0
            else:
                if 0 < deep < TRANSIENT_MAX:
                    release = max(release, TRANSIENT_MAX // 2)
                deep = 0

            if release > 0:
                release -= 1
                armed += 1
                if env_slow < env_fast:
                    overridden += 1
                env_slow = max(env_slow, env_fast)
                slow_target = max(slow_target, env_fast)

        e = min(env_fast, env_slow)
        out[i] = e
        adapt += 1
        if adapt >= ADAPT:
            adapt = 0
            red = -20.0 * math.log10(max(e, 1e-6))
            avg_red = red + (avg_red - red) * avg_c
            depth = min(1.0, max(0.0, avg_red / 12.0))
            slow_c = coeff(100.0 + depth * 700.0)

    n = len(held)
    return out, overridden / n, armed / n


def band_rms_db(gain_db, lo, hi):
    """增益曲线（dB）在 [lo, hi] Hz 调制频带里的 RMS 与峰峰（去掉头 2 秒）。"""
    x = gain_db[2 * FS:]
    x = x - np.mean(x)
    spec = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(len(x), 1.0 / FS)
    spec[(freqs < lo) | (freqs > hi)] = 0.0
    y = np.fft.irfft(spec, len(x))
    return float(np.sqrt(np.mean(y * y))), float(np.max(y) - np.min(y))


def make_signals(seconds=12.0):
    n = int(seconds * FS)
    t = np.arange(n) / FS
    rng = np.random.default_rng(20261115)
    pad = rng.standard_normal(n)
    spec = np.fft.rfft(pad)
    freqs = np.fft.rfftfreq(n, 1.0 / FS)
    spec[(freqs < 2000) | (freqs > 8000)] = 0.0
    pad = np.fft.irfft(spec, n)
    pad *= 0.05 / np.sqrt(np.mean(pad * pad))            # 2–8 kHz 稳态铺底，约 -26 dBFS RMS

    beat = 60.0 / 124.0
    kick = np.zeros(n)
    kick_len = int(0.18 * FS)
    kt = np.arange(kick_len) / FS
    phase = 2 * np.pi * (50.0 * kt + (70.0 / 25.0) * (1 - np.exp(-25.0 * kt)))   # 120 Hz 滑到 50 Hz
    one = np.sin(phase) * np.exp(-kt / 0.060)
    k = 0
    while int(k * beat * FS) + kick_len < n:
        s = int(k * beat * FS)
        kick[s:s + kick_len] += one
        k += 1

    duck = np.ones(n)                                    # 贝斯在底鼓处让开（侧链式），反拍回来
    pos = (t % beat) / beat
    duck = np.clip((pos - 0.15) / 0.25, 0.0, 1.0)
    bass50 = np.sin(2 * np.pi * 50.0 * t)
    bass55 = np.sin(2 * np.pi * 55.0 * t)

    return {
        '持续 50 Hz 低音（超限约 3 dB）+ 高频铺底': 1.30 * bass50 + pad,
        '底鼓（超限约 5 dB）+ 反拍 55 Hz 贝斯（超限约 2 dB）+ 铺底': 1.65 * kick + 1.15 * duck * bass55 + pad,
        '只有底鼓（超限约 5 dB）+ 铺底': 1.65 * kick + pad,
        '底鼓 + 持续 50 Hz 低音（不让开，合计超限约 6 dB）+ 铺底': 1.2 * kick + 0.9 * bass50 + pad,
    }


def run(ceiling_db=-1.0):
    ceiling = 10 ** (ceiling_db / 20.0) * SAFETY
    shape = max(1, LOOKAHEAD // 2)
    ramp = max(1, LOOKAHEAD - shape + 1)

    for name, x in make_signals().items():
        peak = np.abs(x)
        required = np.where(peak > ceiling, ceiling / np.maximum(peak, 1e-12), 1.0)
        held = sliding_min(required, WINDOW)
        gaps = np.mean(held[2 * FS:] >= TRANSIENT_RETURN)
        print(f"\n### {name}")
        print(f"  需求增益（保持 3.31 ms 后）回到 0.93 以上的时间占比：{gaps * 100:.1f} %；最深需求 {20 * math.log10(held.min()):.2f} dB")
        print("  门            快释放  慢包络被顶起  松开计时>0  平均GR   0.5-8Hz 调制RMS/峰峰   60-250Hz 纹波RMS/峰峰")

        for fast_ms in (30.0, 80.0):
            for gate in ('current', 'bridged', 'nogate'):
                env, overridden, armed = envelope(held, gate, fast_ms)
                gain = moving_average(moving_average(env, ramp), shape)
                gain_db = 20.0 * np.log10(np.maximum(gain, 1e-6))
                mean_gr = -float(np.mean(gain_db[2 * FS:]))
                pump_rms, pump_pp = band_rms_db(gain_db, 0.5, 8.0)
                rip_rms, rip_pp = band_rms_db(gain_db, 60.0, 250.0)
                print(f"  {gate:<9}  {fast_ms:5.0f} ms   {overridden * 100:6.1f} %    {armed * 100:6.1f} %   {mean_gr:5.2f} dB   "
                      f"{pump_rms:5.3f} / {pump_pp:5.2f} dB          {rip_rms:5.3f} / {rip_pp:5.2f} dB")


if __name__ == '__main__':
    run(float(sys.argv[1]) if len(sys.argv) > 1 else -1.0)
