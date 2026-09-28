import csv, glob, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cols import rows as read_rows
PH={0:'unlearned',1:'listen',2:'planTr',3:'calib',4:'locked'}
def summ(path):
    rows=list(read_rows(path))
    last=rows[-1]
    begins=[];fins=[];fbs=[];guard_blocks=0;blocks=0;never=0
    pb=pf=pfb=0
    first_prof=None
    for r in rows:
        b=int(r['cal_begins']);f=int(r['cal_finishes']);fb=int(r['feedback_steps'])
        t=float(r['t_sec'])
        if b>pb: begins.append(round(t,1))
        if f>pf: fins.append((round(t,1), r['cal_applicable']))
        if fb>pfb: fbs.append(round(t,1))
        pb,pf,pfb=b,f,fb
        if first_prof is None and not (int(r['cal_gate_mask'])&1) and int(r['glaze_phase'])!=1: first_prof=round(t,1)
    return dict(end=PH[int(last['glaze_phase'])], valid=last['cal_applicable'], begins=begins, fins=fins, fbs=fbs, dur=round(float(last['t_sec']),1), prof=first_prof)
for f in sorted(glob.glob(sys.argv[1]+"/*/trace/*.csv")):
    name=os.path.basename(f).replace('_clay-off_auto','')
    s=summ(f)
    print(f"{name[:34]:34} dur={s['dur']:6} prof@{s['prof']} end={s['end']:7} v={s['valid']} begins={s['begins']} fins={s['fins']} fb={s['fbs']}")
