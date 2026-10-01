import numpy as np, sys
src=open('livestereo_emu.py').read().replace('\nmain()\n','\n')
ns={}; exec(compile(src,'emu','exec'),ns)
x,sr=ns['read_audio']('synth/synthetic-tech-124.wav')
ends,P,X,n,hop=ns['frames'](x,sr)
t=ends/sr; bar=4*60/124; b=(t/bar).astype(int)+1
brk=(b>64)&(b<=80); prog=(b-1)%4
g=P>=1e-6
for k,name in enumerate(['Am','F','C','G']):
    m=brk&(prog==k)
    hi=(P*g)[m][:,17:27]; hx=(X*g)[m][:,17:27]
    pm=0.5*(hi+hx).sum(1); ps=0.5*(hi-hx).sum(1)
    tot=P[m].sum(1)
    e17=(P*g)[m][:,17]; up=(P*g)[m][:,18:27].sum(1); b16=P[m][:,16]
    print(f'{name}: high group share {10*np.log10(np.median((pm+ps)/tot)):.1f} dB, band17 share of group {np.median(e17/np.maximum(pm+ps,1e-30)):.2f}, bands18+ / b16 {10*np.log10(np.median(np.maximum(up,1e-30)/b16)):.1f} dB, S/M {10*np.log10(np.median(np.maximum(ps,1e-30)/np.maximum(pm,1e-30))):.1f} dB, frames valid {(pm+ps>=1e-6).mean():.2f}')
