import { useId, type JSX, type ReactNode } from 'react';

/*
  算法页与说明页里的示意图（`![说明](figure:key)`）。纯 SVG。
  每张图两套排版：md 起横排（viewBox 宽 720），手机竖排（viewBox 宽 340）；两套字号都按「渲染出来 10～14 px」定。
  SVG 文字不会自动换行：每段字都按字宽留足了位置；长说明不进图（正文里都有），需要图例的放在图下的 HTML 里，随宽度换行。
  颜色（设计规格-官网）：青 = 信号与模块，紫只表示 GLAZE 自动，橙只表示警告 / 被削掉的部分，灰 = 结构与读数。
*/

const INK = '#0D1B21';
const INK2 = '#4B5E66';
const INK3 = '#637780';
const INK4 = '#8A9CA3';
const LINE = '#DCE6E9';
const PANEL = '#FBFAF7';
const C100 = '#D5F1F5';
const C200 = '#A3E0E8';
const C300 = '#67CAD7';
const C400 = '#33B3C4';
const C500 = '#1799AB';
const C600 = '#127F90';
const C700 = '#0E6473';
const C800 = '#0A4A57';
const VIOLET = '#6A4DFF';
const VIOLET_TEXT = '#4B37C9';
const WARN = '#E8590C';
const WARN_TEXT = '#C2410C';
const SANS = 'var(--font-sans)';
const MONO = 'var(--font-mono)';

const WIDE_W = 720;
const NARROW_W = 340;

type Pt = readonly [number, number];
type Tone = 'c' | 'v' | 'g' | 'o';
type WireTone = 'ink' | 'v' | 'mute' | 'c';

/** 一套排版：横排或竖排，字号与箭头 marker 的 id 前缀。 */
interface V {
  narrow: boolean;
  /** 框标题字号 */
  T: number;
  /** 小字字号 */
  S: number;
  /** 等宽读数字号 */
  M: number;
  /** 框圆角 */
  r: number;
  mk: (tone: WireTone) => string;
}

const TONE: Record<Tone, { fill: string; stroke: string; title: string; sub: string; dash?: string }> = {
  c: { fill: '#E8F6F8', stroke: '#8ED3DD', title: INK, sub: INK3 },
  v: { fill: '#F0ECFF', stroke: '#BDB0FF', title: VIOLET_TEXT, sub: '#5E50C2' },
  g: { fill: '#F5F3EC', stroke: '#D9E2E4', title: INK, sub: INK3 },
  // 旁路监测（峰值守护这类）：虚线框，不是信号要走的一级
  o: { fill: PANEL, stroke: C400, title: C700, sub: INK3, dash: '4 3' }
};

const WIRE: Record<WireTone, string> = { ink: INK2, v: VIOLET, mute: INK4, c: C500 };

function variant(narrow: boolean, prefix: string): V {
  const mk = (tone: WireTone) => `url(#${prefix}-${tone})`;
  return narrow ? { narrow, T: 13.5, S: 11.5, M: 11, r: 9, mk } : { narrow, T: 14, S: 12, M: 12, r: 10, mk };
}

const r1 = (n: number) => Math.round(n * 10) / 10;

/** 折线连线，拐角做成小圆角（二次贝塞尔）。 */
function wirePath(pts: readonly Pt[], radius = 7): string {
  let d = `M${r1(pts[0][0])} ${r1(pts[0][1])}`;
  for (let i = 1; i < pts.length - 1; i++) {
    const [x0, y0] = pts[i - 1];
    const [x1, y1] = pts[i];
    const [x2, y2] = pts[i + 1];
    const l1 = Math.hypot(x1 - x0, y1 - y0);
    const l2 = Math.hypot(x2 - x1, y2 - y1);
    const rr = Math.min(radius, l1 / 2, l2 / 2);
    const ax = x1 - ((x1 - x0) / l1) * rr;
    const ay = y1 - ((y1 - y0) / l1) * rr;
    const bx = x1 + ((x2 - x1) / l2) * rr;
    const by = y1 + ((y2 - y1) / l2) * rr;
    d += ` L${r1(ax)} ${r1(ay)} Q${r1(x1)} ${r1(y1)} ${r1(bx)} ${r1(by)}`;
  }
  const [xl, yl] = pts[pts.length - 1];
  return `${d} L${r1(xl)} ${r1(yl)}`;
}

function Markers({ prefix }: { prefix: string }) {
  return (
    <defs>
      {(Object.keys(WIRE) as WireTone[]).map(k => (
        <marker key={k} id={`${prefix}-${k}`} viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">
          <path d="M0 1.2 L10 5 L0 8.8 z" fill={WIRE[k]} />
        </marker>
      ))}
    </defs>
  );
}

function Wire({ v, pts, tone = 'ink', dashed = false, head = true }: { v: V; pts: readonly Pt[]; tone?: WireTone; dashed?: boolean; head?: boolean }) {
  return (
    <path
      d={wirePath(pts)}
      fill="none"
      stroke={WIRE[tone]}
      strokeWidth={1.2}
      strokeDasharray={dashed ? '4 3' : undefined}
      strokeLinejoin="round"
      markerEnd={head ? v.mk(tone) : undefined}
    />
  );
}

/** 连线的分叉点 */
function Dot({ x, y, tone = 'ink' }: { x: number; y: number; tone?: WireTone }) {
  return <circle cx={x} cy={y} r={2.2} fill={WIRE[tone]} />;
}

function Label({
  v,
  x,
  y,
  children,
  color = INK3,
  anchor = 'start',
  mono = false,
  weight
}: {
  v: V;
  x: number;
  y: number;
  children: ReactNode;
  color?: string;
  anchor?: 'start' | 'middle' | 'end';
  mono?: boolean;
  weight?: number;
}) {
  return (
    <text x={r1(x)} y={r1(y)} textAnchor={anchor} fontSize={mono ? v.M : v.S} fontWeight={weight} fill={color} style={{ fontFamily: mono ? MONO : SANS }}>
      {children}
    </text>
  );
}

