#!/usr/bin/env python3
# T-048 阶段 2：gate_check.py / house_gate.py 的连续编排器判定。不编译插件。
# 旧表（没有 confidence，或格子是空的）走阶段 1 的锁定 / 校准偏差；有 confidence 就看 c ≥ 0.5、不再加校准偏差。
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gate_check  # noqa: E402
import house_gate  # noqa: E402


BASE = (
    "kind,song,glaze,clay,style,lufs,true_peak_dbtp,audio_hash,glaze_phase,calibration_valid,"
    "phase_guard_pct,slow_inverted_pct,replay_check,music_seconds,limit_input_peak_db"
)


def write_csv(path, rows):
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(BASE + "\n")
        for row in rows:
            handle.write(",".join(row) + "\n")


def run_gate(rows, extra_header="", extra_cells=""):
    header = BASE + extra_header
    body = []
    for row in rows:
        body.append(",".join(row) + extra_cells)
    text = header + "\n" + "\n".join(body) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, encoding="utf-8") as handle:
        handle.write(text)
        path = handle.name
    result = subprocess.run([sys.executable, str(HERE / "gate_check.py"), path], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def input_row():
    return ["input", "song", "", "", "", "-14", "-1.2", "aaaa", "", "", "", "", "", "40", ""]


def render(glaze, clay, phase, valid):
    same = glaze == "0"
    return ["render", "song", glaze, clay, "auto", "-10", "-1.2",
            "bbbb" if same or clay == "auto" else "cccc",
            phase, valid, "0", "0", "ok" if clay == "auto" and glaze != "0" else "", "40", "-1"]


def main():
    failures = []

    def check(name, cond, detail=""):
        if cond:
            print(f"ok  {name}")
        else:
            failures.append(name)
            print(f"FAIL {name} {detail}")

    # 阶段 1：锁定 + 校准有效，没有 confidence 列。
    code, out = run_gate([
        input_row(),
        render("0", "auto", "locked", "0"),
        render("0", "off", "locked", "0"),
        render("70", "auto", "locked", "1"),
        render("70", "off", "locked", "1"),
    ])
    check("legacy locked passes", code == 0, out)
    check("legacy output still says lock", "lock(auto/off)" in out and " L/L " in out, out)

    code, out = run_gate([
        input_row(),
        render("70", "auto", "calibrating", "0"),
        render("70", "off", "locked", "1"),
    ])
    check("legacy unlocked fails", code == 1 and "校准无效" in out, out)

    # 空 confidence 列 = 还没填，退回阶段 1。
    code, out = run_gate([
        input_row(),
        render("70", "auto", "locked", "1"),
        render("70", "off", "locked", "1"),
    ], ",confidence", ",")
    check("blank confidence uses the lock rule", code == 0 and " L/L " in out, out)

    # 有 c：不再要求 locked。
    code, out = run_gate([
        input_row(),
        render("70", "auto", "unlearned", "0"),
        render("70", "off", "listening", "0"),
    ], ",confidence", ",0.80")
    check("confidence 0.8 passes without a lock", code == 0 and " C/C " in out, out)

    code, out = run_gate([
        input_row(),
        render("70", "auto", "locked", "1"),
        render("70", "off", "locked", "1"),
    ], ",confidence", ",0.20")
    check("confidence 0.2 fails a long song", code == 1 and "confidence 0.200" in out, out)

    # 短歌：c 不够只说明，不失败。
    short = input_row()
    short[-2] = "10"
    code, out = run_gate([
        short,
        render("70", "auto", "unlearned", "0"),
    ], ",confidence", ",0.20")
    check("short song with low confidence is a note", code == 0 and "时长不够热起" in out, out)

    # house_gate ①
    legacy_row = {"listen_from_bar": "1", "plan_gain_db": "1.0", "listen_phase": "locked",
                  "listen_calibration_valid": "1", "calibration_bias_db": "0.2"}
    legacy_partner = {"listen_from_bar": "33", "plan_gain_db": "1.1", "listen_phase": "locked",
                      "listen_calibration_valid": "1", "calibration_bias_db": "0.2"}
    verdict, note = house_gate.gate_plan_gain(legacy_row, [legacy_partner])
    check("legacy plan gain adds calibration bias", verdict == "PASS" and "方案 + 校准" in note, note)

    unlocked = dict(legacy_partner)
    unlocked["listen_phase"] = "listening"
    verdict, note = house_gate.gate_plan_gain(legacy_row, [unlocked])
    check("legacy plan gain still requires locked", verdict == "FAIL" and "没都锁定" in note, note)

    live_row = {"listen_from_bar": "1", "plan_gain_db": "1.0", "confidence": "0.8",
                "listen_phase": "listening", "listen_calibration_valid": "1", "calibration_bias_db": "5"}
    live_partner = {"listen_from_bar": "33", "plan_gain_db": "1.2", "confidence": "0.9",
                    "listen_phase": "listening", "calibration_bias_db": "0"}
    verdict, note = house_gate.gate_plan_gain(live_row, [live_partner])
    check("continuous plan gain ignores calibration bias", verdict == "PASS" and "方案增益" in note and "+0.20" in note, note)

    cold = dict(live_row)
    cold["confidence"] = "0.2"
    verdict, note = house_gate.gate_plan_gain(cold, [live_partner])
    check("continuous plan gain requires both sides warm", verdict == "FAIL" and "主段还没热" in note, note)

    check("gate_check confidence helper treats blank as absent",
          gate_check.phase2_confidence({"confidence": "  "}) is None
          and gate_check.phase2_confidence({"confidence": "0.5"}) == 0.5)

    if failures:
        print(f"\n{len(failures)} failed")
        return 1
    print("\nphase2 gate self-check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
