import numpy as np, scipy.io.wavfile as wf, sys, os
out=sys.argv[1]; os.makedirs(out, exist_ok=True)
fs=48000; t=np.arange(30*fs)/fs; rng=np.random.default_rng(4343)
noise=lambda a: a*(rng.random(len(t))*2-1)
s=np.sin; tp=2*np.pi
for a in (0.055,0.06,0.065,0.07,0.075):
    for off in (0.06,0.1,0.14):
        v=0.45*s(tp*49*t)+a*s(tp*98*t)+off*s(tp*4*t)+noise(0.01)
        wf.write(f'{out}/h{a:.3f}_o{off:.2f}.wav', fs, np.stack([v,v],axis=1).astype(np.float32))
print('ok')
