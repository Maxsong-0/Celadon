import { useId, useRef, useState, type CSSProperties } from 'react';
import { Trans, useTranslation } from 'react-i18next';
import { useLocale } from '@/i18n/locale';
import { Em, SectionHead } from './Eyebrow';
import { gsap, useGSAP } from '@/lib/gsap';
import { useReveal } from '@/lib/reveal';
import { cn, prefersReducedMotion } from '@/lib/utils';

const BASE = import.meta.env.BASE_URL;
const src = (name: string, w: number) => `${BASE}showcase/${name}-${w}.webp`;

// 热点位置：插件界面图上的百分比坐标（图为 1200 × 820 逻辑像素的 2 倍渲染）。标题与说明在 home 命名空间 showcase.spots（同一顺序）。
const spots = [
  { x: 60, y: 20 },
  { x: 56.5, y: 43 },
  { x: 61.2, y: 65.7 },
  { x: 16, y: 70.5 }
] as const;

/** 叁 · 真界面：插件真实渲染的截图（非效果图）+ 四处注释 + GLAZE 0 ↔ 90 对比滑条。 */
export default function Showcase() {
  const root = useRef<HTMLElement>(null);
  const { t } = useTranslation('home');
  const texts = t('showcase.spots', { returnObjects: true });
  // 截图默认跟网站语言走（英文页先看英文界面），可以再切
  const [lang, setLang] = useState<'zh' | 'en'>(useLocale());
  const [active, setActive] = useState<number | null>(null);
  useReveal(root);

  return (
    <section id="interface" ref={root} className="section relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead
          chapter={{ n: '叁', label: t('chapters.showcase.label'), en: t('chapters.showcase.en') }}
          title={<Trans t={t} i18nKey="showcase.title" components={{ em: <Em /> }} />}
          lead={t('showcase.lead')}
        />

        <figure data-reveal className="mt-16">
          <div className="slab">
            <div className="slab-core relative overflow-hidden p-2 sm:p-3">
              <div className="relative">
                <img
                  src={src(`ui-${lang}`, 1200)}
                  srcSet={`${src(`ui-${lang}`, 1200)} 1200w, ${src(`ui-${lang}`, 2400)} 2400w`}
                  sizes="(min-width: 1280px) 1200px, 100vw"
                  width={2400}
                  height={1640}
                  loading="lazy"
                  decoding="async"
                  alt={lang === 'zh' ? t('showcase.altZh') : t('showcase.altEn')}
                  className="block h-auto w-full rounded-[1.1rem]"
                />
                {/* 热点：桌面端悬停 / 聚焦显示说明；手机端改看下方的编号列表 */}
                {spots.map((s, i) => (
                  <div key={i} className="absolute hidden md:block" style={{ left: `${s.x}%`, top: `${s.y}%` }}>
                    <button
                      type="button"
                      aria-label={`${i + 1}. ${texts[i].title}`}
                      aria-expanded={active === i}
                      onMouseEnter={() => setActive(i)}
                      onMouseLeave={() => setActive(v => (v === i ? null : v))}
                      onFocus={() => setActive(i)}
                      onBlur={() => setActive(v => (v === i ? null : v))}
                      onClick={() => setActive(v => (v === i ? null : i))}
                      className="relative -translate-x-1/2 -translate-y-1/2 grid size-8 place-items-center rounded-full bg-paper/90 font-mono text-[12px] text-c800 ring-1 ring-c700/40 shadow-[0_6px_18px_-8px_rgba(13,40,48,0.45)] backdrop-blur-sm transition-transform duration-500 ease-glaze hover:scale-110"
                    >
                      {i + 1}
                    </button>
                    <div
                      role="tooltip"
                      className={cn(
                        'pointer-events-none absolute top-5 z-10 w-64 rounded-2xl bg-panel/95 p-4 text-left ring-1 ring-ink/8 shadow-[0_18px_40px_-18px_rgba(13,40,48,0.35)] backdrop-blur transition-all duration-500 ease-glaze',
                        s.x > 50 ? 'right-6' : 'left-6',
                        active === i ? 'translate-y-0 opacity-100' : 'translate-y-1 opacity-0'
                      )}
                    >
                      <div className="font-serif text-[16px] font-semibold text-ink">{texts[i].title}</div>
                      <p className="mt-1.5 text-[13px] leading-relaxed text-ink-2">{texts[i].body}</p>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          </div>
          <figcaption className="mt-5 flex flex-wrap items-center justify-between gap-4 text-[13px] text-ink-4">
            <span>{t('showcase.caption')}</span>
            <div role="group" aria-label={t('showcase.langGroup')} className="inline-flex rounded-full bg-ink/5 p-1 ring-1 ring-ink/8">
              {(['zh', 'en'] as const).map(l => (
                <button
                  key={l}
                  type="button"
                  lang={l === 'zh' ? 'zh-CN' : 'en'}
                  aria-pressed={lang === l}
                  onClick={() => setLang(l)}
                  className={cn('rounded-full px-3.5 py-1 text-[12px] transition-colors duration-300', lang === l ? 'bg-panel text-ink shadow-sm' : 'text-ink-3 hover:text-ink')}
                >
                  {l === 'zh' ? '中文' : 'English'}
                </button>
              ))}
            </div>
          </figcaption>
        </figure>

        <ol className="mt-12 grid gap-x-10 gap-y-6 sm:grid-cols-2 lg:grid-cols-4">
          {texts.map((s, i) => (
            <li key={i} data-reveal className="border-t border-line pt-5">
              <div className="flex items-baseline gap-3">
                <span className="font-mono text-[12px] text-c700">{String(i + 1).padStart(2, '0')}</span>
                <h3 className="font-serif text-[18px] font-semibold text-ink">{s.title}</h3>
              </div>
              <p className="mt-2 text-[14px] leading-[1.8] text-ink-3">{s.body}</p>
            </li>
          ))}
        </ol>

        <GlazeCompare />
      </div>
    </section>
  );
}

/** GLAZE 0 % ↔ 90 % 对比：同一首歌、同一处，拖动分割线看开片、釉色与粒子怎么随 GLAZE 变。 */
function GlazeCompare() {
  const root = useRef<HTMLDivElement>(null);
  const { t } = useTranslation('home');
  const [pos, setPos] = useState(50);
  const id = useId();

  useGSAP(
    () => {
      // 进视口时分割线轻轻扫一下，提示可以拖；只一次，减少动效时不动。
      if (!root.current || prefersReducedMotion()) return;
      const o = { p: 50 };
      gsap.timeline({ scrollTrigger: { trigger: root.current, start: 'top 75%', once: true } })
        .to(o, { p: 68, duration: 0.9, ease: 'power2.inOut', onUpdate: () => setPos(Math.round(o.p)) })
        .to(o, { p: 50, duration: 0.9, ease: 'power2.inOut', onUpdate: () => setPos(Math.round(o.p)) });
    },
    { scope: root }
  );

  const img = (name: string, alt: string, className?: string, style?: CSSProperties) => (
    <img
      src={src(name, 1080)}
      srcSet={`${src(name, 1080)} 1080w, ${src(name, 2160)} 2160w`}
      sizes="(min-width: 1280px) 1200px, 100vw"
      width={2160}
      height={642}
      loading="lazy"
      decoding="async"
      alt={alt}
      className={cn('block h-auto w-full select-none', className)}
      style={style}
      draggable={false}
    />
  );

  return (
    <div ref={root} data-reveal className="mt-24">
      <div className="flex flex-wrap items-end justify-between gap-4">
        <div>
          <h3 className="font-serif text-[26px] font-semibold text-ink">
            GLAZE <span className="font-display tabular-nums">0 %</span> ↔ <span className="font-display tabular-nums">90 %</span>
          </h3>
          <p className="mt-2 measure text-[15px] leading-[1.85] text-ink-2">{t('showcase.compareBody')}</p>
        </div>
        <label htmlFor={id} className="text-[13px] text-ink-4">
          {t('showcase.dragHint')}
        </label>
      </div>
      <div className="relative mt-8 overflow-hidden rounded-[1.25rem] ring-1 ring-ink/8 has-[:focus-visible]:ring-2 has-[:focus-visible]:ring-c500">
        {img('glaze0', t('showcase.alt0'))}
        {img('glaze90', t('showcase.alt90'), 'absolute inset-0', { clipPath: `inset(0 0 0 ${pos}%)` })}
        {/* 分割线与把手（纯装饰；真正的控件是下面盖满整块的 range）。把手放在下沿，别压住旋钮中间的读数 */}
        <div className="pointer-events-none absolute inset-y-0 w-px bg-ink/50" style={{ left: `${pos}%` }} aria-hidden="true">
          <span className="absolute top-[86%] left-1/2 grid size-10 -translate-x-1/2 -translate-y-1/2 place-items-center rounded-full bg-paper ring-1 ring-ink/15 shadow-[0_8px_20px_-10px_rgba(13,40,48,0.5)]">
            <svg viewBox="0 0 20 20" className="size-4 text-ink-2" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round">
              <path d="M7 5 2 10l5 5M13 5l5 5-5 5" />
            </svg>
          </span>
        </div>
        <span className="pointer-events-none absolute left-4 top-4 rounded-full bg-paper/85 px-3 py-1 font-mono text-[11px] text-ink-3 ring-1 ring-ink/8">0 %</span>
        <span className="pointer-events-none absolute right-4 top-4 rounded-full bg-paper/85 px-3 py-1 font-mono text-[11px] text-c800 ring-1 ring-ink/8">90 %</span>
        <input
          id={id}
          type="range"
          min={0}
          max={100}
          value={pos}
          onChange={e => setPos(Number(e.target.value))}
          aria-label={t('showcase.rangeLabel')}
          aria-valuetext={t('showcase.rangeValue', { pct: 100 - pos })}
          className="absolute inset-0 h-full w-full cursor-ew-resize opacity-0"
        />
      </div>
    </div>
  );
}
