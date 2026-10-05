import { useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { gsap, useGSAP } from '@/lib/gsap';
import { Chapter } from './Eyebrow';
import { cn, prefersReducedMotion, seeded } from '@/lib/utils';

// 文字（名称、动作、导语、要点）在 i18n 的 home 命名空间 process.stages.<key>；这里只留结构。
const stages = [
  { key: 'balance', index: '01', en: 'BALANCE' },
  { key: 'clay', index: '02', en: 'CLAY' },
  { key: 'color', index: '03', en: 'COLOR' },
  { key: 'limit', index: '04', en: 'LIMIT' }
] as const;

/**
 * 壹 · 四道工序：内容块居中时钉住，滚动推进四个阶段；右侧一块瓷面上的图跟着从「处理前」变到「处理后」。
 * 不做吸附（snap）：吸附会在滚动停下后自己拽页面，叠上 scrub 的延迟会出现阶段倒退的错觉，也等于劫持了用户的滚动。
 * 左侧竖排 01～04 刻度只在阶段变化时更新状态（不按帧）；减少动效时不钉住，四个阶段直接竖排、图停在「处理后」。
 */
export default function ProcessSection() {
  const root = useRef<HTMLElement>(null);
  const pin = useRef<HTMLDivElement>(null);
  const fill = useRef<HTMLSpanElement>(null);
  const [current, setCurrent] = useState(0);
  const [reduced] = useState(prefersReducedMotion);
  const { t } = useTranslation('home');

  useGSAP(
    () => {
      const section = root.current;
      const wrap = pin.current;
      if (!section || !wrap || reduced) return;
      const q = gsap.utils.selector(wrap);
      const panels = q<HTMLElement>('[data-stage]');
      const visuals = q<SVGGElement>('[data-visual]');
      gsap.set(panels.slice(1), { autoAlpha: 0, y: 32 });
      gsap.set(visuals.slice(1), { autoAlpha: 0 });

      let last = 0;
      const STEP = 1.5; // 每个阶段在时间线上占的长度：前 1.0 画图变化，后 0.5 换到下一阶段
      const tl = gsap.timeline({
        defaults: { ease: 'none' },
        // 当前阶段按时间线自己的播放位置算（scrub 平滑之后），刻度与面板同一时刻换；面板在 at + 1.2 开始换入
        onUpdate: () => {
          const i = Math.min(stages.length - 1, Math.floor(tl.time() / STEP + 0.2));
          if (i !== last) {
            last = i;
            setCurrent(i);
          }
        },
        scrollTrigger: {
          trigger: wrap,
          start: 'center center',
          end: () => `+=${window.innerHeight * 3}`,
          pin: wrap,
          pinSpacing: true,
          scrub: 0.6,
          anticipatePin: 1,
          invalidateOnRefresh: true
        }
      });

      if (fill.current) tl.fromTo(fill.current, { scaleY: 0 }, { scaleY: 1, duration: STEP * stages.length }, 0);
      stages.forEach((_, i) => {
        const at = i * STEP;
        tl.fromTo(`[data-visual="${i}"] [data-before]`, { opacity: 1 }, { opacity: 0, duration: 0.7 }, at + 0.15);
        tl.fromTo(`[data-visual="${i}"] [data-after]`, { opacity: 0 }, { opacity: 1, duration: 0.7 }, at + 0.15);
        tl.fromTo(`[data-visual="${i}"] [data-progress]`, { attr: { width: 0 } }, { attr: { width: 560 }, duration: 0.9 }, at);
        if (i < stages.length - 1) {
          tl.to(panels[i], { autoAlpha: 0, y: -32, duration: 0.25, ease: 'power2.in' }, at + 1.05);
          tl.to(visuals[i], { autoAlpha: 0, duration: 0.25 }, at + 1.05);
          tl.to(panels[i + 1], { autoAlpha: 1, y: 0, duration: 0.3, ease: 'power2.out' }, at + 1.2);
          tl.to(visuals[i + 1], { autoAlpha: 1, duration: 0.3 }, at + 1.2);
        }
      });
      tl.to({}, { duration: 0.3 });
    },
    { scope: root, dependencies: [reduced] }
  );

  if (reduced) {
    return (
      <section id="process" ref={root} className="section relative">
        <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
          <Chapter n="壹" label={t('chapters.process.label')} en={t('chapters.process.en')} />
          <p className="mt-6 text-[15px] text-ink-3">{t('process.intro')}</p>
          <div className="mt-12 space-y-20">
            {stages.map((s, i) => (
              <div key={s.key} className="grid gap-8 lg:grid-cols-12 lg:items-center">
                <div className="lg:col-span-5">
                  <StageText s={s} />
                </div>
                <div className="lg:col-span-7">
                  <div className="slab">
                    <div className="slab-core relative aspect-[4/3] md:aspect-[16/11] overflow-hidden">
                      <StageVisuals only={i} />
                    </div>
                  </div>
                </div>
              </div>
            ))}
          </div>
        </div>
      </section>
    );
  }

  return (
    <section id="process" ref={root} className="section relative">
      <div ref={pin} className="mx-auto w-full max-w-[1280px] px-5 sm:px-8">
        <div className="flex flex-wrap items-baseline justify-between gap-4">
          <Chapter n="壹" label={t('chapters.process.label')} en={t('chapters.process.en')} />
          <p className="text-[13px] text-ink-4">{t('process.introShort')}</p>
        </div>

        {/* 手机：顶部 4 段细进度 */}
        <ol className="mt-6 grid grid-cols-4 gap-2 lg:hidden" aria-label={t('process.current')}>
          {stages.map((s, i) => (
            <li key={s.key} aria-current={i === current ? 'step' : undefined}>
              <span className={cn('block h-0.5 rounded-full transition-colors duration-500', i <= current ? 'bg-c600' : 'bg-ink/12')} />
              <span className={cn('mt-2 block font-mono text-[10.5px] tracking-[0.14em] transition-colors duration-500', i === current ? 'text-c700' : 'text-ink-4')}>{s.en}</span>
            </li>
          ))}
        </ol>

        <div className="mt-8 grid gap-6 md:gap-10 lg:mt-12 lg:grid-cols-12 lg:gap-8 items-center">
          {/* 桌面：左侧竖排 01～04 刻度，细线随滚动填满 */}
          <ol className="relative hidden lg:col-span-2 lg:block" aria-label={t('process.current')}>
            <span className="absolute left-[5px] top-2 bottom-2 w-px bg-ink/12" aria-hidden="true" />
            <span ref={fill} className="absolute left-[5px] top-2 bottom-2 w-px origin-top scale-y-0 bg-c600" aria-hidden="true" />
            {stages.map((s, i) => (
              <li key={s.key} aria-current={i === current ? 'step' : undefined} className="relative flex items-center gap-4 py-5 pl-0">
                <span className={cn('relative z-10 size-[11px] rounded-full ring-2 ring-paper transition-colors duration-500', i <= current ? 'bg-c600' : 'bg-line-2')} />
                <span className={cn('font-mono text-[12px] tracking-[0.14em] transition-colors duration-500', i === current ? 'text-c700' : 'text-ink-4')}>
                  {s.index} {s.en}
                </span>
              </li>
            ))}
          </ol>

          <div className="lg:col-span-4 relative min-h-[260px] md:min-h-[360px]">
            {stages.map((s, i) => (
              <div key={s.key} data-stage={i} className={i === 0 ? 'relative' : 'absolute inset-0'}>
                <StageText s={s} />
              </div>
            ))}
          </div>

          <div className="lg:col-span-6">
            <div className="slab">
              <div className="slab-core relative aspect-[4/3] md:aspect-[16/11] overflow-hidden">
                <StageVisuals />
              </div>
            </div>
            <p className="mt-3 text-center text-[12px] text-ink-4">{t('process.caption')}</p>
          </div>
        </div>
      </div>
    </section>
  );
}

function StageText({ s }: { s: (typeof stages)[number] }) {
  const { t } = useTranslation('home');
  const facts = t(`process.stages.${s.key}.facts`, { returnObjects: true });
  return (
    <>
      <div className="flex items-baseline gap-4">
        <span className="display text-[48px] md:text-[60px] leading-none text-c300 tabular-nums">{s.index}</span>
        <span className="display text-[22px] text-ink-3 tracking-[0.12em]">{s.en}</span>
      </div>
      {/* 名称一行、动作一行：窄栏里不会在「·」后面断成半截 */}
      <h3 className="mt-4 font-serif text-[clamp(1.9rem,3.2vw,2.75rem)] font-semibold leading-[1.18] text-ink">
        <span className="block">{t(`process.stages.${s.key}.name`)}</span>
        <span className="block text-c700">{t(`process.stages.${s.key}.verb`)}</span>
      </h3>
      <p className="mt-4 md:mt-5 max-w-md text-[15px] md:text-[16px] leading-[1.8] md:leading-[1.85] text-ink-2">{t(`process.stages.${s.key}.lead`)}</p>
      <ul className="mt-6 space-y-2.5 hidden md:block">
        {facts.map(f => (
          <li key={f} className="flex items-start gap-3 text-[14px] text-ink-3">
            <span className="mt-[9px] size-1.5 shrink-0 rounded-full bg-c500" />
            {f}
          </li>
        ))}
      </ul>
    </>
  );
}

/* ---------------- 四张示意图 ---------------- */

const W = 640;
const H = 440;

/** only：只画这一张并停在「处理后」（减少动效时的竖排）。 */
function StageVisuals({ only }: { only?: number }) {
  const show = (i: number) => only === undefined || only === i;
  return (
    <svg viewBox={`0 0 ${W} ${H}`} className={cn('absolute inset-0 h-full w-full', only !== undefined && '[&_[data-before]]:opacity-0 [&_[data-progress]]:hidden')} aria-hidden="true">
      <defs>
        <linearGradient id="pv-fill" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="#67CAD7" stopOpacity="0.55" />
          <stop offset="1" stopColor="#67CAD7" stopOpacity="0.05" />
        </linearGradient>
        <linearGradient id="pv-wave" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stopColor="#127F90" />
          <stop offset="1" stopColor="#1799AB" />
        </linearGradient>
        <pattern id="pv-grid" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M40 0H0V40" fill="none" stroke="#DCE6E9" strokeWidth="1" />
        </pattern>
      </defs>
      <rect width={W} height={H} fill="url(#pv-grid)" opacity="0.6" />
      {show(0) ? <g data-visual="0"><BalanceVisual /></g> : null}
      {show(1) ? <g data-visual="1"><ClayVisual /></g> : null}
      {show(2) ? <g data-visual="2"><ColorVisual /></g> : null}
      {show(3) ? <g data-visual="3"><LimitVisual /></g> : null}
    </svg>
  );
}

function Frame({ title, note }: { title: string; note: string }) {
  return (
    <>
      <text x="40" y="44" style={{ fontFamily: 'var(--font-mono)', fontSize: 12, letterSpacing: '0.18em', fill: '#586C74' }}>{title}</text>
      <text x={W - 40} y="44" textAnchor="end" style={{ fontFamily: 'var(--font-sans)', fontSize: 12, fill: '#6E8189' }}>{note}</text>
      <rect x="40" y="56" width="560" height="2" fill="#E6EEF1" />
      <rect data-progress x="40" y="56" width="0" height="2" fill="#1799AB" />
    </>
  );
}

function BalanceVisual() {
  const { t } = useTranslation('home');
  const rnd = seeded(3);
  const bands = Array.from({ length: 27 }, (_, i) => {
    const contour = 250 - Math.sin((i / 26) * Math.PI) * 60 - i * 1.4; // 歌自己的轮廓
    const dev = (rnd() - 0.5) * 70 * (i > 3 && i < 24 ? 1 : 0.3);
    return { contour, before: contour + dev, after: contour + dev * 0.15 };
  });
  const x0 = 60;
  const bw = 520 / 27;
  const contourPath = bands.map((b, i) => `${i === 0 ? 'M' : 'L'}${x0 + i * bw + bw / 2} ${b.contour}`).join(' ');
  return (
    <>
      <Frame title="BALANCE · 27 BANDS" note={t('process.visual.balanceNote')} />
      <path d={contourPath} fill="none" stroke="#0A4A57" strokeDasharray="4 5" strokeWidth="1.2" opacity="0.6" />
      <g data-before>
        {bands.map((b, i) => (
          <rect key={i} x={x0 + i * bw + 2} y={Math.min(b.before, 380)} width={bw - 4} height={Math.max(2, 380 - b.before)} rx="2" fill={b.before < b.contour - 6 ? '#E8590C' : '#A3E0E8'} opacity="0.85" />
        ))}
      </g>
      <g data-after>
        {bands.map((b, i) => (
          <rect key={i} x={x0 + i * bw + 2} y={Math.min(b.after, 380)} width={bw - 4} height={Math.max(2, 380 - b.after)} rx="2" fill="#1799AB" opacity="0.9" />
        ))}
        {bands.map((b, i) => {
          const g = b.contour - b.before; // 正 = 抬（Recover）负 = 压（Tame）
          if (Math.abs(g) < 6) return null;
          return <text key={`t${i}`} x={x0 + i * bw + bw / 2} y={b.after - 8} textAnchor="middle" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: g > 0 ? '#6A4DFF' : '#0E6473' }}>{g > 0 ? '+' : '−'}</text>;
        })}
      </g>
      <text x="60" y="412" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>40 Hz</text>
      <text x="580" y="412" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>16 kHz</text>
    </>
  );
}