interface BoxSpec {
  x: number;
  y: number;
  w: number;
  h: number;
  title: string;
  sub?: string | readonly string[];
  tone?: Tone;
}

function Box({ v, b }: { v: V; b: BoxSpec }) {
  const t = TONE[b.tone ?? 'c'];
  const subs = b.sub === undefined ? [] : typeof b.sub === 'string' ? [b.sub] : b.sub;
  // 标题一行 + 若干行小字，整块在框内垂直居中；字的视觉中心约在基线上 0.35 em
  const gap = subs.length ? v.S * 0.42 : 0;
  const lead = v.S * 1.4;
  const total = v.T + (subs.length ? gap + v.S + (subs.length - 1) * lead : 0);
  const top = b.y + (b.h - total) / 2;
  const cx = b.x + b.w / 2;
  return (
    <g>
      <rect x={b.x} y={b.y} width={b.w} height={b.h} rx={v.r} fill={t.fill} stroke={t.stroke} strokeDasharray={t.dash} />
      <text x={r1(cx)} y={r1(top + v.T * 0.85)} textAnchor="middle" fontSize={v.T} fontWeight={600} fill={t.title} style={{ fontFamily: SANS }}>
        {b.title}
      </text>
      {subs.map((s, i) => (
        <text key={s} x={r1(cx)} y={r1(top + v.T + gap + i * lead + v.S * 0.85)} textAnchor="middle" fontSize={v.S} fill={t.sub} style={{ fontFamily: SANS }}>
          {s}
        </text>
      ))}
    </g>
  );
}

type Swatch = 'line' | 'thick' | 'dash' | 'bar' | 'frame' | 'band' | 'dot';
interface LegendItem {
  swatch: Swatch;
  color: string;
  label: string;
}

function swatchShape(swatch: Swatch, color: string): JSX.Element {
  switch (swatch) {
    case 'line':
      return <line x1="1" y1="5" x2="19" y2="5" stroke={color} strokeWidth="1.6" strokeLinecap="round" />;
    case 'thick':
      return <line x1="1" y1="5" x2="19" y2="5" stroke={color} strokeWidth="2.6" strokeLinecap="round" />;
    case 'dash':
      return <line x1="1" y1="5" x2="19" y2="5" stroke={color} strokeWidth="1.5" strokeDasharray="3 2.4" />;
    case 'bar':
      return <rect x="6" y="0.5" width="8" height="9" rx="1.5" fill={color} />;
    case 'frame':
      return <rect x="5.5" y="1" width="9" height="8" rx="1.5" fill={C100} stroke={color} strokeDasharray="2 1.5" />;
    case 'band':
      return <rect x="1" y="2" width="18" height="6" rx="1" fill={color} opacity="0.3" />;
    case 'dot':
      return <circle cx="10" cy="5" r="3" fill={color} />;
  }
}

function SwatchMark({ swatch, color }: { swatch: Swatch; color: string }) {
  return (
    <svg viewBox="0 0 20 10" className="h-2.5 w-5 shrink-0" aria-hidden="true">
      {swatchShape(swatch, color)}
    </svg>
  );
}

/** 图下的图例：HTML，随宽度换行。 */
function Legend({ items }: { items: readonly LegendItem[] }) {
  return (
    <div className="mt-5 flex flex-wrap justify-center gap-x-5 gap-y-2 border-t border-line pt-4 text-[12.5px] leading-snug text-ink-3">
      {items.map(it => (
        <span key={it.label} className="inline-flex items-center gap-2">
          <SwatchMark swatch={it.swatch} color={it.color} />
          {it.label}
        </span>
      ))}
    </div>
  );
}

/** 一张图：md 起显示横排，手机显示竖排；两套各自一份 marker（id 按实例唯一，同页多张图不串）。 */
function Figure({ label, wideH, narrowH, draw, legend }: { label: string; wideH: number; narrowH: number; draw: (v: V) => ReactNode; legend?: readonly LegendItem[] }) {
  const uid = useId().replace(/[^a-zA-Z0-9_-]/g, '');
  const wide = variant(false, `${uid}w`);
  const narrow = variant(true, `${uid}n`);
  return (
    <div>
      <svg viewBox={`0 0 ${WIDE_W} ${wideH}`} role="img" aria-label={label} className="mx-auto hidden h-auto w-full max-w-[760px] md:block">
        <Markers prefix={`${uid}w`} />
        {draw(wide)}
      </svg>
      <svg viewBox={`0 0 ${NARROW_W} ${narrowH}`} role="img" aria-label={label} className="mx-auto block h-auto w-full max-w-[400px] md:hidden">
        <Markers prefix={`${uid}n`} />
        {draw(narrow)}
      </svg>
      {legend ? <Legend items={legend} /> : null}
    </div>
  );
}

/* ---------------- 信号链 ---------------- */

const CHAIN_MODS = [
  { title: 'BALANCE', sub: '平衡' },
  { title: 'CLAY', sub: '修坯' },
  { title: 'COLOR', sub: '染色' },
  { title: 'LIMIT', sub: '限幅' }
] as const;

