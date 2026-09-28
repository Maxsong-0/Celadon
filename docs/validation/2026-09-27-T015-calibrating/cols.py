# 追踪列名：诊断期间（run1 修前 / run3 修后）用的是临时的 dbg_* 列名，定稿后改成 README「逐块追踪的校准列」里的名字。
# 读入时统一成定稿名，两种追踪都能分析。
OLD_TO_NEW = {
    'dbg_phase': 'glaze_phase', 'dbg_round_active': 'cal_round_active', 'dbg_cal_state': 'cal_state',
    'dbg_block_mask': 'cal_interrupt_mask', 'dbg_gate_mask': 'cal_gate_mask', 'dbg_cal_round': 'cal_rounds_done',
    'dbg_cal_subwin': 'cal_subwindows', 'dbg_cal_subwin_s': 'cal_subwindow_s', 'dbg_cal_guard_s': 'cal_guard_s',
    'dbg_cal_last_err': 'cal_last_error_db', 'dbg_cal_suggested': 'cal_suggested_db', 'dbg_cal_target': 'cal_target_db',
    'dbg_cal_valid': 'cal_applicable', 'dbg_applied_bias': 'applied_bias_db', 'dbg_executed_bias': 'executed_bias_db',
    'dbg_guard_db': 'guard_db', 'dbg_transition': 'plan_transition', 'dbg_plan_transitions': 'plan_transitions',
    'dbg_cal_begins': 'cal_begins', 'dbg_cal_finishes': 'cal_finishes', 'dbg_feedback_steps': 'feedback_steps',
    'dbg_feedback_max': 'feedback_max_db',
}

def rows(path):
    import csv
    for r in csv.DictReader(open(path, encoding='utf-8')):
        yield {OLD_TO_NEW.get(k, k): v for k, v in r.items()}
