#!/usr/bin/env python3
# 把逐块追踪压成「状态段」：相位 / 轮 / 校准器状态 / 作废原因 / 开轮门 变化时打印一行。
import csv, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cols import rows as read_rows

PH = {0: 'unlearned', 1: 'listen', 2: 'planTr', 3: 'calib', 4: 'locked'}
CS = {0: 'idle', 1: 'settle', 2: 'collect', 3: 'finish'}

def bits(m, names):
    return '+'.join(n for i, n in enumerate(names) if m & (1 << i)) or '-'

BLOCK = ['paused', '!active', 'trans', '!match', 'manual', 'guard', 'bias']
GATE = ['noProf', '!active', 'paused', 'trans', 'manual', '!guardSettled', '!refKnown', 'applicable', '!stable', 'sameAttempt']

def main(path, maxrows=400):
    rows = list(read_rows(path))
    last = None
    n = 0
    for r in rows:
        key = (r['glaze_phase'], r['cal_round_active'], r['cal_state'], r['cal_interrupt_mask'], r['cal_gate_mask'],
               r['cal_rounds_done'], r['cal_subwindows'], r['cal_applicable'], r['plan_transition'], r['cal_begins'])
        if key != last:
            last = key
            n += 1
            if n > maxrows:
                print('...'); break
            print(f"{float(r['t_sec']):8.2f} {PH[int(r['glaze_phase'])]:9} rnd={r['cal_round_active']} cal={CS[int(r['cal_state'])]:7} "
                  f"blk={bits(int(r['cal_interrupt_mask']), BLOCK):14} gate={bits(int(r['cal_gate_mask']), GATE):28} "
                  f"r{r['cal_rounds_done']} sw{r['cal_subwindows']} {r['cal_subwindow_s']:>5}s err={r['cal_last_error_db']:>7} "
                  f"sug={r['cal_suggested_db']:>6} tgt={r['cal_target_db']:>6} valid={r['cal_applicable']} "
                  f"bias={r['executed_bias_db']:>7} grd={r['guard_db']:>6} gs={r['cal_guard_s']:>5} tr={r['plan_transition']} "
                  f"nTr={r['plan_transitions']} beg={r['cal_begins']} fin={r['cal_finishes']} fb={r['feedback_steps']} fbMax={r['feedback_max_db']}")

if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 400)
