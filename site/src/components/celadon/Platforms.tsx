import { useRef } from 'react';
import { Trans, useTranslation } from 'react-i18next';
import Magnet from '@/components/react-bits/Magnet';
import { Em, LinkArrow, SectionHead } from './Eyebrow';
import DownloadLink, { DownloadButtonFace } from './DownloadLink';
import { useReveal } from '@/lib/reveal';

// 平台名之外的文字（系统、格式、架构、首次打开）在 home 命名空间 get.platforms（同一顺序）。
const platforms = ['macOS', 'Windows'] as const;

/** 捌 · 获取：两栏纯排版 + 一个主按钮。页面最后一节，底部多留一点。 */
export default function Platforms() {
  const root = useRef<HTMLElement>(null);
  const { t } = useTranslation('home');
  const texts = t('get.platforms', { returnObjects: true });
  useReveal(root);

  return (
    <section id="get" ref={root} className="section-last relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead
          align="center"
          chapter={{ n: '捌', label: t('chapters.get.label'), en: t('chapters.get.en') }}
          title={<Trans t={t} i18nKey="get.title" components={{ em: <Em /> }} />}
          lead={t('get.lead')}
        />

        <div className="mx-auto mt-16 grid max-w-5xl gap-x-16 md:grid-cols-2">
          {platforms.map((name, i) => (
            <div key={name} data-reveal className="border-t border-line py-8">
              <div className="flex items-baseline justify-between gap-4">
                <h3 className="display text-[40px] leading-none text-ink">{name}</h3>
                <span className="text-[13px] text-ink-3">{texts[i].formats}</span>
              </div>
              <dl className="mt-6 space-y-3 text-[15px]">
                <div className="grid grid-cols-[5.5rem_1fr] gap-4">
                  <dt className="text-[12px] tracking-[0.2em] text-ink-4 pt-0.5">{t('get.system')}</dt>
                  <dd className="text-ink-2">{texts[i].req}</dd>
                </div>
                <div className="grid grid-cols-[5.5rem_1fr] gap-4">
                  <dt className="text-[12px] tracking-[0.2em] text-ink-4 pt-0.5">{t('get.arch')}</dt>
                  <dd className="text-ink-2">{texts[i].arch}</dd>
                </div>
                <div className="grid grid-cols-[5.5rem_1fr] gap-4">
                  <dt className="text-[12px] tracking-[0.2em] text-ink-4 pt-0.5">{t('get.firstRun')}</dt>
                  <dd className="text-ink-3 leading-relaxed">{texts[i].note}</dd>
                </div>
              </dl>
            </div>
          ))}
        </div>

        <div data-reveal className="mt-14 flex flex-wrap items-center justify-center gap-x-8 gap-y-5">
          <Magnet padding={40} magnetStrength={14}>
            <DownloadLink className="group btn-island-primary no-underline">{pkg => <DownloadButtonFace pkg={pkg} />}</DownloadLink>
          </Magnet>
          <LinkArrow to="/docs/install">{t('get.install')}</LinkArrow>
          <LinkArrow to="/changelog">{t('get.changelog')}</LinkArrow>
        </div>
      </div>
    </section>
  );
}
