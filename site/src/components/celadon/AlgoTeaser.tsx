import { useRef } from 'react';
import { LocaleLink } from '@/i18n/locale';
import { ArrowUpRight, Em, LinkArrow, SectionHead } from './Eyebrow';
import { useReveal } from '@/lib/reveal';

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
          return <rect key={i} x={20 + i * 36} y={100 - h} width="22" height={h} rx="3" fill={i === 1 ? '#0A4A57' : i === 2 || i === 3 ? '#1799AB' : '#A9C3B7'} opacity={i === 1 ? 0.9 : 0.85} />;
        })}
        <path d="M67 30 C 90 8, 120 12, 128 44" fill="none" stroke="#127F90" strokeWidth="1.3" strokeDasharray="3 3" />
        <path d="M67 30 C 100 -6, 160 0, 164 66" fill="none" stroke="#127F90" strokeWidth="1.1" strokeDasharray="3 3" opacity="0.6" />
        <text x="20" y="116" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#586C74' }}>Tame 压下的份额 → 上方邻段</text>
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
        <text x="16" y="108" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#586C74' }}>浅 = 矩形窗 · 深 = 两级滑动平均（拐角连续）</text>
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
        <text x="16" y="108" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#586C74' }}>浅 = 联动增益 · 深 = 分频后各自的增益</text>
      </svg>
    )
  }
];

export default function AlgoTeaser() {
  const root = useRef<HTMLElement>(null);
  useReveal(root);

  return (
    <section ref={root} className="section relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <div className="flex flex-col gap-8 lg:flex-row lg:items-end lg:justify-between">
          <SectionHead
            chapter={{ n: '陆', label: '公开算法', en: 'Open algorithms' }}
            title={
              <>
                算法写在纸上，<Em>式子和常数都给你</Em>。
              </>
            }
            lead="每道工序的结构、式子与常数都摊开写：哪些做了、哪些刻意没做、为什么，都有验证记录可查。"
          />
          <LinkArrow to="/algorithms" className="lg:mb-2 shrink-0">
            全部算法页
          </LinkArrow>
        </div>
        <ul className="rule-list mt-16">
          {items.map(it => (
            <li key={it.to} data-reveal>
              <LocaleLink to={it.to} className="group grid gap-6 py-10 no-underline md:grid-cols-[140px_minmax(0,1fr)_260px_24px] md:items-center md:gap-10">
                <div className="font-mono text-[12px] tracking-[0.18em] text-c700">{it.tag}</div>
                <div>
                  <h3 className="font-serif text-[26px] font-semibold leading-snug text-ink transition-colors duration-300 group-hover:text-c700">{it.title}</h3>
                  <p className="mt-3 measure text-[15px] leading-[1.85] text-ink-2">{it.body}</p>
                </div>
                <div className="max-w-[320px]">{it.art}</div>
                <ArrowUpRight className="hidden md:block size-5 text-ink-4 transition-transform duration-500 ease-glaze group-hover:translate-x-0.5 group-hover:-translate-y-0.5 group-hover:text-c700" />
              </LocaleLink>
            </li>
          ))}
        </ul>
      </div>
    </section>
  );
}
