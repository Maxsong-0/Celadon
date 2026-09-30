import { useRef } from 'react';
import { gsap, useGSAP } from '@/lib/gsap';
import { Eyebrow } from './Eyebrow';
import { prefersReducedMotion, seeded } from '@/lib/utils';

const stages = [
  {
    key: 'balance',
    index: '01',
    en: 'BALANCE',
    zh: '平衡',
    verb: '找回被遮住的',
    lead: '27 段动态频谱平衡。参考不是别人的曲线，是这首歌自己抹平后的轮廓：突出的压下去，被盖住的提起来。',
    facts: ['1/3 倍频程 27 段，40 Hz ～ 16 kHz', '死区 ±1 dB，每 hop 最多变 1 dB', '零延迟，Recover / Tame / Brighten / Speed']
  },
  {
    key: 'clay',
    index: '02',
    en: 'CLAY',
    zh: '修坯',
    verb: '把坯体修干净',
    lead: '施釉之前先修坯。切掉听不见的次声与直流，低频收成单声道，窄的歌适度加宽；先在这首歌上试一遍，会抬峰值就自动让开。',
    facts: ['4 阶低切 10 ～ 60 Hz，线性相位收单', '只动 S：单声道回放完全不变', '峰值守护：修坯从不让峰值变高']
  },
  {
    key: 'color',
    index: '03',
    en: 'COLOR',
    zh: '染色',
    verb: '添一层温度',
    lead: 'Tone 跷跷板、Glue 分频施压的总线压缩、Heat 保留低频主体的电子管残差染色。密一点、暖一点，后面的限幅就能推得更响。',
    facts: ['Heat 8× 过采样，150 Hz 以下原声不碰', 'Glue 低 / 中 / 高分开施压', '两侧更轻、奇对称：声像不飘']
  },
  {
    key: 'limit',
    index: '04',
    en: 'LIMIT',
    zh: '限幅',
    verb: '推响不破',
    lead: '前瞻真峰值限幅：8× 插值看见采样点之间的峰，1× 域施加增益。三角攻击沿、快慢双包络、持续低音的周期释放。',
    facts: ['真峰值 ≤ Ceiling，另留 0.35 dB 重建余量', '短过载按快释放回来，长过载慢包络压住', 'Clip 软削波 4× 过采样先削圆']
  }
];

/** 「四道工序」：整段钉住，滚动推进四个阶段；右侧一块瓷面上的图跟着从「处理前」变到「处理后」。 */
export default function ProcessSection() {
  const root = useRef<HTMLElement>(null);
  const pin = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      const section = root.current;
      const wrap = pin.current;
      if (!section || !wrap) return;
      const q = gsap.utils.selector(wrap);
      const reduced = prefersReducedMotion();

      const panels = q<HTMLElement>('[data-stage]');
      const visuals = q<SVGGElement>('[data-visual]');
      gsap.set(panels.slice(1), { autoAlpha: 0, y: 40 });
      gsap.set(visuals.slice(1), { autoAlpha: 0 });

      const tl = gsap.timeline({
        defaults: { ease: 'none' },
        scrollTrigger: {
          trigger: section,
          start: 'top top',
          end: () => `+=${window.innerHeight * 3.4}`,
          pin: wrap,
          pinSpacing: true,
          scrub: reduced ? false : 0.8,
          snap: reduced ? undefined : { snapTo: 'labels', duration: { min: 0.2, max: 0.6 }, ease: 'power2.inOut' },
          anticipatePin: 1,
          invalidateOnRefresh: true
        }
      });

      stages.forEach((_, i) => {
        tl.addLabel(`s${i}`);
        // 每个阶段内部：图从「处理前」走到「处理后」
        tl.fromTo(`[data-visual="${i}"] [data-before]`, { opacity: 1 }, { opacity: 0, duration: 0.6 }, `s${i}+=0.15`);
        tl.fromTo(`[data-visual="${i}"] [data-after]`, { opacity: 0 }, { opacity: 1, duration: 0.6 }, `s${i}+=0.15`);
        tl.fromTo(`[data-visual="${i}"] [data-progress]`, { attr: { width: 0 } }, { attr: { width: 560 }, duration: 0.75 }, `s${i}`);
        if (i < stages.length - 1) {
          tl.to(panels[i], { autoAlpha: 0, y: -40, duration: 0.25, ease: 'power2.in' }, `s${i}+=0.95`);
          tl.to(visuals[i], { autoAlpha: 0, duration: 0.25 }, `s${i}+=0.95`);
          tl.to(panels[i + 1], { autoAlpha: 1, y: 0, duration: 0.3, ease: 'power2.out' }, `s${i}+=1.1`);
          tl.to(visuals[i + 1], { autoAlpha: 1, duration: 0.3 }, `s${i}+=1.1`);
        } else {
          tl.to({}, { duration: 0.4 });
        }
      });
      tl.addLabel('end');
    },
    { scope: root }
  );

  return (
    <section id="process" ref={root} className="relative">
      <div ref={pin} className="min-h-[100dvh] flex items-center">
        <div className="mx-auto w-full max-w-[1280px] px-5 sm:px-8 py-16 grid gap-10 lg:grid-cols-12 lg:gap-8 items-center">
          <div className="lg:col-span-5 relative min-h-[380px]">
            <Eyebrow>四道工序 · 顺序固定，不可换</Eyebrow>
            <div className="relative mt-8">
              {stages.map((s, i) => (
                <div key={s.key} data-stage={i} className={i === 0 ? 'relative' : 'absolute inset-0'}>
                  <div className="flex items-baseline gap-4">
                    <span className="display text-[64px] leading-none text-c300/80 tabular-nums">{s.index}</span>
                    <span className="display text-[26px] text-ink-3 tracking-[0.12em]">{s.en}</span>
                  </div>
                  <h3 className="mt-4 font-serif text-[clamp(2rem,3.6vw,3rem)] font-semibold leading-tight text-ink">
                    {s.zh}
                    <span className="text-ink-4 font-normal"> · </span>
                    <span className="glaze-text">{s.verb}</span>
                  </h3>
                  <p className="mt-5 max-w-md text-[16px] leading-[1.85] text-ink-2">{s.lead}</p>
                  <ul className="mt-6 space-y-2.5">
                    {s.facts.map(f => (
                      <li key={f} className="flex items-start gap-3 text-[14px] text-ink-3">
                        <span className="mt-[9px] size-1.5 shrink-0 rounded-full bg-c500" />
                        {f}
                      </li>
                    ))}
                  </ul>
                </div>
              ))}
            </div>
            <div className="mt-10 flex items-center gap-2 text-[11px] uppercase tracking-[0.22em] text-ink-4">
              {stages.map(s => (
                <span key={s.key} className="flex items-center gap-2">
                  {s.en}
                  <span className="last:hidden text-c400">→</span>
                </span>
              ))}
            </div>
          </div>

          <div className="lg:col-span-7">
            <div className="bezel">
              <div className="bezel-core relative aspect-[16/11] overflow-hidden">
                <StageVisuals />
              </div>
            </div>
            <p className="mt-4 text-center text-[12px] text-ink-4">示意图：左半为处理前，右半为处理后；滚动推进工序。</p>
          </div>
        </div>
      </div>
    </section>
  );
}