function ClayVisual() {
  const { t } = useTranslation('home');
  // 立体声宽度随频率：上下包络。处理前低频很宽、次声有能量；处理后 120 Hz 以下收成单声道（画的是手动收单，ADR-015 起自动不收单，所以线与标签用手动的釉青、不用表示自动的紫）、次声切掉、2 kHz 以上略宽。
  const xs = Array.from({ length: 40 }, (_, i) => 60 + (i / 39) * 520);
  const mid = 236;
  const before = xs.map((x, i) => {
    const t = i / 39;
    const w = 26 + 60 * Math.exp(-t * 3.2) + 14 * Math.sin(t * 9);
    return [x, w] as const;
  });
  const after = xs.map((x, i) => {
    const t = i / 39;
    const mono = t < 0.16 ? 0 : Math.min(1, (t - 0.16) / 0.08);
    const widen = t > 0.62 ? 1 + 0.35 * Math.min(1, (t - 0.62) / 0.2) : 1;
    const w = (26 + 14 * Math.sin(t * 9)) * mono * widen;
    return [x, w] as const;
  });
  const band = (pts: readonly (readonly [number, number])[]) => {
    const top = pts.map(([x, w], i) => `${i === 0 ? 'M' : 'L'}${x.toFixed(1)} ${(mid - w).toFixed(1)}`).join(' ');
    const bottom = [...pts].reverse().map(([x, w]) => `L${x.toFixed(1)} ${(mid + w).toFixed(1)}`).join(' ');
    return `${top} ${bottom} Z`;
  };
  return (
    <>
      <Frame title="CLAY · STEREO WIDTH vs FREQUENCY" note={t('process.visual.clayNote')} />
      <line x1="60" y1={mid} x2="580" y2={mid} stroke="#0A4A57" strokeWidth="1.2" opacity="0.7" />
      <g data-before>
        <path d={band(before)} fill="url(#pv-fill)" stroke="#33B3C4" strokeWidth="1" />
        <rect x="60" y={mid - 60} width="24" height="120" fill="#E8590C" opacity="0.18" />
        <text x="72" y={mid + 84} textAnchor="middle" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#C2410C' }}>{t('process.visual.infrasonic')}</text>
      </g>
      <g data-after>
        <path d={band(after)} fill="url(#pv-fill)" stroke="#127F90" strokeWidth="1.2" />
        <line x1="143" y1={mid - 110} x2="143" y2={mid + 110} stroke="#127F90" strokeDasharray="3 4" strokeWidth="1" />
        <text x="148" y={mid - 100} style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>{t('process.visual.mono')}</text>
        <line x1="70" y1={mid - 110} x2="70" y2={mid + 110} stroke="#0E6473" strokeDasharray="3 4" strokeWidth="1" />
        <text x="75" y={mid + 105} style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>{t('process.visual.lowCut')}</text>
        <text x="500" y={mid - 70} style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#127F90' }}>{t('process.visual.width')}</text>
      </g>
      <text x="60" y="412" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>10 Hz</text>
      <text x="580" y="412" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>20 kHz</text>
    </>
  );
}

