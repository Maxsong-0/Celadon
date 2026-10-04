# P1R 常数扫描（HEAD 的 celadon_render，--live-constants；Jamendo 38 首可算 + 合成两首）

门槛：第 1 条 M2 中位 ≤ 0.3、P90 ≤ 1.0；第 2 条每段 breakdown ≤ 0.5；第 3 条每首 |δ4| ≤ 1（✗ = 第 1 条不达标）。

| 组 | 常数 | M1 中位 / P90 | M2（≥16） | M2b | M3 P90 / 最大（达标段） | M4 达标首 / \|δ4\| 中位 / P90 | n ≤ 4 | 跟踪误差 > 1 的首 | c 到 0.8 中位 / P90 s | c 掉了的首 / κ 比最小 | intro / drop / bd m≥0.5 首判 % | 换歌 / 衰减 首 | 合成两首 M4 达标 / M2 中位 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| g20-l45-c8 | fourOnFloor.gateWidthLu=2,fourOnFloor.lowGateFactor=0.45 | 0.28 / 0.83 | 0.29 / 0.89 | 0.30 / 0.75 | 0.17 / 0.31（31 / 31） | 21 / 0.91 / 2.32 | 20 | 7 | 46.2 / 75.2 | 17 / 0.51 | 73 / 84 / 18 | 0 / 7 | 2 / 0.10 |
| g20-l50-c8 | fourOnFloor.gateWidthLu=2,fourOnFloor.lowGateFactor=0.5 | 0.29 / 0.81 | 0.29 / 0.88 | 0.30 / 0.75 | 0.16 / 0.32（31 / 31） | 21 / 0.91 / 2.31 | 20 | 7 | 46.2 / 76.2 | 17 / 0.51 | 73 / 84 / 17 | 0 / 7 | 2 / 0.10 |
| g20-l60-c8 | fourOnFloor.gateWidthLu=2 | 0.32 / 0.77 | 0.29 / 0.87 | 0.30 / 0.75 | 0.17 / 0.32（31 / 31） | 21 / 0.87 / 2.36 | 21 | 7 | 47.9 / 78.3 | 17 / 0.51 | 72 / 82 / 17 | 0 / 9 | 2 / 0.10 |
| g25-l45-c8 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.lowGateFactor=0.45 | 0.22 / 0.54 | 0.26 / 0.74 | 0.25 / 0.73 | 0.18 / 0.42（31 / 31） | 21 / 0.88 / 2.28 | 20 | 8 | 42.3 / 73.1 | 16 / 0.46 | 77 / 89 / 19 | 0 / 4 | 2 / 0.11 |
| g25-l50-c8 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.lowGateFactor=0.5 | 0.22 / 0.53 | 0.26 / 0.73 | 0.25 / 0.72 | 0.18 / 0.42（31 / 31） | 22 / 0.88 / 2.35 | 20 | 8 | 42.3 / 73.1 | 16 / 0.46 | 76 / 88 / 19 | 0 / 5 | 2 / 0.11 |
| g30-l45-c8 | fourOnFloor.lowGateFactor=0.45 | 0.15 / 0.44 | 0.23 / 0.81 | 0.22 / 0.70 | 0.18 / 0.47（31 / 31） | 20 / 0.88 / 2.34 | 19 | 10 | 39.8 / 68.9 | 15 / 0.45 | 80 / 91 / 21 | 0 / 4 | 2 / 0.12 |
| g30-l50-c8 | fourOnFloor.lowGateFactor=0.5 | 0.16 / 0.44 | 0.23 / 0.81 | 0.22 / 0.69 | 0.18 / 0.48（31 / 31） | 21 / 0.88 / 2.42 | 19 | 10 | 40.5 / 70.2 | 15 / 0.45 | 79 / 90 / 21 | 0 / 5 | 2 / 0.12 |
| g20-l45-c6 | fourOnFloor.gateWidthLu=2,fourOnFloor.lowGateFactor=0.45,fourOnFloor.confidenceBars=6 | 0.28 / 0.83 | 0.29 / 0.89 | 0.30 / 0.75 | 0.17 / 0.31（31 / 31） | 21 / 0.91 / 2.32 | 20 | 7 | 33.8 / 65.9 | 17 / 0.49 | 73 / 84 / 18 | 0 / 7 | 2 / 0.10 |
| g20-l50-c6 | fourOnFloor.gateWidthLu=2,fourOnFloor.lowGateFactor=0.5,fourOnFloor.confidenceBars=6 | 0.29 / 0.81 | 0.29 / 0.88 | 0.30 / 0.75 | 0.16 / 0.32（31 / 31） | 21 / 0.91 / 2.31 | 20 | 7 | 33.8 / 66.5 | 17 / 0.49 | 73 / 84 / 17 | 0 / 7 | 2 / 0.10 |
| g20-l60-c6 | fourOnFloor.gateWidthLu=2,fourOnFloor.confidenceBars=6 | 0.32 / 0.77 | 0.29 / 0.87 | 0.30 / 0.75 | 0.17 / 0.32（31 / 31） | 21 / 0.87 / 2.36 | 21 | 7 | 34.2 / 66.5 | 17 / 0.49 | 72 / 82 / 17 | 0 / 9 | 2 / 0.10 |
| g25-l45-c6 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.lowGateFactor=0.45,fourOnFloor.confidenceBars=6 | 0.22 / 0.54 | 0.26 / 0.74 | 0.25 / 0.73 | 0.18 / 0.42（31 / 31） | 21 / 0.88 / 2.28 | 20 | 8 | 30.9 / 58.8 | 16 / 0.47 | 77 / 89 / 19 | 0 / 4 | 2 / 0.11 |
| g25-l50-c6 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.lowGateFactor=0.5,fourOnFloor.confidenceBars=6 | 0.22 / 0.53 | 0.26 / 0.73 | 0.25 / 0.72 | 0.18 / 0.42（31 / 31） | 22 / 0.88 / 2.35 | 20 | 8 | 30.9 / 58.8 | 16 / 0.47 | 76 / 88 / 19 | 0 / 5 | 2 / 0.11 |
| g25-l60-c6 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.confidenceBars=6 | 0.22 / 0.49 | 0.28 / 0.73 | 0.27 / 0.73 | 0.18 / 0.43（31 / 31） | 21 / 0.90 / 2.47 | 19 | 9 | 32.4 / 62.6 | 16 / 0.47 | 75 / 85 / 19 | 0 / 6 | 2 / 0.11 |
| g30-l45-c6 | fourOnFloor.lowGateFactor=0.45,fourOnFloor.confidenceBars=6 | 0.15 / 0.44 | 0.23 / 0.81 | 0.22 / 0.70 | 0.18 / 0.47（31 / 31） | 20 / 0.88 / 2.34 | 19 | 10 | 28.9 / 48.3 | 14 / 0.47 | 80 / 91 / 21 | 0 / 4 | 2 / 0.12 |
| g30-l50-c6 | fourOnFloor.lowGateFactor=0.5,fourOnFloor.confidenceBars=6 | 0.16 / 0.44 | 0.23 / 0.81 | 0.22 / 0.69 | 0.18 / 0.48（31 / 31） | 21 / 0.88 / 2.42 | 19 | 10 | 29.9 / 56.2 | 15 / 0.47 | 79 / 90 / 21 | 0 / 5 | 2 / 0.12 |
| g25-l50-c8-rp98 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.lowGateFactor=0.5,fourOnFloor.referencePercentile=0.98 | 0.26 / 0.74 | 0.28 / 0.84 | 0.28 / 0.76 | 0.17 / 0.42（31 / 31） | 24 / 0.88 / 2.30 | 22 | 5 | 44.1 / 79.7 | 14 / 0.45 | 74 / 86 / 19 | 0 / 5 | 2 / 0.11 |
| g25-l50-c8-lr1 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.lowGateFactor=0.5,fourOnFloor.loudnessRampLu=1 | 0.23 / 0.52 | 0.27 / 0.73 | 0.25 / 0.72 | 0.18 / 0.42（31 / 31） | 21 / 0.89 / 2.34 | 20 | 7 | 42.3 / 73.1 | 16 / 0.45 | 76 / 88 / 19 | 0 / 5 | 2 / 0.12 |
| g25-l50-c8-rm90 | fourOnFloor.gateWidthLu=2.5,fourOnFloor.lowGateFactor=0.5,referenceMemorySeconds=90 | 0.20 / 0.51 | 0.26 / 0.73 | 0.26 / 0.72 | 0.18 / 0.42（31 / 31） | 22 / 0.88 / 2.21 | 20 | 8 | 42.8 / 73.1 | 16 / 0.45 | 76 / 88 / 19 | 0 / 5 | 2 / 0.11 |