/* ---------------- 四张示意图 ---------------- */

const W = 640;
const H = 440;

function StageVisuals() {
  return (
    <svg viewBox={`0 0 ${W} ${H}`} className="absolute inset-0 h-full w-full" aria-hidden="true">
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
      <g data-visual="0"><BalanceVisual /></g>
      <g data-visual="1"><ClayVisual /></g>
      <g data-visual="2"><ColorVisual /></g>
      <g data-visual="3"><LimitVisual /></g>
    </svg>
  );
}

function Frame({ title, note }: { title: string; note: string }) {
  return (
    <>
      <text x="40" y="44" style={{ fontFamily: 'var(--font-mono)', fontSize: 12, letterSpacing: '0.18em', fill: '#637780' }}>{title}</text>
      <text x={W - 40} y="44" textAnchor="end" style={{ fontFamily: 'var(--font-sans)', fontSize: 12, fill: '#8A9CA3' }}>{note}</text>
      <rect x="40" y="56" width="560" height="2" fill="#E6EEF1" />
      <rect data-progress x="40" y="56" width="0" height="2" fill="#1799AB" />
    </>
  );
}

function BalanceVisual() {
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
      <Frame title="BALANCE · 27 BANDS" note="虚线 = 这首歌自己的平滑轮廓" />
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
      <text x="60" y="412" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#8A9CA3' }}>40 Hz</text>
      <text x="580" y="412" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#8A9CA3' }}>16 kHz</text>
    </>
  );
}

function ClayVisual() {
  // 立体声宽度随频率：上下包络。处理前低频很宽、次声有能量；处理后 120 Hz 以下收成单声道、次声切掉、2 kHz 以上略宽。
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
      <Frame title="CLAY · STEREO WIDTH vs FREQUENCY" note="中线 = 单声道（M），带宽 = 侧声道（S）" />
      <line x1="60" y1={mid} x2="580" y2={mid} stroke="#0A4A57" strokeWidth="1.2" opacity="0.7" />
      <g data-before>
        <path d={band(before)} fill="url(#pv-fill)" stroke="#33B3C4" strokeWidth="1" />
        <rect x="60" y={mid - 60} width="24" height="120" fill="#E8590C" opacity="0.18" />
        <text x="72" y={mid + 84} textAnchor="middle" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#C2410C' }}>次声</text>
      </g>
      <g data-after>
        <path d={band(after)} fill="url(#pv-fill)" stroke="#127F90" strokeWidth="1.2" />
        <line x1="143" y1={mid - 110} x2="143" y2={mid + 110} stroke="#6A4DFF" strokeDasharray="3 4" strokeWidth="1" />
        <text x="148" y={mid - 100} style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6A4DFF' }}>收单 120 Hz</text>
        <line x1="70" y1={mid - 110} x2="70" y2={mid + 110} stroke="#0E6473" strokeDasharray="3 4" strokeWidth="1" />
        <text x="75" y={mid + 105} style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>低切 25 Hz</text>
        <text x="500" y={mid - 70} style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#127F90' }}>加宽 +0.8 dB</text>
      </g>
      <text x="60" y="412" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#8A9CA3' }}>10 Hz</text>
      <text x="580" y="412" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#8A9CA3' }}>20 kHz</text>
    </>
  );
}