function ChainWide({ v }: { v: V }) {
  const y = 132;
  const h = 52;
  const mods = CHAIN_MODS.map((m, i) => ({ ...m, x: 110 + i * 132, y, w: 104, h }));
  return (
    <>
      {/* GLAZE：读入口特征与模块统计，经一条总线把实际参数交给四个模块 */}
      <Wire v={v} tone="v" dashed pts={[[50, 132], [50, 46], [240, 46]]} />
      <Label v={v} x={60} y={38} color={VIOLET_TEXT}>
        入口特征
      </Label>
      <Wire v={v} tone="mute" dashed pts={[[670, 132], [670, 46], [480, 46]]} />
      <Label v={v} x={660} y={38} anchor="end">
        模块统计
      </Label>
      <Wire v={v} tone="v" dashed head={false} pts={[[360, 72], [360, 102]]} />
      <Wire v={v} tone="v" dashed head={false} pts={[[162, 102], [558, 102]]} />
      {mods.map(m => (
        <Wire key={m.title} v={v} tone="v" dashed pts={[[m.x + m.w / 2, 102], [m.x + m.w / 2, 130]]} />
      ))}
      <Dot x={360} y={102} tone="v" />
      <Label v={v} x={368} y={92} color={VIOLET_TEXT}>
        实际参数 = 手动 + 自动增量
      </Label>
      <Box v={v} b={{ x: 242, y: 20, w: 236, h: 52, title: 'GLAZE 编排器', sub: '画像 → 方案 → 校准 → 保护', tone: 'v' }} />

      {/* 主信号：顺序固定 */}
      <Box v={v} b={{ x: 18, y, w: 64, h, title: '输入', tone: 'g' }} />
      {mods.map(m => (
        <Box key={m.title} v={v} b={m} />
      ))}
      <Box v={v} b={{ x: 638, y, w: 64, h, title: '输出', tone: 'g' }} />
      {[82, 214, 346, 478, 610].map(x => (
        <Wire key={x} v={v} pts={[[x, 158], [x + 26, 158]]} />
      ))}

      {/* 入口与出口两块表 */}
      <Wire v={v} tone="mute" dashed pts={[[50, 184], [50, 220]]} />
      <Wire v={v} tone="mute" dashed pts={[[670, 184], [670, 220]]} />
      <Box v={v} b={{ x: 18, y: 222, w: 116, h: 46, title: 'IN 表', sub: '真峰值 · LUFS', tone: 'g' }} />
      <Box v={v} b={{ x: 578, y: 222, w: 124, h: 46, title: 'OUT 表', sub: '真峰值 · LUFS · GR', tone: 'g' }} />
    </>
  );
}

function ChainNarrow({ v }: { v: V }) {
  const rows = [16, 80, 144, 208, 272, 336];
  return (
    <>
      {/* 竖排：GLAZE 在右，一条竖总线从右侧把实际参数交给四个模块 */}
      <Wire v={v} tone="v" dashed head={false} pts={[[189, 102], [189, 294]]} />
      {rows.slice(1, 5).map(y => (
        <Wire key={y} v={v} tone="v" dashed pts={[[189, y + 22], [172, y + 22]]} />
      ))}
      <Wire v={v} tone="v" dashed head={false} pts={[[208, 198], [189, 198]]} />
      <Dot x={189} y={198} tone="v" />
      <Wire v={v} tone="v" dashed pts={[[266, 60], [266, 164]]} />
      <Label v={v} x={274} y={116} color={VIOLET_TEXT}>
        入口特征
      </Label>
      <Wire v={v} tone="mute" dashed pts={[[266, 336], [266, 232]]} />
      <Label v={v} x={274} y={288}>
        模块统计
      </Label>
      <Box v={v} b={{ x: 208, y: 166, w: 116, h: 64, title: 'GLAZE 编排器', sub: ['画像 → 方案', '校准 → 保护'], tone: 'v' }} />

      <Box v={v} b={{ x: 20, y: rows[0], w: 150, h: 44, title: '输入', tone: 'g' }} />
      {CHAIN_MODS.map((m, i) => (
        <Box key={m.title} v={v} b={{ x: 20, y: rows[i + 1], w: 150, h: 44, title: m.title, sub: m.sub }} />
      ))}
      <Box v={v} b={{ x: 20, y: rows[5], w: 150, h: 44, title: '输出', tone: 'g' }} />
      {rows.slice(0, 5).map(y => (
        <Wire key={y} v={v} pts={[[95, y + 44], [95, y + 62]]} />
      ))}

      <Wire v={v} tone="mute" dashed pts={[[170, 38], [206, 38]]} />
      <Wire v={v} tone="mute" dashed pts={[[170, 358], [206, 358]]} />
      <Box v={v} b={{ x: 208, y: 16, w: 116, h: 44, title: 'IN 表', sub: '真峰值 · LUFS', tone: 'g' }} />
      <Box v={v} b={{ x: 208, y: 336, w: 116, h: 44, title: 'OUT 表', sub: '真峰值 · LUFS · GR', tone: 'g' }} />
    </>
  );
}

/* ---------------- 限幅器 ---------------- */

// 粗略示意：|x| 的包络 → 需求增益 r → 前瞻滑动最小 rMin → 移动平均后的最终增益。
// 平均窗口不长于滑动最小的窗口，所以峰值那一刻的增益一定 ≤ r：限幅后的线不会越过 Ceiling。
const LIM = (() => {
  const n = 90;
  const ceil = 0.72;
  const win = 6;
  const a: number[] = [];
  for (let i = 0; i <= n; i++) {
    const t = i / n;
    const beat = Math.exp(-((t * 3) % 1) * 5);
    a.push(0.35 + 0.65 * beat * Math.abs(Math.sin(t * 40)));
  }
  const r = a.map(v => Math.min(1, ceil / Math.max(v, 1e-3)));
  const rMin = r.map((_, i) => Math.min(...r.slice(i, i + win)));
  const env = rMin.map((_, i) => {
    const w = rMin.slice(Math.max(0, i - win + 1), i + 1);
    return w.reduce((s, v) => s + v, 0) / w.length;
  });
  return { n, ceil, a, r, rMin, env };
})();

