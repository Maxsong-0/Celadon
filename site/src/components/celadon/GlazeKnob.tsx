import { useCallback, useEffect, useId, useLayoutEffect, useRef, type KeyboardEvent } from 'react';
import Crackle, { visibleCrackCount } from './Crackle';
import { angleFor, cometSegments, deadZoneRatio, startAngle as START_RAD, sweep as SWEEP_RAD } from './dialMotion';
import { useDialControl, type DialGeometry } from './useDialControl';
import { clamp, cn } from '@/lib/utils';

export interface GlazeKnobProps {
  /** 0 ～ 100 */
  value: number;
  onChange?: (v: number) => void;
  /** 示意用的自动加量（0 ～ 100），画成紫色薄弧 */
  autoIncrement?: number;
  size?: number;
  interactive?: boolean;
  className?: string;
  label?: string;
}

const START = -135; // 弧从左下开始
const SWEEP = 270;

/** 插件 cyanAt 的复刻：0 = c200 → 0.35 c400 → 0.7 c600 → 1 = c800 */
export function cyanAt(x: number): string {
  const stops: [number, [number, number, number]][] = [
    [0, [0xa3, 0xe0, 0xe8]],
    [0.35, [0x33, 0xb3, 0xc4]],
    [0.7, [0x12, 0x7f, 0x90]],
    [1, [0x0a, 0x4a, 0x57]]
  ];
  const t = clamp(x, 0, 1);
  for (let i = 1; i < stops.length; i++) {
    const [t0, c0] = stops[i - 1];
    const [t1, c1] = stops[i];
    if (t <= t1) {
      const k = (t - t0) / (t1 - t0);
      const c = c0.map((v, j) => Math.round(v + (c1[j] - v) * k));
      return `rgb(${c[0]}, ${c[1]}, ${c[2]})`;
    }
  }
  return 'rgb(10, 74, 87)';
}

/** 两个 #RRGGBB 按 t 线性混合（插件 Porcelain.cpp 的 mixHex）。 */
function mixHex(a: string, b: string, t: number) {
  const pa = [1, 3, 5].map(i => parseInt(a.slice(i, i + 2), 16));
  const pb = [1, 3, 5].map(i => parseInt(b.slice(i, i + 2), 16));
  return `rgb(${pa.map((v, i) => Math.round(v + (pb[i] - v) * t)).join(', ')})`;
}

function arcPath(cx: number, cy: number, r: number, a0: number, a1: number) {
  const rad = (d: number) => ((d - 90) * Math.PI) / 180;
  const x0 = cx + r * Math.cos(rad(a0));
  const y0 = cy + r * Math.sin(rad(a0));
  const x1 = cx + r * Math.cos(rad(a1));
  const y1 = cy + r * Math.sin(rad(a1));
  const large = a1 - a0 > 180 ? 1 : 0;
  return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}

const CX = 200;
const CY = 200;
/** 读数的字号与等宽数字格：Fraunces 没有 tnum，每个数字各占一格（0.56 em，与 .tnum-cell 同）、在格里居中；「%」接在最后一格后面。 */
const READOUT_PX = 44;
const READOUT_CELL = READOUT_PX * 0.56;
const PERCENT_GAP = 4;
const PERCENT_W = 20 * 0.63; // 20 px 的「%」字宽，只用来让整串居中
const R_TRACK = 176;
const R_DISC = 150;
const R_DIMPLE = 44;
const TRACK_WIDTH = 5;
/** 刻度点所在的圈（插件主旋钮没有刻度点）。 */
const R_TICKS = R_TRACK + 16;
/** 外圈的外沿：插件是轨道外 8 px（GlazeKnob.cpp ringHitMargin）；网站轨道外还有一圈刻度点，点上去也该算外圈，外沿放到刻度点外 8（R 200，正好到画布左右边）。 */
const R_GRAB = R_TICKS + 8;
/** 彗尾头部满强度时多出的线宽：插件主旋钮 10 px 配轨道半径 102（GlazeKnob.cpp cometExtraWidth），按半径比例换到这里的 176。 */
const COMET_EXTRA_WIDTH = (10 * R_TRACK) / 102;
/** 读数在动时，瓷面（开片、釉色、积釉）比例差这么多以内且开片条数没变就不跟（插件 GlazeKnob.cpp porcelainTolerance）。 */
const PORCELAIN_TOLERANCE = 0.004;
const DEFAULT_VALUE = 35;
/**
 * 外圈 / 中间的分区与拖法（↔ 插件 GlazeKnob::getDialGeometry）：中间半径 = 0.55 × 轨道半径（dialMotion.deadZoneRatio）；
 * 中间上下拖满量程 250 px（插件 GlazeKnob.cpp fullDragPixels）。
 */
