import { memo, useMemo } from 'react';

/**
 * 开片网：逐行移植插件 Source/ui/Porcelain.cpp 的 generate()（同一随机数、同一参数），
 * 所以网站旋钮上的开片与插件界面里的一模一样。
 *
 * 做法：从整块圆面开始，反复「按面积加权挑一块 → 在形心附近、垂直于它的主轴切一刀」，
 * 切线用中点位移分形抖成自然的裂纹，再整体弯一点。三级：12 条大裂、120 条中裂、260 条细裂。
 * GLAZE 越大露出越多：可见条数 = 总数 × t^0.9（与插件 visibleCrackCount 相同），先露大裂。
 */

type Vec = [number, number];
type Polygon = Vec[];

export interface Crack {
  level: 0 | 1 | 2;
  points: Vec[];
}

/** 插件里的瓷面半径（逻辑像素）；输出坐标按 radius / GLAZE_RADIUS 缩放。 */
const GLAZE_RADIUS = 86;

/** 设计稿的随机数：sd = (imul(sd, 1664525) + 1013904223) >>> 0；rnd = sd / 2^32。 */
function lcg(seed: number) {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
}

function polygonArea(p: Polygon) {
  let a = 0;
  for (let i = 0; i < p.length; i++) {
    const u = p[i];
    const v = p[(i + 1) % p.length];
    a += u[0] * v[1] - v[0] * u[1];
  }
  return Math.abs(a) * 0.5;
}

function centroid(p: Polygon): Vec {
  let x = 0;
  let y = 0;
  for (const v of p) {
    x += v[0];
    y += v[1];
  }
  return [x / p.length, y / p.length];
}

/** 主轴方向（二阶矩）：新裂线垂直于它，块才会越切越接近等大的多边形。 */
function principalAxis(p: Polygon, c: Vec) {
  let sxx = 0;
  let syy = 0;
  let sxy = 0;
  for (const v of p) {
    const dx = v[0] - c[0];
    const dy = v[1] - c[1];
    sxx += dx * dx;
    syy += dy * dy;
    sxy += dx * dy;
  }
  return 0.5 * Math.atan2(2 * sxy, sxx - syy);
}

/** 中点位移分形：递归 depth 层，每层幅度 × 0.52，结果按顺序追加到 acc。 */
function fractal(a: Vec, b: Vec, amp: number, depth: number, rng: () => number, acc: Polygon) {
  if (depth === 0) return;
  const dx = b[0] - a[0];
  const dy = b[1] - a[1];
  const length = Math.hypot(dx, dy) || 1;
  const offset = (rng() - 0.5) * 2 * amp;
  const m: Vec = [(a[0] + b[0]) * 0.5 - (dy / length) * offset, (a[1] + b[1]) * 0.5 + (dx / length) * offset];
  fractal(a, m, amp * 0.52, depth - 1, rng, acc);
  acc.push(m);
  fractal(m, b, amp * 0.52, depth - 1, rng, acc);
}

// 与插件 levels[] 相同：count、bias、jitter、spread、amplitude、depth、minLength
const LEVELS = [
  { count: 12, bias: 1.7, jitter: 0.25, spread: 0.5, amplitude: 0.045, depth: 5, minLength: 14 },
  { count: 120, bias: 1.4, jitter: 0.3, spread: 0.9, amplitude: 0.055, depth: 4, minLength: 6 },
  { count: 260, bias: 1.15, jitter: 0.34, spread: 1.2, amplitude: 0.06, depth: 3, minLength: 3 }
] as const;