/** 一段带尖峰的鼓 + 贝斯波形；after = 过一遍软饱和再补一点增益：尖峰被削圆、包络更紧。 */
function colorWave(after: boolean) {
  const rnd = seeded(11);
  const pts: string[] = [];
  const n = 240;
  for (let i = 0; i <= n; i++) {
    const t = i / n;
    const beat = (t * 6) % 1;
    const hit = Math.exp(-beat * 9); // 每拍开头一个尖峰
    let y = 0.42 * Math.sin(t * Math.PI * 2 * 18) + hit * 0.62 * Math.sin(t * Math.PI * 2 * 57) + (rnd() - 0.5) * 0.05;
    if (after) y = Math.tanh(y * 1.9) / Math.tanh(1.9) * 0.78;
    pts.push(`${(60 + t * 520).toFixed(1)},${(150 - y * 80).toFixed(1)}`);
  }
  return pts.join(' ');
}

// 谐波频谱：基频在 150 Hz 以下的原样不动；以上的基频在处理后多出 2、3 次谐波
const SPEC = [
  { f: 55, a: 0.95, low: true },
  { f: 110, a: 0.7, low: true },
  { f: 180, a: 0.8, low: false },
  { f: 250, a: 0.55, low: false },
  { f: 410, a: 0.6, low: false },
  { f: 690, a: 0.35, low: false }
] as const; // 150 Hz 以上的基频互相不成整数倍，新添的谐波不会叠在已有的柱子上
const specX = (f: number) => 60 + (Math.log2(f / 40) / Math.log2(8000 / 40)) * 520;

