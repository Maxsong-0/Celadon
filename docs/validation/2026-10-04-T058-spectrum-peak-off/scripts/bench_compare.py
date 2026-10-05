#!/usr/bin/env python3
"""把 --only spectrum-peak-bench 改前 / 改后各一份输出的「summary」段对比：PEAK 关的组合看重建曲线数、设备像素、频谱仪 CPU（推进 + 重画）；
PEAK 开的组合应与改前完全相同。用法：bench_compare.py <改前 txt> <改后 txt>"""
import re, sys

def parse(path):
    rows, on = {}, False
    for ln in open(path, encoding="utf-8"):
        if ln.startswith("==== summary"):
            on = True
            continue
        if ln.startswith("==== spectrum peak-off bench done"):
            break
        if not on:
            continue
        m = re.match(r"\s*(.+?)\s*\|\s*PEAK (off|on)\s*\|\s*(\d+) fps\s*\|\s*x(\d)\s*\|\s*fr\s+(\d+) dirty\s+(\d+) curves\s+(\d+)\s*\|\s*dev px\s+(\d+)\s*\|\s*advance wall\s+([\d.]+) cpu\s+([\d.]+).*?paint wall\s+([\d.]+) cpu\s+([\d.]+)", ln)
        if m:
            lab, pk, fps, sc, fr, dirty, curves, dev, aw, ac, pw, pc = m.groups()
            rows[(lab.strip(), pk, int(fps), int(sc))] = dict(dirty=int(dirty), curves=int(curves), dev=int(dev), cpu=float(ac) + float(pc))
    return rows

def main():
    b, a = parse(sys.argv[1]), parse(sys.argv[2])
    print(f"{'场景':16s}{'fps':>4s}{'倍':>3s} | {'重建曲线 前→后':>14s} | {'设备像素 前→后':>22s} | 频谱仪 CPU ms（推进 + 重画）前→后")
    for k in sorted(b):
        if k[1] == "off" and k in a:
            x, y = b[k], a[k]
            print(f"{k[0]:16s}{k[2]:4d}{k[3]:3d} | {x['curves']:6d} → {y['curves']:<5d} | {x['dev']/1e6:8.1f}M → {y['dev']/1e6:<8.1f}M | {x['cpu']:8.1f} → {y['cpu']:<8.1f} ({100*(x['cpu']-y['cpu'])/x['cpu']:3.0f} % 省)")
    same = [k for k in b if k[1] == "on" and k in a and b[k]["curves"] == a[k]["curves"] and b[k]["dirty"] == a[k]["dirty"] and b[k]["dev"] == a[k]["dev"]]
    total = [k for k in b if k[1] == "on" and k in a]
    print(f"PEAK 开：曲线帧 / 脏帧 / 设备像素改前改后完全相同的组合 {len(same)} / {len(total)}")

main()