function generate(): Crack[] {
  const rng = lcg(0x51a7c0de);
  const rim: Polygon = Array.from({ length: 160 }, (_, i) => {
    const angle = (i / 160) * Math.PI * 2;
    return [GLAZE_RADIUS * Math.cos(angle), GLAZE_RADIUS * Math.sin(angle)];
  });
  const cells: { polygon: Polygon; area: number }[] = [{ polygon: rim, area: polygonArea(rim) }];
  const cracks: Crack[] = [];

  const splitOnce = (levelIndex: 0 | 1 | 2) => {
    const level = LEVELS[levelIndex];
    // 按「面积^bias」加权随机选一块
    const weights = cells.map(c => Math.pow(c.area, level.bias));
    const total = weights.reduce((a, b) => a + b, 0);
    let r = rng() * total;
    let chosen = 0;
    for (; chosen + 1 < cells.length; chosen++) {
      r -= weights[chosen];
      if (r <= 0) break;
    }

    const polygon = cells[chosen].polygon;
    const area = cells[chosen].area;
    const c0 = centroid(polygon);
    const c: Vec = [c0[0] + (rng() - 0.5) * Math.sqrt(area) * level.jitter, c0[1] + (rng() - 0.5) * Math.sqrt(area) * level.jitter];
    const theta = principalAxis(polygon, c0) + Math.PI / 2 + (rng() - 0.5) * level.spread;
    const dx = Math.cos(theta);
    const dy = Math.sin(theta);
    const side = (p: Vec) => (p[0] - c[0]) * dy - (p[1] - c[1]) * dx;

    const left: Polygon = [];
    const right: Polygon = [];
    const hits: Vec[] = [];
    const hitLeft = [-1, -1];
    const hitRight = [-1, -1];

    for (let i = 0; i < polygon.length; i++) {
      const p = polygon[i];
      const q = polygon[(i + 1) % polygon.length];
      const sp = side(p);
      const sq = side(q);
      (sp >= 0 ? left : right).push(p);
      if (sp >= 0 !== sq >= 0) {
        const u = sp / (sp - sq);
        const x: Vec = [p[0] + (q[0] - p[0]) * u, p[1] + (q[1] - p[1]) * u];
        if (hits.length < 2) {
          hitLeft[hits.length] = left.length;
          hitRight[hits.length] = right.length;
        }
        left.push(x);
        right.push(x);
        hits.push(x);
      }
    }

    if (hits.length !== 2 || left.length < 3 || right.length < 3) return;

    const [h0, h1] = hits;
    const length = Math.hypot(h1[0] - h0[0], h1[1] - h0[1]);
    if (length < level.minLength) return;

    const mids: Polygon = [];
    const nx = -(h1[1] - h0[1]) / length;
    const ny = (h1[0] - h0[0]) / length;
    const bend = (rng() - 0.5) * length * 0.12;
    fractal(h0, h1, length * level.amplitude, level.depth, rng, mids);

    // 整条裂线再整体弯一点（中间最弯）：真瓷器的开片很少是直的
    for (let i = 0; i < mids.length; i++) {
      const u = (i + 1) / (mids.length + 1);
      const b = bend * 4 * u * (1 - u);
      mids[i] = [mids[i][0] + nx * b, mids[i][1] + ny * b];
    }

    // 弯折后的裂线同时成为两个新块的边（沿各自多边形的顶点顺序插进去）
    const bendIn = (shape: Polygon, i0: number, i1: number) => {
      const n = shape.length;
      if ((i0 + 1) % n === i1) shape.splice(i0 + 1, 0, ...mids);
      else if ((i1 + 1) % n === i0) shape.splice(i1 + 1, 0, ...[...mids].reverse());
    };
    bendIn(left, hitLeft[0], hitLeft[1]);
    bendIn(right, hitRight[0], hitRight[1]);

    cells[chosen] = { polygon: left, area: polygonArea(left) };
    cells.splice(chosen + 1, 0, { polygon: right, area: polygonArea(right) });

    const points: Vec[] = [h0, ...mids, h1];
    if (rng() >= 0.5) points.reverse();
    cracks.push({ level: levelIndex, points });
  };

  ([0, 1, 2] as const).forEach(levelIndex => {
    const goal = cracks.length + LEVELS[levelIndex].count;
    let attempts = 0;
    while (cracks.length < goal && attempts++ < LEVELS[levelIndex].count * 12) splitOnce(levelIndex);
  });
  return cracks;
}

let network: Crack[] | null = null;
/** 整张开片网只生成一次（与插件的 static 一样）。 */
export function crackNetwork(): Crack[] {
  network ??= generate();
  return network;
}

/** 与插件 visibleCrackCount 相同：round(总数 × t^0.9)。 */
export function visibleCrackCount(t: number) {
  const x = Math.min(1, Math.max(0, Number.isFinite(t) ? t : 0));
  return Math.round(crackNetwork().length * Math.pow(x, 0.9));
}

// 插件的线宽（逻辑像素，瓷面半径 86）与不透明度；网站旋钮更大，不透明度略提一点让开片看得见。
const WIDTHS = [0.48, 0.34, 0.24];
const OPACITIES = [0.21, 0.15, 0.1];

/**
 * 开片纹 SVG 组：中心在 (0, 0)，按 radius 缩放。value 0 ～ 1。
 * 每一级合成一条 path（最多 3 个元素），数值变化时只重拼字符串，不增删上百个节点。
 */
function Crackle({ radius, value, color = '#4B7166', boost = 1.5 }: { radius: number; value: number; color?: string; boost?: number }) {
  const visible = visibleCrackCount(value);
  const scale = radius / GLAZE_RADIUS;
  const paths = useMemo(() => {
    const d = ['', '', ''];
    const cracks = crackNetwork();
    for (let i = 0; i < visible; i++) {
      const c = cracks[i];
      d[c.level] += c.points.map((p, k) => `${k === 0 ? 'M' : 'L'}${(p[0] * scale).toFixed(1)} ${(p[1] * scale).toFixed(1)}`).join('');
    }
    return d;
  }, [visible, scale]);
  // 插件：alpha = 不透明度 × (0.8 + 0.2 t)
  const k = (0.8 + 0.2 * value) * boost;
  return (
    <g fill="none" stroke={color} strokeLinecap="round" strokeLinejoin="round">
      {paths.map((d, level) =>
        d ? <path key={level} d={d} strokeWidth={WIDTHS[level] * scale} strokeOpacity={Math.min(1, OPACITIES[level] * k)} /> : null
      )}
    </g>
  );
}

export default memo(Crackle);