function ColorVisual() {
  const { t } = useTranslation('home');
  const base = 392;
  const x150 = specX(150);
  return (
    <>
      <Frame title="COLOR · TONE → GLUE → HEAT" note={t('process.visual.colorNote')} />
      {/* 上：波形 */}
      <line x1="60" y1="150" x2="580" y2="150" stroke="#DCE6E9" />
      <text x="60" y="82" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>{t('process.visual.waveform')}</text>
      <g data-before>
        <polyline points={colorWave(false)} fill="none" stroke="#A3E0E8" strokeWidth="1.3" />
      </g>
      <g data-after>
        <polyline points={colorWave(false)} fill="none" stroke="#A3E0E8" strokeWidth="1" opacity="0.45" />
        <polyline points={colorWave(true)} fill="none" stroke="#127F90" strokeWidth="1.5" />
      </g>
      {/* 下：谐波频谱 */}
      <text x="60" y="262" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>{t('process.visual.spectrum')}</text>
      <line x1="60" y1={base} x2="580" y2={base} stroke="#DCE6E9" />
      <line x1={x150} y1="268" x2={x150} y2={base} stroke="#0D1B21" strokeOpacity="0.35" strokeDasharray="3 4" />
      <text x={x150 - 6} y="280" textAnchor="end" style={{ fontFamily: 'var(--font-sans)', fontSize: 11, fill: '#586C74' }}>{t('process.visual.below150')}</text>
      {SPEC.map(p => (
        <rect key={p.f} x={specX(p.f) - 3} y={base - p.a * 100} width="6" height={p.a * 100} rx="2" fill={p.low ? '#67CAD7' : '#33B3C4'} />
      ))}
      <g data-after>
        {SPEC.filter(p => !p.low).flatMap(p =>
          [2, 3].filter(k => p.f * k < 8000).map(k => (
            <rect key={`${p.f}-${k}`} x={specX(p.f * k) - 2.5} y={base - p.a * (k === 2 ? 34 : 18)} width="5" height={p.a * (k === 2 ? 34 : 18)} rx="2" fill="#0A4A57" opacity="0.85" />
          ))
        )}
        <text x="580" y={base - 58} textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>{t('process.visual.newHarmonics')}</text>
      </g>
      <text x="60" y="412" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>40 Hz</text>
      <text x="580" y="412" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>8 kHz</text>
    </>
  );
}

