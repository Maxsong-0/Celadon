import sys, numpy as np, soundfile as sf
sys.path.insert(0, '.')
from guard_model import lowcut, guard
def p99cost(x, y, fs, s0, s1):
    blk = int(round(0.1*fs)); a, b = int(s0*fs), int(s1*fs); n = (b-a)//blk
    px = np.abs(x[a:a+n*blk]).max(axis=1).reshape(n, blk).max(axis=1)
    py = np.abs(y[a:a+n*blk]).max(axis=1).reshape(n, blk).max(axis=1)
    return 20*np.log10(np.percentile(py, 99)/np.percentile(px, 99))
for path in sys.argv[1:]:
    x, fs = sf.read(path, dtype='float64', always_2d=True)
    dur = len(x)/fs
    # 次声成分：<10 Hz 与 10~20 Hz 的能量占比
    X = np.fft.rfft(x[:, 0]+x[:, 1]); f = np.fft.rfftfreq(len(x), 1/fs); P = np.abs(X)**2
    tot = P.sum()
    print(f"{path.split('/')[-1]}  dur {dur:.0f}s  share <5Hz {10*np.log10(P[f<5].sum()/tot):.1f} dB  5-10 {10*np.log10(P[(f>=5)&(f<10)].sum()/tot):.1f}  10-16 {10*np.log10(P[(f>=10)&(f<16)].sum()/tot):.1f}  16-25 {10*np.log10(P[(f>=16)&(f<25)].sum()/tot):.1f}  25-40 {10*np.log10(P[(f>=25)&(f<40)].sum()/tot):.1f} dB")
    for fc in [10.0, 10.59, 13.6, 16.0, 20.0, 25.0]:
        y = lowcut(x, fs, fc)
        g, press, B, req = guard(x, y, fs)
        d = y - x; keep = 10*np.log10(((g[:, None]*d)**2).sum()/(d**2).sum())
        print(f"  fc {fc:5.2f}: P99 cost listen[0,14.6] {p99cost(x,y,fs,0,14.6):+.3f}  whole {p99cost(x,y,fs,0,dur):+.3f} dB | guard g<1 {100*(g<1).mean():5.1f}%  mean g {g.mean():.3f}  retain {keep:+.2f} dB  press {press.mean():.3f}")
