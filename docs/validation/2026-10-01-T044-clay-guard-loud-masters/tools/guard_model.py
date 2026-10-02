"""T-044：修坯峰值守护（ADR-014 第④级，只管①低切）的独立模型，照 ClayModule.cpp PeakGuard::processBlock 逐步写（不追求逐位）。
用法：python guard_model.py <wav> <lowcut Hz> [hr_db=0] [start_s] [dur_s]
"""
import sys, numpy as np, soundfile as sf
from numba import njit
from scipy.signal import sosfilt
from scipy.ndimage import maximum_filter1d, minimum_filter1d

def rbj_hp(fs, f0, q):
    w0 = 2*np.pi*f0/fs; c = np.cos(w0); al = np.sin(w0)/(2*q)
    b = np.array([(1+c)/2, -(1+c), (1+c)/2]); a = np.array([1+al, -2*c, 1-al])
    return np.concatenate([b/a[0], a/a[0]])

def lowcut(x, fs, fc):
    sos = np.vstack([rbj_hp(fs, fc, 0.54119610014619698), rbj_hp(fs, fc, 1.3065629648763766)])
    return sosfilt(sos, x, axis=0)

@njit(cache=True)
def envelope(E, retain):
    P = np.empty_like(E); env = 0.0
    for i in range(E.size):
        env = max(E[i], env*retain); P[i] = env
    return P

@njit(cache=True)
def release(h, step):
    r = np.empty_like(h); prev = 1.0
    for i in range(h.size):
        prev = min(h[i], prev + step); r[i] = prev
    return r

def guard(x, y, fs, hr_db=0.0, tau=0.50118723362727224, floor=0.01, rel_s=0.1):
    W = int(round(0.008*fs)); A = int(round(0.003*fs))
    mag = np.abs(x).max(axis=1)
    E = maximum_filter1d(mag, 2*W+1, mode='constant', cval=0.0)
    P = envelope(E, np.exp(-1.0/(1.0*fs)))
    B = np.maximum(E*10**(hr_db/20), np.maximum(tau*P, floor))
    d = y - x
    req = np.ones(len(x))
    for c in range(x.shape[1]):
        over = np.abs(y[:, c]) > B
        lim = np.where(over, (B - np.where(d[:, c] > 0, x[:, c], -x[:, c])) / np.maximum(np.abs(d[:, c]), 1e-300), 1.0)
        req = np.minimum(req, np.clip(lim, 0.0, 1.0))
    # h(j) = min req[j, j+A]（前瞻）
    h = minimum_filter1d(req, A+1, origin=-((A+1)//2), mode='nearest')  # 窗 = [j, j + A]（A 奇偶都对；109 上用单点冲激核对过）
    r = release(h, 1.0/(rel_s*fs))
    g = np.convolve(r, np.ones(A+1)/(A+1))[:len(r)]  # 长 A+1 的滑动平均（向后）
    g[:A] = np.cumsum(r[:A])/np.arange(1, A+1)
    g = np.minimum(g, 1.0)
    ratio = np.abs(y).max(axis=1) / B
    cl = int(round(0.01*fs)); n = len(ratio)//cl
    cells = ratio[:n*cl].reshape(n, cl).max(axis=1)
    pressDb = np.where(cells > 1, 20*np.log10(cells), 0.0)
    return g, pressDb, B, req

def main():
    path, fc = sys.argv[1], float(sys.argv[2])
    hr = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    x, fs = sf.read(path, dtype='float64', always_2d=True)
    if len(sys.argv) > 5:
        s0, du = float(sys.argv[4]), float(sys.argv[5]); x = x[int(s0*fs):int((s0+du)*fs)]
    y = lowcut(x, fs, fc)
    g, pressDb, B, req = guard(x, y, fs, hr)
    d = y - x
    out = x + g[:, None]*d
    act = (np.abs(x).max(axis=1) > 0.01)
    keep = 10*np.log10((g[:, None]**2*d**2).sum()/(d**2).sum())
    print(f"fc={fc} hr={hr}dB  g<1 {100*(g<1).mean():.2f}%  mean g {g.mean():.4f}  g<0.5 {100*(g<0.5).mean():.2f}%  min g {g.min():.4f}  "
          f"retainDb {keep:.2f}  pressMean {pressDb.mean():.4f} dB  cells>0.05 {100*(pressDb>0.05).mean():.1f}%  >0.2 {100*(pressDb>0.2).mean():.1f}%  "
          f"maxPress {pressDb.max():.3f} dB  req<1 {100*(req<1).mean():.2f}%")
    # 整首、每 400 ms 的峰值：不守护 / 守护 / 干路
    blk = int(0.4*fs); n = len(x)//blk
    def pk(s): return 20*np.log10(np.abs(s[:n*blk]).max(axis=1).reshape(n, blk).max(axis=1) + 1e-12)
    px, py, po = pk(x), pk(y), pk(out)
    print(f"  400ms block peak rise vs dry: unguarded mean {np.mean(py-px):+.3f} p95 {np.percentile(py-px,95):+.3f} max {np.max(py-px):+.3f} dB | "
          f"guarded mean {np.mean(po-px):+.3f} max {np.max(po-px):+.3f} | song peak dry {px.max():.3f} unguarded {py.max():.3f} guarded {po.max():.3f} dBFS")
    np.save(path + f".g_{fc}_{hr}.npy", g.astype(np.float32))

if __name__ == '__main__':
    main()
