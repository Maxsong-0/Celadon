import sys, numpy as np, soundfile as sf
sys.path.insert(0, '.')
from guard_model import lowcut
S = '/home/maxsong/.cache/celadon-render/songs-9a355b99857c/'
alias = {"deep": "in/deep-track_0155328.wav", "tech": "in/tech-track_0046047.wav"}
cases = [("somggggg", 11.24), ("deep", 10.59), ("emotapes", 11.58), ("tech", 13.6), ("somggggg_mp3", 11.42), ("fule", 12.15), ("woqu", 11.60)]
def bpk(s, fs):
    blk = int(round(0.1*fs)); n = len(s)//blk
    return np.abs(s[:n*blk]).max(axis=1).reshape(n, blk).max(axis=1)
for name, fc0 in cases:
    x, fs = sf.read(alias.get(name, S + name + ".wav"), dtype='float64', always_2d=True)
    px = bpk(x, fs); valid = px > px.max()*10**(-30/20)
    out = []
    for scale in (1.0, 0.5):
        py = bpk(lowcut(x, fs, 10*(fc0/10)**scale), fs)
        wy, wx, best = [], [], {200: -9.0, 300: -9.0, 512: -9.0}
        for t in range(len(px)):
            if not valid[t]: continue
            wy.append(py[t]); wx.append(px[t]); n = len(wy)
            if n >= 200:
                c = 20*np.log10(np.percentile(wy, 99)/np.percentile(wx, 99))
                for k in best:
                    if n >= k: best[k] = max(best[k], c)
        out.append(f"s{scale}: max cost n>=200 {best[200]:+.2f} n>=300 {best[300]:+.2f} n>=512 {best[512]:+.2f}")
    print(f"{name[:14]:14s} " + " | ".join(out), flush=True)
