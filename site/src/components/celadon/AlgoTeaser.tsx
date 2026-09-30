import { useRef } from 'react';
import { Link } from 'react-router-dom';
import { SectionHead } from './Eyebrow';
import { gsap, useGSAP } from '@/lib/gsap';

const items = [
  {
    to: '/algorithms/balance',
    tag: 'BALANCE',
    title: '上行掩蔽转移',
    body: 'Recover 与 Tame 同时打开时，被压下去的主导段把一部分削减「让」给更高、仍有内容、间隙不超过 16 dB 的邻段；A 计权把净增益拉回 0 附近，幅度封在 ±0.45 dB。',
    art: (
      <svg viewBox="0 0 280 120" className="w-full" aria-hidden="true">
        {[0, 1, 2, 3, 4, 5, 6].map(i => {
          const h = [40, 78, 52, 30, 44, 36, 28][i];
          return <rect key={i} x={20 + i * 36} y={100 - h} width="22" height={h} rx="3" fill={i === 1 ? '#0A4A57' : i === 2 || i === 3 ? '#6A4DFF' : '#A3E0E8'} opacity={i === 1 ? 0.9 : 0.85} />;
        })}
        <path d="M67 30 C 90 8, 120 12, 128 44" fill="none" stroke="#6A4DFF" strokeWidth="1.4" strokeDasharray="3 3" markerEnd="url(#arrow)" />
        <path d="M67 30 C 100 -6, 160 0, 164 66" fill="none" stroke="#6A4DFF" strokeWidth="1.2" strokeDasharray="3 3" />
        <text x="20" y="116" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#637780' }}>Tame 压下的份额 → 上方邻段</text>
      </svg>
    )
  },
  {
    to: '/algorithms/limit',
    tag: 'LIMIT',
    title: '三角攻击沿 · 短过载快释放',
    body: '矩形滑动平均换成两级滑动平均：攻击斜坡拐角连续，峰值那一刻增益同时到位，恒定增益仍精确回到 1。短于约 70 ms 的过载结束后，慢包络在半个窗口里不许低于快包络。',
    art: (
      <svg viewBox="0 0 280 120" className="w-full" aria-hidden="true">
        <line x1="16" y1="24" x2="264" y2="24" stroke="#DCE6E9" />
        <path d="M16 24 L80 24 L120 78 L160 78 L200 30 L264 24" fill="none" stroke="#A3E0E8" strokeWidth="1.6" />
        <path d="M16 24 L76 24 C 96 28, 104 74, 122 78 L160 78 C 176 76, 186 40, 204 28 L264 24" fill="none" stroke="#127F90" strokeWidth="1.8" />
        <text x="16" y="108" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#637780' }}>浅 = 矩形窗 · 深 = 两级滑动平均（拐角连续）</text>
      </svg>
    )
  },
  {
    to: '/algorithms/color',
    tag: 'COLOR',
    title: 'Glue 低、中、高分开施压',
    body: '表与统计仍是联动包络（100 Hz 高通侧链、2:1、6 dB 拐点）。低（≈180 Hz 以下）与高（≈5 kHz 以上）若自己更轻，增益最多比联动高 3.2 dB，也可再深 0.6 dB。人声一响，低音不再跟着躲。',
    art: (
      <svg viewBox="0 0 280 120" className="w-full" aria-hidden="true">
        <line x1="16" y1="60" x2="264" y2="60" stroke="#DCE6E9" />
        <path d="M16 60 C 60 60, 80 84, 120 84 C 160 84, 180 60, 264 60" fill="none" stroke="#A3E0E8" strokeWidth="1.6" />
        <path d="M16 60 C 40 60, 56 70, 80 70 C 110 70, 130 84, 160 84 C 200 84, 220 62, 264 62" fill="none" stroke="#127F90" strokeWidth="1.8" />
        <text x="20" y="46" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#0E6473' }}>LOW ≤ 180 Hz</text>
        <text x="200" y="46" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#0E6473' }}>HIGH ≥ 5 kHz</text>
        <text x="16" y="108" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#637780' }}>浅 = 联动增益 · 深 = 分频后各自的增益</text>
      </svg>
    )
  }
];

export default function AlgoTeaser() {
  const root = useRef<HTMLElement>(null);
  useGSAP(
    () => {
      if (!root.current) return;
      const q = gsap.utils.selector(root);
      gsap.from(q('[data-card]'), {
        y: 50,
        opacity: 0,
        rotate: (i: number) => (i - 1) * 1.2,
        duration: 1,
        stagger: 0.12,
        ease: 'power3.out',
        scrollTrigger: { trigger: root.current, start: 'top 70%', once: true }
      });
    },
    { scope: root }
  );
  return (
    <section ref={root} className="relative py-28 sm:py-36 bg-paper-2/60">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <div className="flex flex-col gap-8 lg:flex-row lg:items-end lg:justify-between">
          <SectionHead eyebrow="公开算法 · 不是黑盒" title={<>算法写在纸上，<span className="glaze-text">式子和常数都给你</span>。</>} lead="对照 Gullfoss Master、Invisible Limiter G2、The God Particle 的公开手册，Celadon 用自己的式子补缺口。哪些做了、哪些刻意没做，都写清楚。" />
          <Link to="/algorithms" className="group btn-island-ghost no-underline self-start lg:self-auto">
            全部算法页
            <span className="btn-island-icon bg-ink/6">
              <svg viewBox="0 0 16 16" className="size-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round"><path d="M4 12 12 4M6 4h6v6" /></svg>
            </span>
          </Link>
        </div>
        <div className="mt-16 grid gap-5 md:grid-cols-3">
          {items.map(it => (
            <Link key={it.to} to={it.to} data-card className="bezel group no-underline">
              <div className="bezel-core h-full p-7 flex flex-col transition-transform duration-700 ease-glaze group-hover:-translate-y-1">
                <div className="text-[10px] uppercase tracking-[0.22em] text-c600">{it.tag}</div>
                <h3 className="mt-2 font-serif text-[22px] font-semibold text-ink leading-snug">{it.title}</h3>
                <p className="mt-4 text-[14px] leading-[1.8] text-ink-2">{it.body}</p>
                <div className="mt-auto pt-6">{it.art}</div>
              </div>
            </Link>
          ))}
        </div>
      </div>
    </section>
  );
}