function LimiterPlot({ v }: { v: V }) {
  const W = v.narrow ? NARROW_W : WIDE_W;
  // 左侧留一条窄栏放「Ceiling」与增益刻度，曲线从不压到字
  const x0 = v.narrow ? 58 : 72;
  const x1 = W - 12;
  const X = (i: number) => x0 + (i / LIM.n) * (x1 - x0);
  const aTop = 34;
  const aBase = v.narrow ? 128 : 138;
  const yA = (a: number) => aBase - (aBase - aTop) * a;
  const gTop = v.narrow ? 180 : 196;
  const gBot = v.narrow ? 262 : 280;
  const yG = (g: number) => gTop + ((1 - g) / 0.3) * (gBot - gTop);
  const path = (vals: readonly number[], y: (val: number) => number) => vals.map((val, i) => `${i ? 'L' : 'M'}${r1(X(i))} ${r1(y(val))}`).join(' ');
  const ceilY = yA(LIM.ceil);
  const gutter = x0 - 6;
  return (
    <>
      <Label v={v} x={x0} y={20} mono>
        |x| 与 8× 插值真峰值
      </Label>
      <line x1={x0} y1={aBase} x2={x1} y2={aBase} stroke={LINE} />
      <line x1={x0} y1={ceilY} x2={x1} y2={ceilY} stroke={WARN} strokeWidth={1.1} strokeDasharray="5 4" />
      <Label v={v} x={gutter} y={ceilY + 4} anchor="end" color={WARN_TEXT} mono>
        Ceiling
      </Label>
      <path d={path(LIM.a, yA)} fill="none" stroke={C200} strokeWidth={1.5} />
      <path d={path(LIM.a.map((a, i) => a * LIM.env[i]), yA)} fill="none" stroke={C600} strokeWidth={1.6} />

      <Label v={v} x={x0} y={gTop - 16} mono>
        增益
      </Label>
      <line x1={x0} y1={gTop} x2={x1} y2={gTop} stroke={LINE} />
      <line x1={x0} y1={gBot} x2={x1} y2={gBot} stroke={LINE} strokeDasharray="2 4" />
      <Label v={v} x={gutter} y={gTop + 4} anchor="end" color={INK4} mono>
        1.0
      </Label>
      <Label v={v} x={gutter} y={gBot + 4} anchor="end" color={INK4} mono>
        0.7
      </Label>
      <path d={path(LIM.r, yG)} fill="none" stroke="#C7D3D6" strokeWidth={1.2} />
      <path d={path(LIM.rMin, yG)} fill="none" stroke={C300} strokeWidth={1.3} strokeDasharray="4 3" />
      <path d={path(LIM.env, yG)} fill="none" stroke={C800} strokeWidth={1.8} />
    </>
  );
}

/* ---------------- 真峰值 ---------------- */

// 采样点之间的连续波形用 sinc 插值还原；真峰值取插值曲线的最大值（算出来的，不是手填）。
const TP = (() => {
  const s = [0.05, 0.22, 0.8, 0.8, 0.16, -0.42, -0.78, -0.62, -0.12, 0.2];
  const sinc = (x: number) => (x === 0 ? 1 : Math.sin(Math.PI * x) / (Math.PI * x));
  const at = (t: number) => s.reduce((acc, val, i) => acc + val * sinc(t - i), 0);
  const curve: [number, number][] = [];
  let peak = -Infinity;
  let peakT = 0;
  for (let k = 0; k <= 9 * 32; k++) {
    const t = k / 32;
    const val = at(t);
    curve.push([t, val]);
    if (val > peak) {
      peak = val;
      peakT = t;
    }
  }
  return { s, curve, peak, peakT, samplePeak: Math.max(...s) };
})();

function TruePeakPlot({ v }: { v: V }) {
  const W = v.narrow ? NARROW_W : WIDE_W;
  const x0 = v.narrow ? 34 : 72;
  const step = v.narrow ? 30 : 64;
  const base = v.narrow ? 112 : 118;
  const k = v.narrow ? 84 : 92;
  const X = (t: number) => x0 + t * step;
  const Y = (val: number) => base - val * k;
  const right = W - 14;
  const spY = Y(TP.samplePeak);
  const tpY = Y(TP.peak);
  return (
    <>
      <line x1={x0 - 16} y1={base} x2={right} y2={base} stroke={LINE} />
      <line x1={x0 - 16} y1={spY} x2={right} y2={spY} stroke={INK4} strokeDasharray="3 4" />
      <Label v={v} x={right} y={spY + 15} anchor="end">
        采样点峰值
      </Label>
      <line x1={x0 - 16} y1={tpY} x2={right} y2={tpY} stroke={WARN} strokeDasharray="5 4" />
      <Label v={v} x={right} y={tpY - 7} anchor="end" color={WARN_TEXT}>
        真峰值
      </Label>
      <polyline points={TP.curve.map(([t, val]) => `${r1(X(t))},${r1(Y(val))}`).join(' ')} fill="none" stroke={C500} strokeWidth={1.6} />
      {TP.s.map((val, i) => (
        <g key={i}>
          <line x1={X(i)} y1={base} x2={X(i)} y2={Y(val)} stroke={C800} strokeWidth={1.2} />
          <circle cx={X(i)} cy={Y(val)} r={3.4} fill={C800} />
        </g>
      ))}
      <circle cx={X(TP.peakT)} cy={tpY} r={3.6} fill={PANEL} stroke={WARN} strokeWidth={1.4} />
    </>
  );
}

/* ---------------- BALANCE ---------------- */

// 参考轮廓 R[b]（这首歌自己抹平后的样子）与快跟踪电平的偏差 D；|D| ≤ 1 dB 不动。
const BAL = (() => {
  const n = 27;
  const R = Array.from({ length: n }, (_, i) => 30 + 15 * Math.sin((Math.PI * (i + 2)) / 30) - 0.16 * i);
  const D = [0.5, -3.4, 2.8, 0.4, -2.0, 5.2, 2.1, -0.6, -4.8, -2.6, 0.7, 3.8, 5.8, 1.4, -0.8, -4.0, -1.5, 2.4, 0.3, 4.4, -3.1, -0.4, 1.8, -5.4, 0.9, 3.2, -1.8];
  return { n, R, D };
})();

