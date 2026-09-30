import { useRef, type ReactNode } from 'react';
import SpotlightCard from '@/components/react-bits/SpotlightCard';
import { SectionHead } from './Eyebrow';
import { gsap, useGSAP } from '@/lib/gsap';
import { cn } from '@/lib/utils';

interface Cell {
  title: string;
  en: string;
  body: string;
  span: string;
  art: ReactNode;
}

const cells: Cell[] = [
  {
    title: '等响 A / B',
    en: 'LOUDNESS-MATCHED A/B',
    body: '测整条链的实测响度差，把较响的一边衰减到另一边；只衰减、不放大，表仍显示衰减前的母带。判断「变好了还是只是变响了」。',
    span: 'md:col-span-7 md:row-span-2',
    art: <ArtAB />
  },
  {
    title: '试听差值',
    en: 'LISTEN TO DIFFERENCE',
    body: 'LIMIT 卡片上的耳机：只听被削掉的那部分。鼓点有没有变扁、低音有没有发闷，一听就知道。',
    span: 'md:col-span-5',
    art: <ArtDiff />
  },
  {
    title: '参考学习',
    en: 'REFERENCE FROM SIDECHAIN',
    body: '参考曲接进 DAW 的外部侧链，累计 10 秒有效声音，得到 27 段相对频谱目标；参考曲不进主输出。',
    span: 'md:col-span-5',
    art: <ArtRef />
  },
  {
    title: '逐位契约',
    en: 'BIT-EXACT CONTRACT',
    body: 'GLAZE 为 0、模块归零或旁通，输出与输入零差；任意块长、SIMD 与标量、空闲跳过与一直在跑——逐位相同。编译器不许融合乘加。',
    span: 'md:col-span-4',
    art: <ArtBits />
  },
  {
    title: '发行安全',
    en: 'RELEASE SAFETY',
    body: '归一化预听（−24 ～ −8 LUFS，只衰减）、编码余量（再降 0 ～ 1.5 dB 上限）、TPDF 抖动（16 / 24 位）。导出前该关的，界面一直提醒。',
    span: 'md:col-span-4',
    art: <ArtRelease />
  },
  {
    title: '双语 · 双平台',
    en: 'ZH / EN · MAC & WINDOWS',
    body: '中英文一键切换，设置跟人走。macOS 12+ 按芯片选包（Apple Silicon / Intel），Windows 10 / 11 x64 装包即用。',
    span: 'md:col-span-4',
    art: <ArtPlatform />
  }
];

