import { useCallback, useEffect, useId, useRef, type PointerEvent as ReactPointerEvent, type KeyboardEvent } from 'react';
import Crackle from './Crackle';
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
const R_TRACK = 176;
const R_DISC = 150;
const R_DIMPLE = 44;

/**
 * 青瓷主旋钮（设计规格-官网 第 5 节）：豆青釉面（与插件同色） + 随 GLAZE 生长的开片 + 边缘积釉 + 底部透光。
 * 中心是同釉的浅凹窝和一圈刻线，指针是釉面边缘刻出的短槽——不再有深色旋钮帽（那样像眼球）。
 * 交互照插件：上下拖动调节，⌘ / Ctrl 精调，滚轮微调，双击回 35，键盘 ↑↓ / PageUp / Home / End。
 */
export default function GlazeKnob({ value, onChange, autoIncrement = 0, size = 320, interactive = true, className, label = 'GLAZE' }: GlazeKnobProps) {
  const id = useId().replace(/:/g, '');
  const drag = useRef<{ startY: number; startV: number; fine: boolean } | null>(null);
  const ref = useRef<SVGSVGElement>(null);
  // 滚轮处理读最新值用 ref，监听器只挂一次（不随每次数值变化重挂）。
  const valueRef = useRef(value);
  valueRef.current = value;

  const v01 = clamp(value / 100, 0, 1);
  const angle = START + SWEEP * v01;
  const autoEnd = clamp(angle + (SWEEP * autoIncrement) / 100, START, START + SWEEP);

  const set = useCallback((v: number) => onChange?.(clamp(Math.round(v * 10) / 10, 0, 100)), [onChange]);

  const onPointerDown = (e: ReactPointerEvent<SVGSVGElement>) => {
    if (!interactive) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    drag.current = { startY: e.clientY, startV: value, fine: e.metaKey || e.ctrlKey };
  };
  const onPointerMove = (e: ReactPointerEvent<SVGSVGElement>) => {
    if (!drag.current) return;
    const dy = drag.current.startY - e.clientY;
    const perPixel = (drag.current.fine ? 100 / 8 : 100) / 220; // 拖 220 px 走满刻度；精调慢 8 倍
    set(drag.current.startV + dy * perPixel);
  };
  const onPointerUp = (e: ReactPointerEvent<SVGSVGElement>) => {
    drag.current = null;
    try {
      e.currentTarget.releasePointerCapture(e.pointerId);
    } catch {
      /* 指针已经释放 */
    }
  };
  const onKey = (e: KeyboardEvent<SVGSVGElement>) => {
    if (!interactive) return;
    const step = e.shiftKey ? 10 : 1;
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') set(value + step);
    else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') set(value - step);
    else if (e.key === 'PageUp') set(value + 10);
    else if (e.key === 'PageDown') set(value - 10);
    else if (e.key === 'Home') set(0);
    else if (e.key === 'End') set(100);
    else return;
    e.preventDefault();
  };

  useEffect(() => {
    // 滚轮要 passive: false 才能 preventDefault；React 的 onWheel 是 passive 的，这里手动挂。
    const el = ref.current;
    if (!el || !interactive) return;
    const handler = (e: globalThis.WheelEvent) => {
      e.preventDefault();
      set(valueRef.current - Math.sign(e.deltaY) * (e.metaKey || e.ctrlKey ? 0.5 : 2));
    };
    el.addEventListener('wheel', handler, { passive: false });
    return () => el.removeEventListener('wheel', handler);
  }, [interactive, set]);

  // 釉越厚（GLAZE 越大）边缘积釉越深、开片越密越清楚
  const pool = 0.45 + v01 * 0.55;
  // 釉色随 GLAZE 从浅灰青走到较深的豆青：与插件 renderGlaze 同一组端点色、同一条 t^0.75 曲线
  const depth = Math.pow(v01, 0.75);
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
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
      onDoubleClick={() => interactive && set(35)}
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
      <path d={arcPath(CX, CY, R_TRACK, START, START + SWEEP)} stroke="#DCE6E9" strokeWidth="5" fill="none" strokeLinecap="round" />
      {/* 数值弧：值越大越深 */}
      {v01 > 0.002 ? <path d={arcPath(CX, CY, R_TRACK, START, angle)} stroke={`url(#${id}-arc)`} strokeWidth="5" fill="none" strokeLinecap="round" /> : null}
      {/* 紫色 = GLAZE 此刻的自动加量 */}
      {autoIncrement > 0.5 && autoEnd > angle + 0.5 ? (
        <path d={arcPath(CX, CY, R_TRACK + 9, angle, autoEnd)} stroke="#6A4DFF" strokeWidth="2" fill="none" strokeLinecap="round" opacity="0.85" />
      ) : null}
      {/* 刻度点 */}
      {Array.from({ length: 11 }, (_, i) => {
        const a = ((START + (SWEEP * i) / 10 - 90) * Math.PI) / 180;
        const r0 = R_TRACK + 16;
        return <circle key={i} cx={CX + r0 * Math.cos(a)} cy={CY + r0 * Math.sin(a)} r={i % 5 === 0 ? 1.8 : 1.1} fill={i / 10 <= v01 + 1e-6 ? '#127F90' : '#C7D3D6'} />;
      })}

      {/* 落在纸上的接触影 */}
      <ellipse cx={CX} cy={CY + R_DISC + 12} rx={R_DISC * 0.72} ry="12" fill="#2E4A42" opacity="0.14" filter={`url(#${id}-soft)`} />

      {/* 釉面 */}
      <g filter={`url(#${id}-shadow)`}>
        <circle cx={CX} cy={CY} r={R_DISC} fill={`url(#${id}-glaze)`} />
      </g>
      <g clipPath={`url(#${id}-clip)`}>
        {/* 釉色的起伏：几团极淡的深浅 */}
        <ellipse cx={CX - 44} cy={CY + 58} rx="92" ry="48" fill="#5C8579" opacity={0.08 + v01 * 0.08} filter={`url(#${id}-soft)`} />
        <ellipse cx={CX + 64} cy={CY - 26} rx="66" ry="40" fill="#FFFFFF" opacity="0.28" filter={`url(#${id}-soft)`} />
        {/* 开片：与插件同一张网（大裂 → 中裂 → 细裂依次露出）；0 % 时没有 */}
        <g transform={`translate(${CX} ${CY})`}>
          <Crackle radius={R_DISC} value={v01} />
        </g>
      </g>
      <circle cx={CX} cy={CY} r={R_DISC} fill={`url(#${id}-pool)`} opacity={pool} />
      <circle cx={CX} cy={CY} r={R_DISC - 3} fill="none" stroke={`url(#${id}-rim)`} strokeWidth="6" filter={`url(#${id}-hair)`} />
      <circle cx={CX} cy={CY} r={R_DISC - 0.75} fill="none" stroke="#3D6258" strokeOpacity={0.3 + v01 * 0.2} strokeWidth="1.5" />

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
      <text x={CX} y={CY + R_DISC + 60} textAnchor="middle" className="font-display" style={{ fontSize: 44, fontWeight: 500, fill: '#0D1B21', letterSpacing: '-0.02em', fontVariationSettings: '"opsz" 144, "SOFT" 30' }}>
        {Math.round(value)}
        <tspan style={{ fontSize: 20, fill: '#637780' }} dx="4">
          %
        </tspan>
      </text>
      <text x={CX} y={CY + R_DISC + 84} textAnchor="middle" style={{ fontSize: 11, letterSpacing: '0.28em', fill: '#8A9CA3', fontFamily: 'var(--font-sans)' }}>
        {label}
      </text>
    </svg>
  );
}