function BalancePlot({ v }: { v: V }) {
  const W = v.narrow ? NARROW_W : WIDE_W;
  const x0 = v.narrow ? 16 : 48;
  const x1 = W - (v.narrow ? 16 : 40);
  const base = v.narrow ? 176 : 214;
  const k = v.narrow ? 3.3 : 4.4; // 每 dB 多少单位
  const Y = (db: number) => base - (db - 12) * k;
  const bw = (x1 - x0) / BAL.n;
  const gap = v.narrow ? 3 : 5;
  const cx = (i: number) => x0 + i * bw + bw / 2;
  const line = (f: (i: number) => number) => BAL.R.map((_, i) => `${i ? 'L' : 'M'}${r1(cx(i))} ${r1(Y(f(i)))}`).join(' ');
  const dead = `${line(i => BAL.R[i] + 1)} ${BAL.R.map((_, i) => BAL.n - 1 - i)
    .map(i => `L${r1(cx(i))} ${r1(Y(BAL.R[i] - 1))}`)
    .join(' ')} Z`;
  return (
    <>
      <Label v={v} x={x0} y={v.narrow ? 18 : 20} mono>
        {v.narrow ? 'Lfast[b] 与 R[b]' : 'Lfast[b] 与参考轮廓 R[b]'}
      </Label>
      {BAL.R.map((R, i) => {
        const L = R + BAL.D[i];
        const x = x0 + i * bw + gap / 2;
        const w = bw - gap;
        const top = Math.min(L, R + 1);
        return (
          <g key={i}>
            <rect x={r1(x)} y={r1(Y(top))} width={r1(w)} height={r1(base - Y(top))} rx="2" fill={C200} />
            {L > R + 1 ? <rect x={r1(x)} y={r1(Y(L))} width={r1(w)} height={r1(Y(R + 1) - Y(L))} rx="2" fill={WARN} opacity="0.88" /> : null}
            {L < R - 1 ? (
              <rect x={r1(x + 0.5)} y={r1(Y(R - 1))} width={r1(w - 1)} height={r1(Y(L) - Y(R - 1))} rx="2" fill={C100} stroke={C600} strokeWidth="1" strokeDasharray="3 2" />
            ) : null}
          </g>
        );
      })}
      <path d={dead} fill={C500} opacity="0.16" />
      <path d={line(i => BAL.R[i])} fill="none" stroke={C800} strokeWidth={1.3} strokeDasharray="4 4" />
      <line x1={x0} y1={base} x2={x1} y2={base} stroke={LINE} />
      <Label v={v} x={x0} y={base + 20} mono color={INK4}>
        40 Hz
      </Label>
      <Label v={v} x={cx(14)} y={base + 20} anchor="middle" mono color={INK4}>
        1 kHz
      </Label>
      <Label v={v} x={x1} y={base + 20} anchor="end" mono color={INK4}>
        16 kHz
      </Label>
    </>
  );
}

/* ---------------- Glue ---------------- */

function GlueWide({ v }: { v: V }) {
  const bands: BoxSpec[] = [
    { x: 140, y: 134, w: 152, h: 48, title: '低 ≤ 180 Hz', sub: '攻击 36 ms' },
    { x: 316, y: 134, w: 152, h: 48, title: '中', sub: '跟联动攻击' },
    { x: 492, y: 134, w: 152, h: 48, title: '高 ≥ 5 kHz', sub: '攻击 8 ms' }
  ];
  return (
    <>
      <Wire v={v} pts={[[104, 44], [150, 44]]} />
      <Wire v={v} pts={[[348, 44], [394, 44]]} />
      {/* 音频进三段；联动包络 g 从上方驱动三段，各段自己的攻击与偏移 */}
      <Wire v={v} pts={[[64, 68], [64, 158], [118, 158]]} />
      <Wire v={v} dashed pts={[[498, 68], [498, 86], [392, 86], [392, 106]]} />
      <Wire v={v} pts={[[392, 200], [392, 220]]} />
      <Box v={v} b={{ x: 24, y: 20, w: 80, h: 48, title: '输入', tone: 'g' }} />
      <Box v={v} b={{ x: 152, y: 20, w: 196, h: 48, title: '侧链 100 Hz 高通', sub: 'L / R 取大 · 2:1 · 6 dB 拐点' }} />
      <Box v={v} b={{ x: 396, y: 20, w: 204, h: 48, title: '联动包络 g', sub: '攻击 20 ms · 自动释放' }} />
      <rect x={120} y={108} width={544} height={92} rx={14} fill="none" stroke={C300} strokeDasharray="4 4" />
      <rect x={132} y={100} width={96} height={16} fill={PANEL} />
      <Label v={v} x={140} y={112} color={C700} weight={600}>
        三段分开施压
      </Label>
      {bands.map(b => (
        <Box key={b.title} v={v} b={b} />
      ))}
      <Box v={v} b={{ x: 292, y: 222, w: 200, h: 48, title: '求和 → 输出', sub: '三路差为 0 时退回单增益', tone: 'g' }} />
    </>
  );
}

function GlueNarrow({ v }: { v: V }) {
  const bands: BoxSpec[] = [
    { x: 28, y: 176, w: 284, h: 44, title: '低 ≤ 180 Hz', sub: '攻击 36 ms' },
    { x: 28, y: 232, w: 284, h: 44, title: '中', sub: '跟联动攻击' },
    { x: 28, y: 288, w: 284, h: 44, title: '高 ≥ 5 kHz', sub: '攻击 8 ms' }
  ];
  return (
    <>
      <Wire v={v} pts={[[116, 38], [138, 38]]} />
      <Wire v={v} pts={[[230, 60], [230, 82]]} />
      <Wire v={v} pts={[[68, 60], [68, 154]]} />
      <Wire v={v} dashed pts={[[230, 128], [230, 154]]} />
      <Wire v={v} pts={[[170, 352], [170, 370]]} />
      <Box v={v} b={{ x: 20, y: 16, w: 96, h: 44, title: '输入', tone: 'g' }} />
      <Box v={v} b={{ x: 140, y: 16, w: 180, h: 44, title: '侧链 100 Hz 高通', sub: 'L / R 取大 · 2:1 · 6 dB 拐点' }} />
      <Box v={v} b={{ x: 140, y: 84, w: 180, h: 44, title: '联动包络 g', sub: '攻击 20 ms · 自动释放' }} />
      <rect x={12} y={156} width={316} height={196} rx={12} fill="none" stroke={C300} strokeDasharray="4 4" />
      <rect x={86} y={148} width={86} height={16} fill={PANEL} />
      <Label v={v} x={92} y={160} color={C700} weight={600}>
        三段分开施压
      </Label>
      {bands.map(b => (
        <Box key={b.title} v={v} b={b} />
      ))}
      <Box v={v} b={{ x: 70, y: 372, w: 200, h: 44, title: '求和 → 输出', sub: '三路差为 0 时退回单增益', tone: 'g' }} />
    </>
  );
}