function wavePoints(seed: number, harmonics: number, tighten: number) {
  const rnd = seeded(seed);
  const pts: string[] = [];
  const n = 260;
  const env = (t: number) => 0.55 + 0.45 * Math.sin(t * Math.PI * 3) ** 2;
  for (let i = 0; i <= n; i++) {
    const t = i / n;
    const x = 60 + t * 520;
    let y = Math.sin(t * Math.PI * 2 * 14) * env(t);
    y += harmonics * 0.22 * Math.sin(t * Math.PI * 2 * 28 + 0.4) + harmonics * 0.1 * Math.sin(t * Math.PI * 2 * 42);
    y += (rnd() - 0.5) * 0.06;
    // tighten：把包络往 1 拉（Glue 收紧动态）
    const e = env(t);
    y = y * (1 - tighten + tighten * (0.85 / Math.max(0.3, e)));
    pts.push(`${x.toFixed(1)},${(236 - y * 120).toFixed(1)}`);
  }
  return pts.join(' ');
}

function ColorVisual() {
  return (
    <>
      <Frame title="COLOR · TONE → GLUE → HEAT" note="包络收紧一点，多几分谐波，低频主体不动" />
      <line x1="60" y1="236" x2="580" y2="236" stroke="#DCE6E9" />
      <g data-before>
        <polyline points={wavePoints(11, 0, 0)} fill="none" stroke="#A3E0E8" strokeWidth="1.4" />
      </g>
      <g data-after>
        <polyline points={wavePoints(11, 1, 0.35)} fill="none" stroke="url(#pv-wave)" strokeWidth="1.6" />
        <g transform="translate(60 352)">
          <text x="0" y="0" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#637780' }}>GLUE GR</text>
          <rect x="70" y="-9" width="200" height="8" rx="4" fill="#E6EEF1" />
          <rect x="70" y="-9" width="46" height="8" rx="4" fill="#1799AB" />
          <text x="280" y="0" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>−2.1 dB</text>
        </g>
        <g transform="translate(60 380)">
          <text x="0" y="0" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#637780' }}>HEAT</text>
          <rect x="70" y="-9" width="200" height="8" rx="4" fill="#E6EEF1" />
          <rect x="70" y="-9" width="86" height="8" rx="4" fill="#6A4DFF" opacity="0.8" />
          <text x="280" y="0" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#6A4DFF' }}>43 · 自动</text>
        </g>
      </g>
    </>
  );
}

function LimitVisual() {
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
      <Frame title="LIMIT · TRUE PEAK, 8× DETECT / 1× APPLY" note="虚线 = Ceiling −1.0 dBTP" />
      <line x1="60" y1={ceiling} x2="580" y2={ceiling} stroke="#E8590C" strokeDasharray="5 5" strokeWidth="1.2" />
      <line x1="60" y1={mid * 2 - ceiling} x2="580" y2={mid * 2 - ceiling} stroke="#E8590C" strokeDasharray="5 5" strokeWidth="1.2" />
      <g data-before>
        {bars.map((b, i) => (
          <line key={i} x1={b.x} y1={mid - b.before} x2={b.x} y2={mid + b.before} stroke="#A3E0E8" strokeWidth="2.4" strokeLinecap="round" />
        ))}
        <text x="580" y={ceiling - 8} textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#8A9CA3' }}>−8.6 LUFS</text>
      </g>
      <g data-after>
        {bars.map((b, i) => (
          <line key={i} x1={b.x} y1={mid - b.after} x2={b.x} y2={mid + b.after} stroke={b.after < b.before * 1.55 - 0.5 ? '#0A4A57' : '#1799AB'} strokeWidth="2.4" strokeLinecap="round" />
        ))}
        <text x="580" y={ceiling - 8} textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>−7.9 LUFS · GR 2.6 dB</text>
      </g>
      <text x="60" y="412" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#8A9CA3' }}>Gain +3.8 dB → 前瞻 2 ms → 8× 插值峰 → 增益 ≤ 1 → 硬钳位保险</text>
    </>
  );
}