const GEOMETRY: DialGeometry = {
  cx: CX,
  cy: CY,
  trackRadius: R_TRACK,
  deadRadius: deadZoneRatio * R_TRACK,
  grabRadius: R_GRAB,
  fullDragPixels: 250
};
const toDegrees = (radians: number) => (radians * 180) / Math.PI;

/**
 * 青瓷主旋钮（设计规格-官网 第 5 节）：豆青釉面（与插件同色） + 随 GLAZE 生长的开片 + 边缘积釉 + 底部透光。
 * 中心是同釉的浅凹窝和一圈刻线，指针是釉面边缘刻出的短槽——不再有深色旋钮帽（那样像眼球）。
 * 交互照插件（T-100 / ADR-037，网站版 T-101，行为在 useDialControl.ts）：外圈点一下跳过去、沿外圈转、快甩松手多滑一段，
 * 转动时指示点身后拖一条青色彗尾；中间上下拖；⌘ / Ctrl 精调；滚轮微调，双击回 35，键盘 ↑↓ / PageUp / Home / End。
 * 画的都是「读数」（带弹簧的位置），停稳时就是参数值本身，静止画面与没有动画时一样。
 */
export default function GlazeKnob({ value, onChange, autoIncrement = 0, size = 320, interactive = true, className, label = 'GLAZE' }: GlazeKnobProps) {
  const id = useId().replace(/:/g, '');
  const ref = useRef<SVGSVGElement>(null);
  const v01 = clamp(value / 100, 0, 1);

  // 设参数：按 0.1 吸附后交给父组件，返回实际设下的比例（插件里是参数自己的步进吸附）。
  const commit = useCallback(
    (p: number) => {
      const v = clamp(Math.round(p * 100 * 10) / 10, 0, 100);
      onChange?.(v);
      return v / 100;
    },
    [onChange]
  );
  const knob = useDialControl({ value: v01, commit, geometry: GEOMETRY, interactive, svgRef: ref });
  const { frame, reduceMotion, jumpTo, param } = knob;

  // 数值弧、刻度、指针、紫弧、读数文字都画读数（插件 paintFace：读数钳到 0 ～ 1）。
  const shown = clamp(frame.reading, 0, 1);
  const angle = START + SWEEP * shown;
  const autoEnd = clamp(angle + (SWEEP * autoIncrement) / 100, START, START + SWEEP);
  // 读数一格一个数字：数值变化时整串不左右晃（首屏旋钮一直在慢慢「呼吸」，拖动时也一样）。停稳时直接用参数值（与改前逐字相同）。
  const digits = [...String(Math.round(frame.settled ? value : shown * 100))];
  const readoutX0 = CX - (digits.length * READOUT_CELL + PERCENT_GAP + PERCENT_W) / 2;

  // 彗尾（插件 DialControl::drawComet）：12 段沿轨道画在读数身后，颜色与插件数值弧同一函数（valueArcColour = cyanAt(0.12 + 0.88 x)）。
  const comet = !reduceMotion && frame.strength > 0 ? cometSegments(angleFor(frame.reading), frame.strength, frame.direction) : [];

  // 瓷面跟读数（插件 GlazeKnob::needsRender）：动画在跑时差 PORCELAIN_TOLERANCE 以内、开片条数也没变就沿用上一次；
  // 动画停了就精确落在读数上。插件「减少动效」时也按容差，那是为了省 CPU 栅格化；网站瓷面只是 SVG 属性，停着时一律精确，
  // 静止画面只由数值决定。插件的 60 ms 限频（minRenderGapMs）同理不搬。
  const porcelainRef = useRef(shown);
  const previousPorcelain = porcelainRef.current;
  const porcelain =
    frame.running && visibleCrackCount(shown) === visibleCrackCount(previousPorcelain) && Math.abs(shown - previousPorcelain) <= PORCELAIN_TOLERANCE
      ? previousPorcelain
      : shown;
  useLayoutEffect(() => {
    porcelainRef.current = porcelain;
  });

  const onKey = (e: KeyboardEvent<SVGSVGElement>) => {
    if (!interactive) return;
    // 网站独有的键盘（无障碍）：同滚轮，直接落到新值、不出彗尾（原版 CometDial 的键盘微调就是这样）。
    const current = param() * 100;
    const step = e.shiftKey ? 10 : 1;
    let next: number;
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') next = current + step;
    else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') next = current - step;
    else if (e.key === 'PageUp') next = current + 10;
    else if (e.key === 'PageDown') next = current - 10;
    else if (e.key === 'Home') next = 0;
    else if (e.key === 'End') next = 100;
    else return;
    jumpTo(next / 100);
    e.preventDefault();
  };

  useEffect(() => {
    // 滚轮要 passive: false 才能 preventDefault；React 的 onWheel 是 passive 的，这里手动挂（jumpTo / param 不随渲染变，只挂一次）。
    const el = ref.current;
    if (!el || !interactive) return;
    const handler = (e: globalThis.WheelEvent) => {
      e.preventDefault();
      jumpTo((param() * 100 - Math.sign(e.deltaY) * (e.metaKey || e.ctrlKey ? 0.5 : 2)) / 100);
    };
    el.addEventListener('wheel', handler, { passive: false });
    return () => el.removeEventListener('wheel', handler);
  }, [interactive, jumpTo, param]);

  // 釉越厚（GLAZE 越大）边缘积釉越深、开片越密越清楚
  const pool = 0.45 + porcelain * 0.55;
  // 釉色随 GLAZE 从浅灰青走到较深的豆青：与插件 renderGlaze 同一组端点色、同一条 t^0.75 曲线
  const depth = Math.pow(porcelain, 0.75);
  const g0 = mixHex('#EFF3ED', '#BFD4C8', depth);
  const g1 = mixHex('#DCE8E1', '#95B5A9', depth);
  const g2 = mixHex('#BCD1C7', '#71978D', depth);

  return (
    <svg
      ref={ref}
      viewBox="0 0 400 440"
      width={size}
      height={size * 1.1}
      className={cn('select-none touch-none', interactive && 'cursor-ns-resize', className)}
      role={interactive ? 'slider' : 'img'}
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(value)}
      aria-valuetext={`${Math.round(value)} %`}
      tabIndex={interactive ? 0 : -1}
      {...knob.handlers}
      onDoubleClick={() => interactive && knob.resetTo(DEFAULT_VALUE / 100)}
      onKeyDown={onKey}
    >
      <defs>
        {/* 豆青釉身：左上受光；三段颜色随 GLAZE 变深（见上方 g0 ～ g2） */}
        <radialGradient id={`${id}-glaze`} cx="36%" cy="30%" r="82%">
          <stop offset="0" stopColor="#F4F7F3" />
          <stop offset="0.3" stopColor={g0} />
          <stop offset="0.66" stopColor={g1} />
          <stop offset="1" stopColor={g2} />
        </radialGradient>
        {/* 边缘积釉：釉往外沿流，越靠边越深 */}
        <radialGradient id={`${id}-pool`} cx="50%" cy="50%" r="50%">
          <stop offset="0.68" stopColor="#5C8579" stopOpacity="0" />
          <stop offset="0.93" stopColor="#5C8579" stopOpacity="0.32" />
          <stop offset="1" stopColor="#3D6258" stopOpacity="0.72" />
        </radialGradient>
        {/* 底部透光：光穿过薄釉，下缘反而更亮 */}
        <linearGradient id={`${id}-rim`} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="#FFFFFF" stopOpacity="0" />
          <stop offset="0.62" stopColor="#FFFFFF" stopOpacity="0" />
          <stop offset="1" stopColor="#EEF5F1" stopOpacity="0.85" />
        </linearGradient>
        {/* 中心浅凹窝：边上略深、中间受光 */}
        <radialGradient id={`${id}-dimple`} cx="56%" cy="60%" r="60%">
          <stop offset="0" stopColor="#EEF4EF" stopOpacity="0.95" />
          <stop offset="0.7" stopColor="#CDDDD4" stopOpacity="0.6" />
          <stop offset="1" stopColor="#8EAB9F" stopOpacity="0.55" />
        </radialGradient>
        <linearGradient id={`${id}-arc`} x1="0" y1="1" x2="1" y2="0">
          <stop offset="0" stopColor="#A3E0E8" />
          <stop offset="0.5" stopColor="#1799AB" />
          <stop offset="1" stopColor="#0A4A57" />
        </linearGradient>
        <filter id={`${id}-soft`} x="-40%" y="-40%" width="180%" height="180%">
          <feGaussianBlur stdDeviation="7" />
        </filter>
        <filter id={`${id}-hair`} x="-10%" y="-10%" width="120%" height="120%">
          <feGaussianBlur stdDeviation="1.2" />
        </filter>
        {/* 投影带釉色，不用纯黑 */}
        <filter id={`${id}-shadow`} x="-40%" y="-40%" width="180%" height="200%">
          <feDropShadow dx="0" dy="20" stdDeviation="17" floodColor="#2E4A42" floodOpacity="0.26" />
        </filter>
        <clipPath id={`${id}-clip`}>
          <circle cx={CX} cy={CY} r={R_DISC} />
        </clipPath>
      </defs>

      {/* 270° 轨道 */}
      <path d={arcPath(CX, CY, R_TRACK, START, START + SWEEP)} stroke="#DCE6E9" strokeWidth={TRACK_WIDTH} fill="none" strokeLinecap="round" />
      {/* 数值弧：值越大越深 */}
      {shown > 0.002 ? <path d={arcPath(CX, CY, R_TRACK, START, angle)} stroke={`url(#${id}-arc)`} strokeWidth={TRACK_WIDTH} fill="none" strokeLinecap="round" /> : null}
      {/* 转动时的彗尾：压在数值弧上，头部最宽最实，往后渐细渐隐（线宽 = 轨道宽 + 额外宽 × 权重 × 强度，不透明度 = 权重 × 强度） */}
      {comet.length ? (
        <g fill="none" strokeLinecap="round">
          {comet.map((s, j) => {
            const amount = s.weight * frame.strength;
            const middle = ((s.fromAngle + s.toAngle) / 2 - START_RAD) / SWEEP_RAD;
            return (
              <path
                key={j}
                d={arcPath(CX, CY, R_TRACK, toDegrees(s.fromAngle), toDegrees(s.toAngle))}
                stroke={cyanAt(0.12 + 0.88 * middle)}
                strokeOpacity={amount.toFixed(3)}
                strokeWidth={(TRACK_WIDTH + COMET_EXTRA_WIDTH * amount).toFixed(2)}
              />
            );
          })}
        </g>
      ) : null}
      {/* 紫色 = GLAZE 此刻的自动加量 */}
      {autoIncrement > 0.5 && autoEnd > angle + 0.5 ? (
        <path d={arcPath(CX, CY, R_TRACK + 9, angle, autoEnd)} stroke="#6A4DFF" strokeWidth="2" fill="none" strokeLinecap="round" opacity="0.85" />
      ) : null}
      {/* 刻度点 */}
      {Array.from({ length: 11 }, (_, i) => {
        const a = ((START + (SWEEP * i) / 10 - 90) * Math.PI) / 180;
        return <circle key={i} cx={CX + R_TICKS * Math.cos(a)} cy={CY + R_TICKS * Math.sin(a)} r={i % 5 === 0 ? 1.8 : 1.1} fill={i / 10 <= shown + 1e-6 ? '#127F90' : '#C7D3D6'} />;
      })}

      {/* 落在纸上的接触影 */}
      <ellipse cx={CX} cy={CY + R_DISC + 12} rx={R_DISC * 0.72} ry="12" fill="#2E4A42" opacity="0.14" filter={`url(#${id}-soft)`} />

      {/* 釉面 */}
      <g filter={`url(#${id}-shadow)`}>
        <circle cx={CX} cy={CY} r={R_DISC} fill={`url(#${id}-glaze)`} />
      </g>
      <g clipPath={`url(#${id}-clip)`}>
        {/* 釉色的起伏：几团极淡的深浅 */}
        <ellipse cx={CX - 44} cy={CY + 58} rx="92" ry="48" fill="#5C8579" opacity={0.08 + porcelain * 0.08} filter={`url(#${id}-soft)`} />
        <ellipse cx={CX + 64} cy={CY - 26} rx="66" ry="40" fill="#FFFFFF" opacity="0.28" filter={`url(#${id}-soft)`} />
        {/* 开片：与插件同一张网（大裂 → 中裂 → 细裂依次露出）；0 % 时没有 */}
        <g transform={`translate(${CX} ${CY})`}>
          <Crackle radius={R_DISC} value={porcelain} />
        </g>
      </g>
      <circle cx={CX} cy={CY} r={R_DISC} fill={`url(#${id}-pool)`} opacity={pool} />
      <circle cx={CX} cy={CY} r={R_DISC - 3} fill="none" stroke={`url(#${id}-rim)`} strokeWidth="6" filter={`url(#${id}-hair)`} />
      <circle cx={CX} cy={CY} r={R_DISC - 0.75} fill="none" stroke="#3D6258" strokeOpacity={0.3 + porcelain * 0.2} strokeWidth="1.5" />

      {/* 中心：同釉浅凹窝 + 一圈刻线（上沿暗、下沿亮，像刻出来的） */}
      <circle cx={CX} cy={CY} r={R_DIMPLE} fill={`url(#${id}-dimple)`} />
      <path d={arcPath(CX, CY, R_DIMPLE, -120, 120)} stroke="#3D6258" strokeOpacity="0.38" strokeWidth="1.1" fill="none" transform={`rotate(180 ${CX} ${CY})`} />
      <path d={arcPath(CX, CY, R_DIMPLE + 1.2, -120, 120)} stroke="#FFFFFF" strokeOpacity="0.7" strokeWidth="1" fill="none" />

      {/* 棚灯反光：左上一块柔边亮面 + 一道细高光弧 */}
      <rect x={CX - 104} y={CY - 112} width="86" height="42" rx="21" fill="#FFFFFF" opacity="0.55" filter={`url(#${id}-soft)`} transform={`rotate(-34 ${CX - 61} ${CY - 91})`} />
      <path d={arcPath(CX, CY, R_DISC - 9, -62, -18)} stroke="#FFFFFF" strokeOpacity="0.75" strokeWidth="2" strokeLinecap="round" fill="none" filter={`url(#${id}-hair)`} />

      {/* 指针：釉面边缘刻出的短槽（暗线 + 右侧一道亮边） */}
      <g transform={`rotate(${angle.toFixed(2)} ${CX} ${CY})`}>
        <line x1={CX} y1={CY - R_DISC + 13} x2={CX} y2={CY - R_DISC + 40} stroke="#2F564C" strokeWidth="3.2" strokeLinecap="round" opacity="0.85" />
        <line x1={CX + 1.8} y1={CY - R_DISC + 14} x2={CX + 1.8} y2={CY - R_DISC + 39} stroke="#FFFFFF" strokeWidth="1" strokeLinecap="round" opacity="0.7" />
      </g>

      {/* 读数 */}
      <text y={CY + R_DISC + 60} textAnchor="middle" className="font-display" style={{ fontSize: READOUT_PX, fontWeight: 500, fill: '#0D1B21', letterSpacing: '-0.02em', fontVariationSettings: '"opsz" 144, "SOFT" 30' }}>
        {digits.map((d, k) => (
          <tspan key={k} x={readoutX0 + READOUT_CELL * (k + 0.5)}>
            {d}
          </tspan>
        ))}
        <tspan x={readoutX0 + digits.length * READOUT_CELL + PERCENT_GAP} textAnchor="start" style={{ fontSize: 20, fill: '#586C74' }}>
          %
        </tspan>
      </text>
      <text x={CX} y={CY + R_DISC + 84} textAnchor="middle" style={{ fontSize: 11, letterSpacing: '0.28em', fill: '#6E8189', fontFamily: 'var(--font-sans)' }}>
        {label}
      </text>
    </svg>
  );
}
