import sys, tempfile, re, json
from pathlib import Path
import numpy as np
import soundfile as sf
sys.path.insert(0,'tools/render')
from codec_tp_check import measure, run, db
rows=[]
with tempfile.TemporaryDirectory(prefix='t085-meter-') as folder:
    sr=48000
    for name, freq, amplitude, duration in [('tone1k',1000,.1,11), ('intersample',12000,1.05,11)]:
        t=np.arange(sr*duration)/sr
        fade=np.minimum(1,np.minimum(t/.1,(duration-t)/.1))
        x=amplitude*np.sin(2*np.pi*freq*t+np.pi/4)*fade
        p=Path(folder)/(name+'.wav');sf.write(p,np.column_stack((x,x)),sr,subtype='FLOAT')
        m=measure(p)
        reference=run(['/usr/bin/ffmpeg','-hide_banner','-nostdin','-i',p,'-af','ebur128=peak=true','-f','null','-'])
        summary=reference[reference.rfind('Summary:'):]
        lufs=float(re.search(r'I:\s+([-\d.]+) LUFS',summary).group(1))
        assert abs(m['lufs_i']-lufs)<.11,(m,lufs)
        assert abs(m['tp8_dbtp']-db(amplitude))<.01,m
        assert abs(m['tp4_dbtp']-m['tp8_dbtp'])<.01,m
        if name=='intersample':
            assert m['sample_clip_count']==0 and m['intersample_clip8_count']>0,m
        rows.append({'case':name,'analytic_tp_dbtp':db(amplitude),'ffmpeg_ebur128':summary,'measurement':m})
from scipy.signal import resample_poly, firwin
from scipy.fft import rfft, irfft, next_fast_len
rng = np.random.default_rng(85)
x = rng.normal(size=(48001, 2))
errors = {}
for factor in (4, 8):
    h = firwin(64 * factor + 1, 1 / factor, window=('kaiser', 12.0))
    reference = resample_poly(x, factor, 1, axis=0, window=h)
    n = next_fast_len(len(x) + 64)
    spectrum = rfft(x, n=n, axis=0)
    error = 0
    for phase in range(factor):
        y = irfft(spectrum * rfft(h[phase::factor] * factor, n=n)[:, None], n=n, axis=0)[32:32 + len(x)]
        error = max(error, float(np.max(np.abs(y - reference[phase::factor]))))
    assert error < 1e-12, error
    errors[str(factor)] = error
print(json.dumps({'fixtures': rows, 'fft_vs_resample_poly_max_abs_error': errors}, indent=2))