export default function Bento() {
  const root = useRef<HTMLElement>(null);
  useGSAP(
    () => {
      if (!root.current) return;
      const q = gsap.utils.selector(root);
      gsap.from(q('[data-cell]'), {
        y: 60,
        opacity: 0,
        filter: 'blur(8px)',
        duration: 1.1,
        stagger: 0.09,
        ease: 'power3.out',
        scrollTrigger: { trigger: root.current, start: 'top 72%', once: true }
      });
    },
    { scope: root }
  );

  return (
    <section ref={root} className="relative py-28 sm:py-36">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead eyebrow="监听与承诺" title={<>让你能判断，<span className="glaze-text">而不是只能相信</span>。</>} lead="母带最难的不是处理，是判断。Celadon 把「听清楚差别」当成功能做：等响、差值、参考，以及一条写进测试里的逐位承诺。" />
        <div className="mt-16 grid gap-4 md:grid-cols-12 md:auto-rows-[minmax(190px,auto)]">
          {cells.map(c => (
            <div key={c.title} data-cell className={cn('bezel', c.span)}>
              <SpotlightCard className="bezel-core h-full flex flex-col">
                <div className="flex items-start justify-between gap-6">
                  <div>
                    <div className="text-[10px] uppercase tracking-[0.22em] text-ink-4">{c.en}</div>
                    <h3 className="mt-2 font-serif text-2xl font-semibold text-ink">{c.title}</h3>
                  </div>
                </div>
                <p className="mt-4 text-[14.5px] leading-[1.8] text-ink-2 max-w-md">{c.body}</p>
                <div className="mt-auto pt-6">{c.art}</div>
              </SpotlightCard>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

function ArtAB() {
  return (
    <svg viewBox="0 0 520 120" className="w-full h-auto" aria-hidden="true">
      <rect x="0" y="0" width="520" height="120" rx="14" fill="#F3F1EA" />
      {Array.from({ length: 44 }).map((_, i) => {
        const h = 18 + 40 * Math.abs(Math.sin(i * 0.7)) + (i % 5 === 0 ? 30 : 0);
        return <rect key={i} x={20 + i * 11} y={60 - h / 2} width="6" height={h} rx="3" fill={i < 22 ? '#A3E0E8' : '#1799AB'} />;
      })}
      <line x1="260" y1="12" x2="260" y2="108" stroke="#0A4A57" strokeDasharray="3 4" />
      <text x="24" y="108" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#637780' }}>B · 原声</text>
      <text x="496" y="108" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 10, fill: '#0E6473' }}>A · 处理后 · 已等响 4.8 dB</text>
    </svg>
  );
}
function ArtDiff() {
  return (
    <svg viewBox="0 0 320 70" className="w-full h-auto" aria-hidden="true">
      <rect width="320" height="70" rx="12" fill="#F3F1EA" />
      <line x1="16" y1="35" x2="304" y2="35" stroke="#DCE6E9" />
      {Array.from({ length: 40 }).map((_, i) => {
        const h = Math.max(0, 26 * Math.abs(Math.sin(i * 0.9)) - 14) * 2;
        return <rect key={i} x={16 + i * 7.2} y={35 - h / 2} width="3.5" height={Math.max(1, h)} rx="1.5" fill="#E8590C" opacity="0.8" />;
      })}
      <text x="16" y="62" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#C2410C' }}>只听被削掉的部分 · 不会比原声响</text>
    </svg>
  );
}
function ArtRef() {
  return (
    <svg viewBox="0 0 320 70" className="w-full h-auto" aria-hidden="true">
      <rect width="320" height="70" rx="12" fill="#F3F1EA" />
      {Array.from({ length: 27 }).map((_, i) => {
        const y = 40 - 18 * Math.sin((i / 26) * Math.PI) + (i % 4) * 2;
        return <circle key={i} cx={20 + i * 10.6} cy={y} r="2.2" fill="#6A4DFF" />;
      })}
      <path d={Array.from({ length: 27 }).map((_, i) => `${i ? 'L' : 'M'}${20 + i * 10.6} ${40 - 18 * Math.sin((i / 26) * Math.PI)}`).join(' ')} fill="none" stroke="#1799AB" strokeWidth="1.2" />
      <text x="16" y="62" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#637780' }}>侧链 · 已收集 10 / 10 s · 参考就绪</text>
    </svg>
  );
}
function ArtBits() {
  return (
    <div className="font-mono text-[11px] leading-relaxed text-ink-3">
      <div className="flex justify-between"><span>GLAZE 0 vs 参照链</span><span className="text-c700">Δ = 0 bit</span></div>
      <div className="flex justify-between"><span>SIMD vs 标量</span><span className="text-c700">Δ = 0 bit</span></div>
      <div className="flex justify-between"><span>块长 17 vs 2048</span><span className="text-c700">Δ = 0 bit</span></div>
      <div className="flex justify-between"><span>-ffp-contract</span><span className="text-c700">off · 守卫 #error</span></div>
    </div>
  );
}
function ArtRelease() {
  return (
    <div className="grid grid-cols-3 gap-2 text-center">
      {[['−14', 'LUFS 预听'], ['−1.0', 'dBTP 余量'], ['24', 'bit TPDF']].map(([v, l]) => (
        <div key={l} className="rounded-xl bg-paper px-2 py-3">
          <div className="display text-xl text-ink tabular-nums">{v}</div>
          <div className="mt-1 text-[10px] text-ink-4">{l}</div>
        </div>
      ))}
    </div>
  );
}
function ArtPlatform() {
  return (
    <div className="flex flex-wrap gap-2">
      {['VST3', 'AU', 'Standalone', 'arm64', 'x86_64', 'Win x64', '中 / EN'].map(t => (
        <span key={t} className="rounded-full bg-paper px-3 py-1 text-[11px] font-medium text-ink-2 ring-1 ring-ink/8">
          {t}
        </span>
      ))}
    </div>
  );
}
