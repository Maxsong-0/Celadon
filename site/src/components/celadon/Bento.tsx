import { useRef, type ReactNode } from 'react';
import { Trans, useTranslation } from 'react-i18next';
import { Em, LinkArrow, SectionHead } from './Eyebrow';
import { useReveal } from '@/lib/reveal';

// 三行的标题、英文副题、正文在 home 命名空间 judgement.rows（同一顺序）；这里只配图。
const arts: ReactNode[] = [<ArtAB key="ab" />, <ArtDiff key="diff" />, <ArtRef key="ref" />];

/** 伍 · 判断：左三行手段，右两块承诺。 */
export default function Bento() {
  const root = useRef<HTMLElement>(null);
  const { t } = useTranslation('home');
  const rows = t('judgement.rows', { returnObjects: true });
  const bits = t('judgement.bitsEn');
  const release = t('judgement.releaseEn');
  useReveal(root);

  return (
    <section ref={root} className="section relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead
          chapter={{ n: '伍', label: t('chapters.judgement.label'), en: t('chapters.judgement.en') }}
          title={<Trans t={t} i18nKey="judgement.title" components={{ em: <Em /> }} />}
          lead={t('judgement.lead')}
        />

        <div className="mt-16 grid gap-12 lg:grid-cols-12 lg:gap-10">
          <ul className="rule-list lg:col-span-7">
            {rows.map((r, i) => (
              <li key={i} data-reveal className="grid gap-6 py-8 sm:grid-cols-[minmax(0,1fr)_200px] sm:items-center">
                <div>
                  <div className="flex items-baseline gap-3">
                    <span className="font-mono text-[12px] text-c700">0{i + 1}</span>
                    <h3 className="font-serif text-[24px] font-semibold text-ink">{r.title}</h3>
                    {r.en ? (
                      <span className="display text-[13px] italic text-ink-4 hidden sm:inline" style={{ fontVariationSettings: '"opsz" 14, "SOFT" 50' }}>
                        {r.en}
                      </span>
                    ) : null}
                  </div>
                  <p className="mt-3 measure text-[15px] leading-[1.85] text-ink-2">{r.body}</p>
                </div>
                <div className="text-ink-3">{arts[i]}</div>
              </li>
            ))}
            <li data-reveal className="py-6">
              <LinkArrow to="/docs/listening">{t('judgement.link')}</LinkArrow>
            </li>
          </ul>

          <div className="lg:col-span-5 grid content-start gap-5">
            <div data-reveal className="biscuit p-7 sm:p-8">
              {bits ? (
                <div className="display text-[13px] italic text-ink-4" style={{ fontVariationSettings: '"opsz" 14, "SOFT" 50' }}>
                  {bits}
                </div>
              ) : null}
              <h3 className="mt-1 font-serif text-[24px] font-semibold text-ink">{t('judgement.bitsTitle')}</h3>
              <p className="mt-3 text-[15px] leading-[1.85] text-ink-2">{t('judgement.bitsBody')}</p>
              <ArtBits />
            </div>
            <div data-reveal className="biscuit p-7 sm:p-8">
              {release ? (
                <div className="display text-[13px] italic text-ink-4" style={{ fontVariationSettings: '"opsz" 14, "SOFT" 50' }}>
                  {release}
                </div>
              ) : null}
              <h3 className="mt-1 font-serif text-[24px] font-semibold text-ink">{t('judgement.releaseTitle')}</h3>
              <p className="mt-3 text-[15px] leading-[1.85] text-ink-2">{t('judgement.releaseBody')}</p>
              <ArtRelease />
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}

/* ---------------- 行内小图：只用细线与单色，数据色（紫 / 橙）只表达它们的本义 ---------------- */

function ArtAB() {
  const { t } = useTranslation('home');
  return (
    <svg viewBox="0 0 200 72" className="w-full h-auto" aria-hidden="true">
      {Array.from({ length: 24 }, (_, i) => {
        const h = 10 + 26 * Math.abs(Math.sin(i * 0.7)) + (i % 5 === 0 ? 12 : 0);
        const after = i >= 12;
        // 右半 = 处理后，已衰减到与左半同响：峰被收住，但整体不比左边高
        const hh = after ? Math.min(h * 1.08, 40) : h;
        return <rect key={i} x={4 + i * 8.2} y={32 - hh / 2} width="3.6" height={hh} rx="1.8" fill={after ? '#127F90' : '#A9C3B7'} />;
      })}
      <line x1="100" y1="2" x2="100" y2="62" stroke="#0D1B21" strokeOpacity="0.35" strokeDasharray="2 3" />
      <text x="4" y="70" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#6E8189' }}>{t('judgement.artOriginal')}</text>
      <text x="196" y="70" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#0E6473' }}>{t('judgement.artMatched')}</text>
    </svg>
  );
}
function ArtDiff() {
  const { t } = useTranslation('home');
  return (
    <svg viewBox="0 0 200 72" className="w-full h-auto" aria-hidden="true">
      <line x1="4" y1="32" x2="196" y2="32" stroke="#DCE6E9" />
      {Array.from({ length: 27 }, (_, i) => {
        const h = Math.max(0, 22 * Math.abs(Math.sin(i * 0.9)) - 12) * 2;
        return <rect key={i} x={4 + i * 7.2} y={32 - Math.max(1, h) / 2} width="2.6" height={Math.max(1, h)} rx="1.3" fill="#E8590C" opacity="0.8" />;
      })}
      <text x="4" y="70" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#C2410C' }}>{t('judgement.artDiff')}</text>
    </svg>
  );
}
function ArtRef() {
  const { t } = useTranslation('home');
  const pts = Array.from({ length: 27 }, (_, i) => [6 + i * 7.1, 34 - 16 * Math.sin((i / 26) * Math.PI)] as const);
  return (
    <svg viewBox="0 0 200 72" className="w-full h-auto" aria-hidden="true">
      <path d={pts.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`).join(' ')} fill="none" stroke="#127F90" strokeWidth="1.2" />
      {pts.map(([x, y], i) => (
        <circle key={i} cx={x} cy={y + ((i % 4) - 1.5) * 1.6} r="1.7" fill="#0D1B21" opacity="0.45" />
      ))}
      <text x="4" y="70" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#6E8189' }}>{t('judgement.artRef')}</text>
    </svg>
  );
}
function ArtBits() {
  const { t } = useTranslation('home');
  return (
    <dl className="mt-6 space-y-1.5 border-t border-line pt-5 font-mono text-[12px] text-ink-3">
      {t('judgement.bitsRows', { returnObjects: true }).map(k => [k, 'Δ = 0 bit'] as const).map(([k, v]) => (
        <div key={k} className="flex justify-between gap-4">
          <dt>{k}</dt>
          <dd className="text-c700">{v}</dd>
        </div>
      ))}
    </dl>
  );
}
const RELEASE_VALUES = ['−14', '1.5', '24'] as const;

function ArtRelease() {
  const { t } = useTranslation('home');
  return (
    <dl className="mt-6 grid grid-cols-3 border-t border-line pt-5">
      {t('judgement.releaseStats', { returnObjects: true }).map((l, i) => [RELEASE_VALUES[i], l] as const).map(([v, l]) => (
        // dt 在前（合法的 dl 结构），视觉上数字在上
        <div key={l} className="flex flex-col-reverse">
          <dt className="mt-2 text-[11px] leading-snug text-ink-4">{l}</dt>
          <dd className="display text-[26px] leading-none text-ink tabular-nums">{v}</dd>
        </div>
      ))}
    </dl>
  );
}
