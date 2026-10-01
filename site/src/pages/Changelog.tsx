import { useEffect, useRef } from 'react';
import { useLocation } from 'react-router-dom';
import { Trans, useTranslation } from 'react-i18next';
import { useLocale } from '@/i18n/locale';
import { Chapter, Em, LinkArrow } from '@/components/celadon/Eyebrow';
import { CHANGELOG, type ReleaseNote } from '@/content/changelog';
import { ScrollTrigger } from '@/lib/gsap';
import { onFontsLoaded } from '@/lib/fonts';
import { scrollToAnchor } from '@/lib/anchor';
import { useReveal } from '@/lib/reveal';
import { PLUGIN_VERSION } from '@/lib/site';

/*
  /changelog：版本时间线，新的在上。左栏版本号（大字）+ 日期，桌面上随滚动钉住；右栏一句话 + 细线条目。
  内容只从 content/changelog.ts 读（每段文字中英两份，按当前语言取）；version 与 PLUGIN_VERSION 相同的那条标「当前版本」。
*/
export default function Changelog() {
  const root = useRef<HTMLElement>(null);
  const { t } = useTranslation('changelog');
  useReveal(root);
  usePageSetup(t('pageTitle'));
  const current = CHANGELOG.find(n => n.version === PLUGIN_VERSION);

  return (
    <main ref={root}>
      <section className="section-last pt-32 sm:pt-40">
        <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
          <header className="max-w-3xl">
            <div data-reveal>
              <Chapter n="记" label={t('chapter.label')} en={t('chapter.en')} />
            </div>
            <h1 data-reveal className="mt-8 font-serif text-[clamp(2rem,4.2vw,3.5rem)] font-medium leading-[1.12] tracking-tight text-ink">
              <Trans t={t} i18nKey="heading" components={{ em: <Em /> }} />
            </h1>
            <p data-reveal className="mt-6 measure text-[17px] leading-[1.85] text-ink-2">
              {t('lead')}
              {current ? null : <> {t('pending', { version: PLUGIN_VERSION })}</>}
            </p>
          </header>

          <ol className="mt-20 space-y-20 sm:mt-24 sm:space-y-28" aria-label={t('listLabel')}>
            {CHANGELOG.map(note => (
              <Entry key={note.id} note={note} current={note === current} />
            ))}
          </ol>
        </div>
      </section>
    </main>
  );
}

function Entry({ note, current }: { note: ReleaseNote; current: boolean }) {
  const { t } = useTranslation('changelog');
  const locale = useLocale();
  return (
    <li id={note.id} data-reveal className="grid scroll-mt-28 gap-7 md:grid-cols-12 md:gap-10">
      <div className="md:sticky md:top-28 md:col-span-4 md:self-start lg:col-span-3">
        <div className="flex items-baseline justify-between gap-4 md:block">
          <h2 className="display text-[clamp(2.5rem,4.6vw,3.75rem)] leading-none tabular-nums text-ink">{note.version ?? note.label?.[locale]}</h2>
          <p className="font-mono text-[12px] tracking-wider text-ink-3 md:mt-4">
            {/* 单日写成 <time>；合并条目写按语言的区间 */}
            {note.range ? note.range[locale] : <time dateTime={note.date}>{note.date}</time>}
          </p>
        </div>
        {current ? (
          <p className="mt-3 flex items-center gap-2 text-[13px] tracking-[0.12em] text-c700">
            <span className="size-1.5 rounded-full bg-c500" aria-hidden="true" />
            {t('current')}
          </p>
        ) : note.note ? (
          <p className="mt-3 text-[13px] tracking-[0.12em] text-ink-4">{note.note[locale]}</p>
        ) : null}
        {current ? (
          <div className="mt-6 hidden md:block">
            <LinkArrow to="/download">{t('download')}</LinkArrow>
          </div>
        ) : null}
      </div>

      <div className="md:col-span-8 lg:col-span-8 lg:col-start-5">
        <p className="font-serif text-[clamp(1.375rem,2.2vw,1.75rem)] font-medium leading-snug text-ink">{note.title[locale]}</p>
        <ul className="rule-list mt-8">
          {note.items.map((item, i) => (
            <li
              key={i}
              className="grid grid-cols-[3.25rem_1fr] items-baseline gap-4 py-5 sm:grid-cols-[4.5rem_1fr] sm:gap-6 [&:lang(en)]:grid-cols-[6.5rem_1fr] sm:[&:lang(en)]:grid-cols-[7.5rem_1fr]"
            >
              <span className="text-[12px] tracking-[0.2em] text-ink-4 [&:lang(en)]:text-[11px] [&:lang(en)]:uppercase [&:lang(en)]:tracking-[0.08em]">{t(`kinds.${item.kind}`)}</span>
              <p className="measure text-[15.5px] leading-[1.8] text-ink-2">{item.text[locale]}</p>
            </li>
          ))}
        </ul>
        {current ? (
          <div className="mt-8 md:hidden">
            <LinkArrow to="/download">{t('download')}</LinkArrow>
          </div>
        ) : null}
      </div>
    </li>
  );
}

/** 页面标题、带 #版本 进来时滚到那一版、字体到位后让滚动触发器重新量一遍位置。 */
function usePageSetup(title: string) {
  const { hash, key } = useLocation();

  useEffect(() => {
    const prev = document.title;
    document.title = title;
    return () => {
      document.title = prev;
    };
  }, [title]);

  // 平滑滚动会被字体到位后的 ScrollTrigger 重新测量打断（原来带锚点进来停在页首），由 lib/anchor 处理。
  useEffect(() => scrollToAnchor(hash), [hash, key]);

  // 字体换上后触发器位置会变，重新量（思源的声明是异步插入的，见 lib/fonts）。
  useEffect(() => onFontsLoaded(() => ScrollTrigger.refresh()), []);
}