/* ---------------- Heat ---------------- */

function HeatWide({ v }: { v: V }) {
  return (
    <>
      <Wire v={v} pts={[[104, 106], [128, 106], [128, 44], [150, 44]]} />
      <Wire v={v} pts={[[128, 106], [128, 168], [150, 168]]} />
      <Dot x={128} y={106} />
      <Wire v={v} pts={[[384, 44], [526, 44]]} />
      <Wire v={v} pts={[[300, 168], [322, 168]]} />
      <Wire v={v} pts={[[504, 168], [526, 168]]} />
      <Wire v={v} pts={[[612, 144], [612, 70]]} />
      <Box v={v} b={{ x: 24, y: 82, w: 80, h: 48, title: '输入', tone: 'g' }} />
      <Box v={v} b={{ x: 152, y: 20, w: 232, h: 48, title: '干路 · 64 样本延迟', sub: '原声完整保留', tone: 'g' }} />
      <Box v={v} b={{ x: 152, y: 144, w: 148, h: 48, title: '150 Hz 高通', sub: '低音不进非线性' }} />
      <Box v={v} b={{ x: 324, y: 144, w: 180, h: 48, title: 'M / S 电子管曲线', sub: '8× 过采样 · S 更轻、奇对称' }} />
      <Box v={v} b={{ x: 528, y: 144, w: 168, h: 48, title: '残差', sub: '湿 − 干' }} />
      <Box v={v} b={{ x: 528, y: 20, w: 168, h: 48, title: '求和 → Mix → Output', tone: 'g' }} />
    </>
  );
}

function HeatNarrow({ v }: { v: V }) {
  return (
    <>
      <Wire v={v} head={false} pts={[[170, 52], [170, 68]]} />
      <Wire v={v} pts={[[170, 68], [86, 68], [86, 82]]} />
      <Wire v={v} pts={[[170, 68], [254, 68], [254, 82]]} />
      <Dot x={170} y={68} />
      <Wire v={v} pts={[[254, 140], [254, 162]]} />
      <Wire v={v} pts={[[254, 220], [254, 242]]} />
      <Wire v={v} pts={[[254, 288], [254, 310]]} />
      <Wire v={v} pts={[[86, 140], [86, 310]]} />
      <Box v={v} b={{ x: 110, y: 12, w: 120, h: 40, title: '输入', tone: 'g' }} />
      <Box v={v} b={{ x: 20, y: 84, w: 132, h: 56, title: '干路', sub: ['64 样本延迟', '原声完整保留'], tone: 'g' }} />
      <Box v={v} b={{ x: 188, y: 84, w: 132, h: 56, title: '150 Hz 高通', sub: '低音不进非线性' }} />
      <Box v={v} b={{ x: 188, y: 164, w: 132, h: 56, title: 'M / S 电子管', sub: ['8× 过采样', 'S 更轻、奇对称'] }} />
      <Box v={v} b={{ x: 188, y: 244, w: 132, h: 44, title: '残差', sub: '湿 − 干' }} />
      <Box v={v} b={{ x: 20, y: 312, w: 300, h: 44, title: '求和 → Mix → Output', tone: 'g' }} />
    </>
  );
}

/* ---------------- CLAY ---------------- */

function ClayWide({ v }: { v: V }) {
  return (
    <>
      <Wire v={v} pts={[[80, 136], [98, 136]]} />
      <Wire v={v} head={false} pts={[[300, 136], [318, 136]]} />
      <Wire v={v} pts={[[318, 136], [318, 56], [334, 56]]} />
      <Wire v={v} pts={[[318, 136], [318, 216], [334, 216]]} />
      <Dot x={318} y={136} />
      <Wire v={v} pts={[[448, 56], [590, 56]]} />
      <Wire v={v} pts={[[448, 216], [466, 216]]} />
      <Wire v={v} pts={[[572, 216], [590, 216]]} />
      <Wire v={v} pts={[[646, 192], [646, 82]]} />
      <Wire v={v} tone="c" dashed pts={[[200, 196], [200, 162]]} />
      <Box v={v} b={{ x: 16, y: 112, w: 64, h: 48, title: 'L / R', tone: 'g' }} />
      <Box v={v} b={{ x: 100, y: 112, w: 200, h: 48, title: '① 低切', sub: '4 阶 Butterworth · 10～60 Hz' }} />
      <Box v={v} b={{ x: 336, y: 32, w: 112, h: 48, title: 'M', sub: '(L + R) / 2 · 不动', tone: 'g' }} />
      <Box v={v} b={{ x: 336, y: 192, w: 112, h: 48, title: 'S', sub: '(L − R) / 2', tone: 'g' }} />
      <Box v={v} b={{ x: 468, y: 192, w: 104, h: 48, title: '② 收单', sub: '线性相位' }} />
      <Box v={v} b={{ x: 592, y: 192, w: 108, h: 48, title: '③ 加宽', sub: '2 kHz 高架' }} />
      <Box v={v} b={{ x: 592, y: 32, w: 108, h: 48, title: 'L′ / R′', sub: '= M ± S', tone: 'g' }} />
      <Box v={v} b={{ x: 100, y: 196, w: 200, h: 48, title: '峰值守护', sub: '前瞻 8 ms · 会抬峰值就让开', tone: 'o' }} />
    </>
  );
}

