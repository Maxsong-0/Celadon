"""T-044：22 首门槛歌里自动低切开着（或压力反馈关掉）的歌，按先试口径（滑动 10 s 的 100 ms 块峰值 P99 比）与现行压力格（10 ms 格超界 dB 平均、2 s 平滑）对照。
模型输入是原曲（插件里 Clay 前还有 Balance），只作量级对照。"""
import sys, numpy as np, soundfile as sf
sys.path.insert(0, '.')
from guard_model import lowcut, guard
from scipy.ndimage import uniform_filter1d
from scipy.signal import butter, sosfiltfilt
S = '/home/maxsong/.cache/celadon-render/songs-9a355b99857c/'
cases = [("emotapes", 11.58), ("fule", 12.15), ("somggggg", 11.24), ("somggggg_mp3", 11.42), ("woqu", 16.25), ("woqu", 20.0),
         ("WTF", 10.58), ("will we be okay", 13.72), ("in/deep", 10.59), ("in/tech", 13.6)]
alias = {"in/deep": "/home/maxsong/.cache/celadon-t044/in/deep-track_0155328.wav", "in/tech": "/home/maxsong/.cache/celadon-t044/in/tech-track_0046047.wav"}
def bp(s, fs):
    blk = int(round(0.1*fs)); n = len(s)//blk
    return np.abs(s[:n*blk]).max(axis=1).reshape(n, blk).max(axis=1)
sos = butter(4, [25, 80], btype='band', fs=fs if False else 44100, output='sos')
print("song               Hz   | wholeP99 | slide10s P99: med  p90  >0.2 | press2s: med  p90  >0.2 | g<1%  mean g retain | 25-80Hz blend")
for name, fc in cases:
    x, fs = sf.read(alias.get(name, S + name + ".wav"), dtype='float64', always_2d=True)
    y = lowcut(x, fs, fc); d = y - x
    g, press, B, req = guard(x, y, fs)
    px, py = bp(x, fs), bp(y, fs)
    act = px > px.max()*10**(-30/20)
    whole = 20*np.log10(np.percentile(py[act], 99)/np.percentile(px[act], 99))
    w = 100; c = np.array([20*np.log10(np.percentile(py[s:s+w], 99)/np.percentile(px[s:s+w], 99)) for s in range(0, max(1, len(px)-w), 10) if act[s:s+w].mean() > 0.5])
    ps = uniform_filter1d(press, 200)
    keep = 10*np.log10(((g[:, None]*d)**2).sum()/(d**2).sum())
    sb = butter(4, [25, 80], btype='band', fs=fs, output='sos')
    out = x + g[:, None]*d
    eb = lambda s: np.sum(sosfiltfilt(sb, s.sum(axis=1))**2)
    blend = 10*np.log10(eb(out)/eb(y))
    print(f"{name[:17]:17s} {fc:5.2f} | {whole:+.3f}  | {np.median(c):+.3f} {np.percentile(c,90):+.3f} {np.mean(c>0.2):.2f} | {np.median(ps):.3f} {np.percentile(ps,90):.3f} {np.mean(ps>0.2):.3f} | {100*(g<1).mean():5.1f} {g.mean():.3f} {keep:+.2f} | {blend:+.3f} dB", flush=True)
