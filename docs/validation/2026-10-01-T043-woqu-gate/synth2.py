import numpy as np, scipy.io.wavfile as wf, sys, os
out=sys.argv[1]; os.makedirs(out, exist_ok=True)
fs=48000; t=np.arange(30*fs)/fs; rng=np.random.default_rng(4343)
noise=lambda a: a*(rng.random(len(t))*2-1)
s=np.sin; tp=2*np.pi
cands={
 'H_49_98a08_off10': 0.45*s(tp*49*t)+0.08*s(tp*98*t)+0.10*s(tp*4*t)+noise(0.01),
 'I_49_98a05_off10': 0.45*s(tp*49*t)+0.05*s(tp*98*t)+0.10*s(tp*4*t)+noise(0.01),
 'J_49_98a10_off12': 0.45*s(tp*49*t)+0.10*s(tp*98*t)+0.12*s(tp*4*t)+noise(0.01),
 'K_49_147a10_off10': 0.45*s(tp*49*t)+0.10*s(tp*147*t)+0.10*s(tp*4*t)+noise(0.01),
 'L_49_98a07_off06': 0.45*s(tp*49*t)+0.07*s(tp*98*t)+0.06*s(tp*4*t)+noise(0.01),
}
for k,v in cands.items():
    x=np.stack([v,v],axis=1).astype(np.float32)
    wf.write(f'{out}/{k}.wav', fs, x)
print('ok')