function ClayNarrow({ v }: { v: V }) {
  return (
    <>
      <Wire v={v} pts={[[70, 56], [70, 86]]} />
      <Wire v={v} tone="c" dashed pts={[[248, 64], [248, 86]]} />
      <Wire v={v} head={false} pts={[[170, 136], [170, 152]]} />
      <Wire v={v} pts={[[170, 152], [86, 152], [86, 166]]} />
      <Wire v={v} pts={[[170, 152], [254, 152], [254, 166]]} />
      <Dot x={170} y={152} />
      <Wire v={v} pts={[[254, 212], [254, 234]]} />
      <Wire v={v} pts={[[254, 280], [254, 302]]} />
      <Wire v={v} pts={[[254, 348], [254, 370]]} />
      <Wire v={v} pts={[[86, 212], [86, 370]]} />
      <Box v={v} b={{ x: 20, y: 12, w: 100, h: 44, title: 'L / R', tone: 'g' }} />
      <Box v={v} b={{ x: 176, y: 8, w: 144, h: 56, title: '峰值守护', sub: ['前瞻 8 ms', '会抬峰值就让开'], tone: 'o' }} />
      <Box v={v} b={{ x: 20, y: 88, w: 300, h: 48, title: '① 低切', sub: '4 阶 Butterworth · 10～60 Hz' }} />
      <Box v={v} b={{ x: 20, y: 168, w: 132, h: 44, title: 'M', sub: '(L + R) / 2 · 不动', tone: 'g' }} />
      <Box v={v} b={{ x: 188, y: 168, w: 132, h: 44, title: 'S', sub: '(L − R) / 2', tone: 'g' }} />
      <Box v={v} b={{ x: 188, y: 236, w: 132, h: 44, title: '② 收单', sub: '线性相位' }} />
      <Box v={v} b={{ x: 188, y: 304, w: 132, h: 44, title: '③ 加宽', sub: '2 kHz 高架' }} />
      <Box v={v} b={{ x: 20, y: 372, w: 300, h: 44, title: 'L′ / R′', sub: '= M ± S', tone: 'g' }} />
    </>
  );
}

/* ---------------- GLAZE 状态机 ---------------- */

function GlazeWide({ v }: { v: V }) {
  return (
    <>
      <Wire v={v} pts={[[144, 50], [178, 50]]} />
      <Wire v={v} pts={[[324, 50], [358, 50]]} />
      <Wire v={v} pts={[[516, 50], [550, 50]]} />
      <Wire v={v} dashed pts={[[622, 76], [622, 170]]} />
      <Wire v={v} dashed pts={[[660, 172], [660, 78]]} />
      {/* 重新聆听：建候选版本，旧方案照常生效，听完再平滑换过去 */}
      <Wire v={v} tone="v" dashed pts={[[566, 76], [566, 132], [252, 132], [252, 78]]} />
      <Label v={v} x={409} y={152} anchor="middle" color={VIOLET_TEXT} weight={600}>
        重新聆听 → 候选版本
      </Label>
      <Label v={v} x={409} y={170} anchor="middle">
        旧方案照常生效，听完约 3 s 平滑换过去
      </Label>
      <Box v={v} b={{ x: 16, y: 24, w: 128, h: 52, title: '未学习', sub: '播放后自动开始', tone: 'g' }} />
      <Box v={v} b={{ x: 180, y: 24, w: 144, h: 52, title: '聆听中', sub: '15～30 s · 攒证据', tone: 'v' }} />
      <Box v={v} b={{ x: 360, y: 24, w: 156, h: 52, title: '校准中', sub: '对齐输入输出响度差', tone: 'v' }} />
      <Box v={v} b={{ x: 552, y: 24, w: 152, h: 52, title: '已锁定', sub: '基准附近有界保护' }} />
      <Box v={v} b={{ x: 580, y: 172, w: 124, h: 52, title: '受限 · 慢速退让', sub: 'g_保护 ∈ [−3, 0] dB', tone: 'g' }} />
      <Box
        v={v}
        b={{
          x: 16,
          y: 248,
          w: 688,
          h: 52,
          title: '随工程保存：画像 + 方案 + 校准 + 标定快照 + 执行快照',
          sub: '重开不重学；同一起点重渲染逐位可复现；参考条件一变，校准即失效',
          tone: 'g'
        }}
      />
    </>
  );
}

function GlazeNarrow({ v }: { v: V }) {
  return (
    <>
      <Wire v={v} pts={[[126, 60], [126, 82]]} />
      <Wire v={v} pts={[[126, 132], [126, 154]]} />
      <Wire v={v} pts={[[126, 204], [126, 226]]} />
      <Wire v={v} dashed pts={[[110, 276], [110, 322]]} />
      <Wire v={v} dashed pts={[[142, 324], [142, 278]]} />
      <Wire v={v} tone="v" dashed pts={[[232, 252], [264, 252], [264, 108], [234, 108]]} />
      <Label v={v} x={272} y={174} color={VIOLET_TEXT} weight={600}>
        重新聆听
      </Label>
      <Label v={v} x={272} y={191} color={VIOLET_TEXT}>
        候选版本
      </Label>
      <Box v={v} b={{ x: 20, y: 12, w: 212, h: 48, title: '未学习', sub: '播放后自动开始', tone: 'g' }} />
      <Box v={v} b={{ x: 20, y: 84, w: 212, h: 48, title: '聆听中', sub: '15～30 s · 攒证据', tone: 'v' }} />
      <Box v={v} b={{ x: 20, y: 156, w: 212, h: 48, title: '校准中', sub: '对齐输入输出响度差', tone: 'v' }} />
      <Box v={v} b={{ x: 20, y: 228, w: 212, h: 48, title: '已锁定', sub: '基准附近有界保护' }} />
      <Box v={v} b={{ x: 20, y: 324, w: 212, h: 52, title: '受限 · 慢速退让', sub: 'g_保护 ∈ [−3, 0] dB', tone: 'g' }} />
      <Box v={v} b={{ x: 20, y: 400, w: 300, h: 84, title: '随工程保存', sub: ['画像 + 方案 + 校准 + 快照', '重开不重学，逐位可复现', '参考条件一变，校准即失效'], tone: 'g' }} />
    </>
  );
}

/* ---------------- 延迟构成 ---------------- */

