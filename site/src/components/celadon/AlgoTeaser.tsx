import { useRef, type ReactNode } from 'react';
import { Trans, useTranslation } from 'react-i18next';
import { LocaleLink } from '@/i18n/locale';
import { ArrowUpRight, Em, LinkArrow, SectionHead } from './Eyebrow';
import { WrappedText } from './SvgText';
import { ALGO_CAPTION, captionMaxWidth } from '@/lib/figureBudget';
import { useReveal } from '@/lib/reveal';

// 标题、正文、图注在 home 命名空间 algorithms.items（同一顺序）；这里是链接、标签与示意图。
type Item = { to: string; tag: string; art: (caption: string) => ReactNode };

// 图注：9px 等宽、贴着图底；英文常常一行放不下，向上折行（WrappedText）。位置与每张图留的行数在 lib/figureBudget.ts（构建时按它核对文案，
// 折出更多行开发期也会告警）。
const CAPTION_STYLE = { fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#586C74' } as const;
const CAPTION_LINE = 12;

const items: Item[] = [
  {
    to: '/algorithms/balance',
    tag: 'BALANCE',
    art: caption => (
      <svg viewBox="0 0 280 120" className="w-full" aria-hidden="true">
        {[0, 1, 2, 3, 4, 5, 6].map(i => {
          const h = [40, 78, 52, 30, 44, 36, 28][i];
          return <rect key={i} x={20 + i * 36} y={100 - h} width="22" height={h} rx="3" fill={i === 1 ? '#0A4A57' : i === 2 || i === 3 ? '#1799AB' : '#A9C3B7'} opacity={i === 1 ? 0.9 : 0.85} />;
        })}
        <path d="M67 30 C 90 8, 120 12, 128 44" fill="none" stroke="#127F90" strokeWidth="1.3" strokeDasharray="3 3" />
        <path d="M67 30 C 100 -6, 160 0, 164 66" fill="none" stroke="#127F90" strokeWidth="1.1" strokeDasharray="3 3" opacity="0.6" />
        <WrappedText text={caption} x={ALGO_CAPTION.balance.x} y={116} maxWidth={captionMaxWidth(ALGO_CAPTION.balance.x)} lineHeight={CAPTION_LINE} maxLines={ALGO_CAPTION.balance.maxLines} grow="up" style={CAPTION_STYLE} />
      </svg>
    )
  },
  {
    to: '/algorithms/limit',
    tag: 'LIMIT',
    art: caption => (
      <svg viewBox="0 0 280 120" className="w-full" aria-hidden="true">
        <line x1="16" y1="24" x2="264" y2="24" stroke="#DCE6E9" />
        <path d="M16 24 L80 24 L120 78 L160 78 L200 30 L264 24" fill="none" stroke="#A3E0E8" strokeWidth="1.6" />
        <path d="M16 24 L76 24 C 96 28, 104 74, 122 78 L160 78 C 176 76, 186 40, 204 28 L264 24" fill="none" stroke="#127F90" strokeWidth="1.8" />
        <WrappedText text={caption} x={ALGO_CAPTION.limit.x} y={110} maxWidth={captionMaxWidth(ALGO_CAPTION.limit.x)} lineHeight={CAPTION_LINE} maxLines={ALGO_CAPTION.limit.maxLines} grow="up" style={CAPTION_STYLE} />
      </svg>
    )
  },
  {
    to: '/algorithms/color',
    tag: 'COLOR',
    art: caption => (
      <svg viewBox="0 0 280 120" className="w-full" aria-hidden="true">
        <line x1="16" y1="60" x2="264" y2="60" stroke="#DCE6E9" />
        <path d="M16 60 C 60 60, 80 84, 120 84 C 160 84, 180 60, 264 60" fill="none" stroke="#A3E0E8" strokeWidth="1.6" />
        <path d="M16 60 C 40 60, 56 70, 80 70 C 110 70, 130 84, 160 84 C 200 84, 220 62, 264 62" fill="none" stroke="#127F90" strokeWidth="1.8" />
        <text x="20" y="46" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#0E6473' }}>LOW ≤ 180 Hz</text>
        <text x="200" y="46" style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fill: '#0E6473' }}>HIGH ≥ 5 kHz</text>
        <WrappedText text={caption} x={ALGO_CAPTION.color.x} y={110} maxWidth={captionMaxWidth(ALGO_CAPTION.color.x)} lineHeight={CAPTION_LINE} maxLines={ALGO_CAPTION.color.maxLines} grow="up" style={CAPTION_STYLE} />
      </svg>
    )
  }
];

export default function AlgoTeaser() {
  const root = useRef<HTMLElement>(null);
  const { t } = useTranslation('home');
  const texts = t('algorithms.items', { returnObjects: true });
  useReveal(root);

  return (
    <section ref={root} className="section relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <div className="flex flex-col gap-8 lg:flex-row lg:items-end lg:justify-between">
          <SectionHead
            chapter={{ n: '陆', label: t('chapters.algorithms.label'), en: t('chapters.algorithms.en') }}
            title={<Trans t={t} i18nKey="algorithms.title" components={{ em: <Em /> }} />}
            lead={t('algorithms.lead')}
          />
          <LinkArrow to="/algorithms" className="lg:mb-2 shrink-0">
            {t('algorithms.allLink')}
          </LinkArrow>
        </div>
        <ul className="rule-list mt-16">
          {items.map((it, i) => (
            <li key={it.to} data-reveal>
              <LocaleLink to={it.to} className="group grid gap-6 py-10 no-underline md:grid-cols-[140px_minmax(0,1fr)_260px_24px] md:items-center md:gap-10">
                <div className="font-mono text-[12px] tracking-[0.18em] text-c700">{it.tag}</div>
                <div>
                  <h3 className="font-serif text-[26px] font-semibold leading-snug text-ink transition-colors duration-300 group-hover:text-c700">{texts[i].title}</h3>
                  <p className="mt-3 measure text-[15px] leading-[1.85] text-ink-2">{texts[i].body}</p>
                </div>
                <div className="max-w-[320px]">{it.art(texts[i].art)}</div>
                <ArrowUpRight className="hidden md:block size-5 text-ink-4 transition-transform duration-500 ease-glaze group-hover:translate-x-0.5 group-hover:-translate-y-0.5 group-hover:text-c700" />
              </LocaleLink>
            </li>
          ))}
        </ul>
      </div>
    </section>
  );
}
