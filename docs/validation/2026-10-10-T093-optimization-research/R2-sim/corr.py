import csv, sys, statistics as st
base = sys.argv[1]
rt = list(csv.DictReader(open(base + '/codec_roundtrips.csv')))
rend = {}
for t in ('-10','-12','-14'):
    for r in csv.DictReader(open(base + f'/render_target_{t}.csv')):
        if r['kind']=='render':
            rend[(r['file'], float(t))] = r
rows=[]
for r in rt:
    key=(r['file'], float(r['target_lufs']))
    rr = rend.get(key)
    if not rr: continue
    rows.append((r['encoder'], r['bitrate_kbps'], r['file'][:20], float(r['overshoot8_db']), float(rr['limit_gr_mean_db'] or 'nan'), float(rr['limit_gr_max_db'] or 'nan'), float(r['pre_psr_median_db']), float(r['pre_crest_db'])))
def pearson(x,y):
    mx,my=st.mean(x),st.mean(y)
    sx=sum((a-mx)**2 for a in x)**.5; sy=sum((b-my)**2 for b in y)**.5
    return sum((a-mx)*(b-my) for a,b in zip(x,y))/(sx*sy)
import math
for enc in sorted(set((r[0],r[1]) for r in rows)):
    sub=[r for r in rows if (r[0],r[1])==enc and not r[2].startswith('stress') and not math.isnan(r[4])]
    o=[r[3] for r in sub]; g=[r[4] for r in sub]; gm=[r[5] for r in sub]; p=[r[6] for r in sub]; c=[r[7] for r in sub]
    print(enc, len(sub), 'r(over,grMean)=%.2f r(over,grMax)=%.2f r(over,PSR)=%.2f r(over,crest)=%.2f'%(pearson(o,g),pearson(o,gm),pearson(o,p),pearson(o,c)), 'over median %.2f'%st.median(o))