// 48 kHz、Lookahead 2 ms：每个模块一行，横条从上一行结束处接着画（瀑布式），小段也有地方写字。
interface LatRow {
  name: string;
  total: number;
  parts: readonly number[];
  fills: readonly string[];
  note: string;
}
const LAT: readonly LatRow[] = [
  { name: 'BALANCE', total: 0, parts: [], fills: [], note: '' },
  { name: 'CLAY', total: 1102, parts: [574, 528], fills: [C300, C400], note: '收单 574 + 守护 528' },
  { name: 'COLOR', total: 64, parts: [64], fills: [C500], note: '' },
  { name: 'LIMIT', total: 218, parts: [60, 96, 62], fills: [C600, C700, C600], note: '过采样 60 + 前瞻 96 + 62' }
];
const LAT_SUM = LAT.reduce((s, row) => s + row.total, 0);

function LatencyPlot({ v }: { v: V }) {
  const x0 = v.narrow ? 84 : 136;
  const span = v.narrow ? 240 : 340;
  const k = span / LAT_SUM;
  const pitch = v.narrow ? 48 : 36;
  const top = 30;
  const barH = v.narrow ? 12 : 14;
  const starts = LAT.map((_, i) => x0 + LAT.slice(0, i).reduce((s, row) => s + row.total, 0) * k);
  const sumY = top + LAT.length * pitch - (v.narrow ? 8 : 10);
  return (
    <>
      {LAT.map((row, i) => {
        const cy = top + i * pitch;
        const start = starts[i];
        const end = start + row.total * k;
        return (
          <g key={row.name}>
            <Label v={v} x={v.narrow ? 12 : 16} y={v.narrow ? cy - 2 : cy + 5} color={INK} weight={600}>
              {row.name}
            </Label>
            {v.narrow ? (
              <Label v={v} x={12} y={cy + 13} mono color={INK3}>
                {row.total}
              </Label>
            ) : null}
            {row.total === 0 ? (
              <rect x={r1(start)} y={cy - barH / 2} width={1.6} height={barH} fill={C300} />
            ) : (
              row.parts.map((p, j) => {
                const sx = start + row.parts.slice(0, j).reduce((s, q) => s + q, 0) * k;
                const w = p * k - (j < row.parts.length - 1 ? 1 : 0);
                return <rect key={j} x={r1(sx)} y={cy - barH / 2} width={r1(Math.max(w, 1))} height={barH} rx="2" fill={row.fills[j]} />;
              })
            )}
            {v.narrow ? (
              row.note ? (
                <Label v={v} x={row.name === 'LIMIT' ? x0 + span : start} y={cy + 22} anchor={row.name === 'LIMIT' ? 'end' : 'start'} color={INK3}>
                  {row.note}
                </Label>
              ) : null
            ) : (
              <Label v={v} x={end + 10} y={cy + 4.5} mono color={INK3}>
                {row.note ? `${row.total} = ${row.note}` : row.total}
              </Label>
            )}
          </g>
        );
      })}
      <path d={`M${x0} ${sumY - 4} V${sumY} H${x0 + span} V${sumY - 4}`} fill="none" stroke={INK4} strokeWidth={1} />
      <Label v={v} x={x0 + span / 2} y={sumY + 22} anchor="middle" color={INK} weight={600}>
        合计 {LAT_SUM} 样本 ≈ 28.8 ms
      </Label>
    </>
  );
}

export const figures: Record<string, () => JSX.Element> = {
  chain: () => <Figure label="信号链示意图" wideH={284} narrowH={396} draw={v => (v.narrow ? <ChainNarrow v={v} /> : <ChainWide v={v} />)} />,

  limiter: () => (
    <Figure
      label="限幅器示意图"
      wideH={296}
      narrowH={280}
      draw={v => <LimiterPlot v={v} />}
      legend={[
        { swatch: 'line', color: C200, label: '输入 |x|（8× 插值峰值）' },
        { swatch: 'line', color: C600, label: '限幅后' },
        { swatch: 'line', color: '#C7D3D6', label: 'r[n] 需求增益' },
        { swatch: 'dash', color: C300, label: 'rMin 前瞻滑动最小' },
        { swatch: 'thick', color: C800, label: '最终增益（1× 域施加，不限幅时恰为 1.0）' }
      ]}
    />
  ),

  truepeak: () => (
    <Figure
      label="真峰值示意图"
      wideH={206}
      narrowH={192}
      draw={v => <TruePeakPlot v={v} />}
      legend={[
        { swatch: 'dot', color: C800, label: '采样点' },
        { swatch: 'line', color: C500, label: '采样点之间的真实波形' }
      ]}
    />
  ),

  balance: () => (
    <Figure
      label="Balance 分析示意图"
      wideH={246}
      narrowH={206}
      draw={v => <BalancePlot v={v} />}
      legend={[
        { swatch: 'bar', color: C200, label: 'Lfast[b] 快跟踪电平' },
        { swatch: 'dash', color: C800, label: 'R[b] 参考轮廓' },
        { swatch: 'band', color: C500, label: '±1 dB 死区：不动' },
        { swatch: 'bar', color: WARN, label: 'D > +1 dB：Tame 压下' },
        { swatch: 'frame', color: C600, label: 'D < −1 dB：Recover 抬起' }
      ]}
    />
  ),

  glue: () => <Figure label="Glue 分频施压示意图" wideH={284} narrowH={430} draw={v => (v.narrow ? <GlueNarrow v={v} /> : <GlueWide v={v} />)} />,

  heat: () => <Figure label="Heat 残差染色示意图" wideH={208} narrowH={370} draw={v => (v.narrow ? <HeatNarrow v={v} /> : <HeatWide v={v} />)} />,

  clay: () => <Figure label="Clay 修坯示意图" wideH={260} narrowH={430} draw={v => (v.narrow ? <ClayNarrow v={v} /> : <ClayWide v={v} />)} />,

  glaze: () => <Figure label="GLAZE 状态机示意图" wideH={316} narrowH={498} draw={v => (v.narrow ? <GlazeNarrow v={v} /> : <GlazeWide v={v} />)} />,

  latency: () => <Figure label="延迟构成示意图" wideH={192} narrowH={250} draw={v => <LatencyPlot v={v} />} />
};