function LimitVisual() {
  const { t } = useTranslation('home');
  const rnd = seeded(5);
  const n = 120;
  const ceiling = 120; // y of ceiling line (top)
  const mid = 236;
  const bars: { x: number; before: number; after: number }[] = [];
  for (let i = 0; i < n; i++) {
    const t = i / n;
    const beat = Math.exp(-((t * 8) % 1) * 4);
    const amp = 40 + 30 * rnd() + beat * 70;
    const before = amp;
    const gained = amp * 1.55; // 推 +3.8 dB
    const after = Math.min(gained, mid - ceiling - 2);
    bars.push({ x: 60 + t * 520, before, after });
  }
  return (
    <>
      <Frame title="LIMIT · TRUE PEAK, 8× DETECT / 1× APPLY" note={t('process.visual.limitNote')} />
      <line x1="60" y1={ceiling} x2="580" y2={ceiling} stroke="#E8590C" strokeDasharray="5 5" strokeWidth="1.2" />
      <line x1="60" y1={mid * 2 - ceiling} x2="580" y2={mid * 2 - ceiling} stroke="#E8590C" strokeDasharray="5 5" strokeWidth="1.2" />
      <g data-before>
        {bars.map((b, i) => (
          <line key={i} x1={b.x} y1={mid - b.before} x2={b.x} y2={mid + b.before} stroke="#A3E0E8" strokeWidth="2.4" strokeLinecap="round" />
        ))}
        <text x="580" y={ceiling - 8} textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>−8.6 LUFS</text>
      </g>
      <g data-after>
        {bars.map((b, i) => (
          <line key={i} x1={b.x} y1={mid - b.after} x2={b.x} y2={mid + b.after} stroke={b.after < b.before * 1.55 - 0.5 ? '#0A4A57' : '#1799AB'} strokeWidth="2.4" strokeLinecap="round" />
        ))}
        <text x="580" y={ceiling - 8} textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>−7.9 LUFS · GR 2.6 dB</text>
      </g>
      <text x="60" y="412" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6E8189' }}>{t('process.visual.limitChain')}</text>
    </>
  );
}
