import { useMemo } from 'react';
import { seeded } from '@/lib/utils';

export type CrackleLevel = 1 | 2 | 3;
export interface CrackleLine {
  level: CrackleLevel;
  d: string;
  /** 这条纹在 GLAZE 到多少（0 ～ 1）时开始出现 */
  appearsAt: number;
}

/**
 * 生成一组固定种子的开片纹（冰裂纹）：
 *  一级 = 贯穿瓷面的长纹（几条），二级 = 从一级上长出的分叉，三级 = 更短的细纹。
 *  和插件 Porcelain.h 一样，GLAZE 越大能看到的开片越多：每条纹带一个 appearsAt 阈值。
 */
export function generateCrackle(radius: number, seed = 7): CrackleLine[] {
  const rnd = seeded(seed);
  const lines: CrackleLine[] = [];
  const inside = (x: number, y: number) => x * x + y * y < radius * radius * 0.985;

  type Pt = [number, number];
  const walk = (start: Pt, angle: number, length: number, jitter: number, step: number): Pt[] => {
    const pts: Pt[] = [start];
    let [x, y] = start;
    let a = angle;
    let travelled = 0;
    while (travelled < length) {
      a += (rnd() - 0.5) * jitter;
      x += Math.cos(a) * step;
      y += Math.sin(a) * step;
      if (!inside(x, y)) break;
      pts.push([x, y]);
      travelled += step;
    }
    return pts;
  };
  const toPath = (pts: Pt[]) => pts.map((p, i) => `${i === 0 ? 'M' : 'L'}${p[0].toFixed(1)} ${p[1].toFixed(1)}`).join(' ');

  const level1: Pt[][] = [];
  const n1 = 7;
  for (let i = 0; i < n1; i++) {
    const a0 = (i / n1) * Math.PI * 2 + rnd() * 0.6;
    const r0 = radius * (0.15 + rnd() * 0.5);
    const start: Pt = [Math.cos(a0) * r0, Math.sin(a0) * r0];
    const dir = a0 + Math.PI * (0.35 + rnd() * 0.3) * (rnd() > 0.5 ? 1 : -1);
    const pts = walk(start, dir, radius * (1.1 + rnd() * 0.8), 0.55, radius * 0.06);
    if (pts.length > 3) {
      level1.push(pts);
      lines.push({ level: 1, d: toPath(pts), appearsAt: 0.04 + i * 0.05 });
    }
  }

  const level2: Pt[][] = [];
  for (const parent of level1) {
    const branches = 2 + Math.floor(rnd() * 3);
    for (let b = 0; b < branches; b++) {
      const idx = 1 + Math.floor(rnd() * (parent.length - 2));
      const [x0, y0] = parent[idx];
      const [x1, y1] = parent[Math.min(parent.length - 1, idx + 1)];
      const tangent = Math.atan2(y1 - y0, x1 - x0);
      const dir = tangent + (Math.PI / 2 + (rnd() - 0.5) * 0.7) * (rnd() > 0.5 ? 1 : -1);
      const pts = walk([x0, y0], dir, radius * (0.3 + rnd() * 0.45), 0.7, radius * 0.05);
      if (pts.length > 2) {
        level2.push(pts);
        lines.push({ level: 2, d: toPath(pts), appearsAt: 0.3 + rnd() * 0.3 });
      }
    }
  }

  for (const parent of level2) {
    const branches = Math.floor(rnd() * 3);
    for (let b = 0; b < branches; b++) {
      const idx = Math.floor(rnd() * (parent.length - 1));
      const [x0, y0] = parent[idx];
      const [x1, y1] = parent[Math.min(parent.length - 1, idx + 1)];
      const tangent = Math.atan2(y1 - y0, x1 - x0);
      const dir = tangent + (Math.PI / 2 + (rnd() - 0.5) * 0.9) * (rnd() > 0.5 ? 1 : -1);
      const pts = walk([x0, y0], dir, radius * (0.12 + rnd() * 0.25), 0.9, radius * 0.04);
      if (pts.length > 2) lines.push({ level: 3, d: toPath(pts), appearsAt: 0.6 + rnd() * 0.35 });
    }
  }
  return lines;
}

/**
 * 开片纹 SVG 组：中心在 (0, 0)。value 0 ～ 1 决定露出多少条、每条露出多长。
 * 每条纹用 pathLength=1 + dashoffset 露出，不重算路径。
 */
export default function Crackle({
  radius,
  value,
  seed = 7,
  color = '#0E6473',
  opacity = 0.55
}: {
  radius: number;
  value: number;
  seed?: number;
  color?: string;
  opacity?: number;
}) {
  const lines = useMemo(() => generateCrackle(radius, seed), [radius, seed]);
  return (
    <g fill="none" stroke={color} strokeLinecap="round" strokeLinejoin="round" opacity={opacity}>
      {lines.map((l, i) => {
        const span = l.level === 1 ? 0.22 : l.level === 2 ? 0.18 : 0.14;
        const reveal = Math.min(1, Math.max(0, (value - l.appearsAt) / span));
        if (reveal <= 0) return null;
        return (
          <path
            key={i}
            d={l.d}
            pathLength={1}
            strokeWidth={l.level === 1 ? 1.1 : l.level === 2 ? 0.7 : 0.45}
            strokeOpacity={l.level === 1 ? 1 : l.level === 2 ? 0.8 : 0.6}
            style={{ strokeDasharray: 1, strokeDashoffset: 1 - reveal, transition: 'stroke-dashoffset 600ms cubic-bezier(0.32,0.72,0,1)' }}
          />
        );
      })}
    </g>
  );
}
