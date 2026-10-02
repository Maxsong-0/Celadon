# T-043：合成候选素材（30 s，48 kHz，两声道相同），写 float wav。
import numpy as np, scipy.io.wavfile as wf, sys, os
out=sys.argv[1]; os.makedirs(out, exist_ok=True)
fs=48000; t=np.arange(30*fs)/fs; rng=np.random.default_rng(4343)
noise=lambda a: a*(rng.random(len(t))*2-1)
s=np.sin; tp=2*np.pi
cands={
 'A_49_off30': 0.5*s(tp*49*t)+0.3*s(tp*4*t),
 'B_60_110': 0.25*s(tp*60*t)+0.15*s(tp*110*t)+noise(0.05),
 'C_55_110_off05': 0.3*s(tp*55*t)+0.15*s(tp*110*t)+0.05*s(tp*4*t)+noise(0.01),
 'D_49_98_off08': 0.45*s(tp*49*t)+0.15*s(tp*98*t)+0.08*s(tp*4*t)+noise(0.01),
 'E_60_110_off10': 0.25*s(tp*60*t)+0.15*s(tp*110*t)+0.1*s(tp*4*t)+noise(0.05),
 'F_49_off10': 0.45*s(tp*49*t)+0.10*s(tp*4*t)+noise(0.01),
 'G_49_98_off15': 0.4*s(tp*49*t)+0.15*s(tp*98*t)+0.15*s(tp*4*t)+noise(0.01),
}
for k,v in cands.items():
    x=np.stack([v,v],axis=1).astype(np.float32)
    wf.write(f'{out}/{k}.wav', fs, x)
print('ok', list(cands))
