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

function arcPath(cx: number, cy: number, r: number, a0: number, a1: number) {
  const rad = (d: number) => ((d - 90) * Math.PI) / 180;
  const x0 = cx + r * Math.cos(rad(a0));
  const y0 = cy + r * Math.sin(rad(a0));
  const x1 = cx + r * Math.cos(rad(a1));
  const y1 = cy + r * Math.sin(rad(a1));
  const large = a1 - a0 > 180 ? 1 : 0;
  return `M ${x0} ${y0} A ${r} ${r} 0 ${large} 1 ${x1} ${y1}`;
}

/**
 * 青瓷主旋钮：釉面 + 开片随 GLAZE 生长 + 深色旋钮帽。
 * 交互照插件：上下拖动调节，滚轮微调，双击回 35，键盘 ↑↓ / PageUp / Home / End。
 */
export default function GlazeKnob({ value, onChange, autoIncrement = 0, size = 320, interactive = true, className, label = 'GLAZE' }: GlazeKnobProps) {
  const id = useId().replace(/:/g, '');
  const drag = useRef<{ startY: number; startV: number; fine: boolean } | null>(null);
  const ref = useRef<SVGSVGElement>(null);

  const v01 = clamp(value / 100, 0, 1);
  const cx = 200;
  const cy = 200;
  const rTrack = 176;
  const rDisc = 150;
  const angle = START + SWEEP * v01;
  const autoEnd = clamp(angle + (SWEEP * autoIncrement) / 100, START, START + SWEEP);

  const set = useCallback((v: number) => onChange?.(clamp(Math.round(v * 10) / 10, 0, 100)), [onChange]);

  const onPointerDown = (e: ReactPointerEvent<SVGSVGElement>) => {
    if (!interactive) return;
    (e.currentTarget as SVGSVGElement).setPointerCapture(e.pointerId);
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
      (e.currentTarget as SVGSVGElement).releasePointerCapture(e.pointerId);
    } catch {
      /* ignore */
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
      set(value - Math.sign(e.deltaY) * (e.metaKey || e.ctrlKey ? 0.5 : 2));
    };
    el.addEventListener('wheel', handler, { passive: false });
    return () => el.removeEventListener('wheel', handler);
  }, [interactive, set, value]);

  const glazeDepth = 0.35 + v01 * 0.65;

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
      tabIndex={interactive ? 0 : -1}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
      onDoubleClick={() => interactive && set(35)}
      onKeyDown={onKey}
    >
      <defs>
        <radialGradient id={`${id}-glaze`} cx="38%" cy="30%" r="78%">
          <stop offset="0" stopColor="#E9F7F8" />
          <stop offset="0.35" stopColor="#BFE9EE" />
          <stop offset="0.72" stopColor="#7ED1DB" />
          <stop offset="1" stopColor="#2E8E9E" />
        </radialGradient>
        <radialGradient id={`${id}-deep`} cx="45%" cy="45%" r="70%">
          <stop offset="0.55" stopColor="#1799AB" stopOpacity="0" />
          <stop offset="1" stopColor="#0A4A57" stopOpacity="0.9" />
        </radialGradient>
        <radialGradient id={`${id}-cap`} cx="40%" cy="30%" r="80%">
          <stop offset="0" stopColor="#1F4049" />
          <stop offset="1" stopColor="#061519" />
        </radialGradient>
        <linearGradient id={`${id}-arc`} x1="0" y1="1" x2="1" y2="0">
          <stop offset="0" stopColor="#A3E0E8" />
          <stop offset="0.5" stopColor="#1799AB" />
          <stop offset="1" stopColor="#0A4A57" />
        </linearGradient>
        <filter id={`${id}-soft`} x="-40%" y="-40%" width="180%" height="180%">
          <feGaussianBlur stdDeviation="14" />
        </filter>
        <filter id={`${id}-shadow`} x="-40%" y="-40%" width="180%" height="200%">
          <feGaussianBlur in="SourceAlpha" stdDeviation="16" result="b" />
          <feOffset dy="22" in="b" result="o" />
          <feComponentTransfer in="o" result="s">
            <feFuncA type="linear" slope="0.28" />
          </feComponentTransfer>
          <feMerge>
            <feMergeNode in="s" />
            <feMergeNode in="SourceGraphic" />
          </feMerge>
        </filter>
        <clipPath id={`${id}-clip`}>
          <circle cx={cx} cy={cy} r={rDisc} />
        </clipPath>
      </defs>

      {/* 270° 轨道 */}
      <path d={arcPath(cx, cy, rTrack, START, START + SWEEP)} stroke="#DCE6E9" strokeWidth="6" fill="none" strokeLinecap="round" />
      {/* 数值弧：值越大越深 */}
      {v01 > 0.002 ? (
        <path d={arcPath(cx, cy, rTrack, START, angle)} stroke={`url(#${id}-arc)`} strokeWidth="6" fill="none" strokeLinecap="round" style={{ transition: 'd 120ms linear' }} />
      ) : null}
      {/* 紫色 = GLAZE 此刻的自动加量 */}
      {autoIncrement > 0.5 && autoEnd > angle + 0.5 ? (
        <path d={arcPath(cx, cy, rTrack + 9, angle, autoEnd)} stroke="#6A4DFF" strokeWidth="2.5" fill="none" strokeLinecap="round" opacity="0.9" />
      ) : null}
      {/* 刻度点 */}
      {Array.from({ length: 11 }).map((_, i) => {
        const a = ((START + (SWEEP * i) / 10 - 90) * Math.PI) / 180;
        const r0 = rTrack + 16;
        return <circle key={i} cx={cx + r0 * Math.cos(a)} cy={cy + r0 * Math.sin(a)} r={i % 5 === 0 ? 2 : 1.2} fill={i / 10 <= v01 + 1e-6 ? '#127F90' : '#C7D3D6'} />;
      })}

      {/* 瓷面 */}
      <g filter={`url(#${id}-shadow)`}>
        <circle cx={cx} cy={cy} r={rDisc} fill={`url(#${id}-glaze)`} />
      </g>
      <circle cx={cx} cy={cy} r={rDisc} fill={`url(#${id}-deep)`} opacity={glazeDepth} />
      {/* 云斑 */}
      <g clipPath={`url(#${id}-clip)`} opacity={0.35 + v01 * 0.25}>
        <ellipse cx={cx - 40} cy={cy + 50} rx="90" ry="46" fill="#0E6473" opacity="0.18" filter={`url(#${id}-soft)`} />
        <ellipse cx={cx + 60} cy={cy - 30} rx="70" ry="40" fill="#D5F1F5" opacity="0.5" filter={`url(#${id}-soft)`} />
      </g>
      {/* 开片：随 GLAZE 生长 */}
      <g clipPath={`url(#${id}-clip)`} transform={`translate(${cx} ${cy})`}>
        <Crackle radius={rDisc} value={v01} color="#0A4A57" opacity={0.28 + v01 * 0.42} />
      </g>
      {/* 边缘积釉 */}
      <circle cx={cx} cy={cy} r={rDisc - 1.5} fill="none" stroke="#0A4A57" strokeOpacity={0.22 + v01 * 0.25} strokeWidth="3" />
      <circle cx={cx} cy={cy} r={rDisc - 4} fill="none" stroke="#FFFFFF" strokeOpacity="0.45" strokeWidth="1" />
      {/* 宽软反光 */}
      <ellipse cx={cx - 46} cy={cy - 78} rx="66" ry="26" fill="#FFFFFF" opacity="0.5" filter={`url(#${id}-soft)`} transform={`rotate(-28 ${cx - 46} ${cy - 78})`} />

      {/* 旋钮帽与指针 */}
      <g transform={`rotate(${angle} ${cx} ${cy})`} style={{ transition: 'transform 120ms linear' }}>
        <line x1={cx} y1={cy - rDisc + 10} x2={cx} y2={cy - rDisc + 34} stroke="#06313A" strokeWidth="3" strokeLinecap="round" opacity="0.9" />
        <circle cx={cx} cy={cy - rDisc + 22} r="4.5" fill="#F3F1EA" stroke="#06313A" strokeWidth="1.5" />
      </g>
      <circle cx={cx} cy={cy} r="38" fill={`url(#${id}-cap)`} />
      <circle cx={cx} cy={cy} r="38" fill="none" stroke="#FFFFFF" strokeOpacity="0.12" />
      <circle cx={cx} cy={cy - 20} r="1.6" fill="#A3E0E8" opacity="0.9" />

      {/* 读数 */}
      <text x={cx} y={cy + rDisc + 58} textAnchor="middle" className="font-display" style={{ fontSize: 44, fontWeight: 500, fill: '#0D1B21', letterSpacing: '-0.02em', fontVariationSettings: '"opsz" 144, "SOFT" 30' }}>
        {Math.round(value)}
        <tspan style={{ fontSize: 20, fill: '#637780' }} dx="4">
          %
        </tspan>
      </text>
      <text x={cx} y={cy + rDisc + 82} textAnchor="middle" style={{ fontSize: 11, letterSpacing: '0.28em', fill: '#8A9CA3', fontFamily: 'var(--font-sans)' }}>
        {label}
      </text>
    </svg>
  );
}
