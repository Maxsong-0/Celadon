import { Trans, useTranslation } from 'react-i18next';
import Mark from './Mark';
import Seal from './Seal';
import { LocaleLink } from '@/i18n/locale';
import { PLUGIN_VERSION } from '@/lib/site';

/** 页脚：落款（竖排「青瓷」+ 印章，全站唯一一枚；两种语言都保留，是品牌字）+ 三列链接。同一张纸，只用一根细线隔开，不换底色。 */
export default function Footer() {
  const { t } = useTranslation();
  return (
    <footer className="relative border-t border-line">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8 py-16 sm:py-20 grid gap-12 md:grid-cols-12">
        <div className="md:col-span-6 flex gap-6">
          <div className="vertical display text-[56px] leading-none text-c800/85 select-none" aria-hidden="true" lang="zh-CN">
            青瓷
          </div>
          <div>
            <div className="flex items-center gap-3">
              <Mark className="size-9" />
              <div>
                <div className="font-display text-xl tracking-tight text-ink">Celadon</div>
                <div className="font-mono text-[12px] tracking-[0.08em] text-ink-4">v{PLUGIN_VERSION}</div>
              </div>
            </div>
            <p className="mt-6 max-w-sm text-sm leading-relaxed text-ink-3">{t('footer.blurb')}</p>
            <Seal className="mt-8" />
          </div>
        </div>
        <FooterCol
          title={t('footer.docs')}
          links={[
            [t('footer.quickstart'), '/docs'],
            [t('footer.glaze'), '/docs/glaze'],
            [t('footer.listening'), '/docs/listening'],
            [t('footer.faq'), '/docs/faq']
          ]}
        />
        <FooterCol
          title={t('footer.algorithms')}
          links={[
            [t('footer.overview'), '/algorithms'],
            [t('footer.glazeAlgo'), '/algorithms/glaze'],
            [t('footer.limitAlgo'), '/algorithms/limit'],
            [t('footer.verification'), '/algorithms/verification']
          ]}
        />
        <FooterCol
          title={t('footer.get')}
          links={[
            [t('footer.download', { version: PLUGIN_VERSION }), '/download'],
            [t('footer.changelog'), '/changelog'],
            [t('footer.install'), '/docs/install']
          ]}
        />
      </div>
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8 pb-10 flex flex-col sm:flex-row gap-3 justify-between text-[12px] text-ink-4">
        <p>{t('footer.copyright')}</p>
        <p>
          <Trans
            i18nKey="footer.credits"
            components={{
              link: <a className="underline decoration-ink/20 hover:text-ink-2" href="https://reactbits.dev" target="_blank" rel="noreferrer" />
            }}
          />
        </p>
      </div>
    </footer>
  );
}

function FooterCol({ title, links }: { title: string; links: [string, string][] }) {
  return (
    <nav aria-label={title} className="md:col-span-2">
      <div className="text-[12px] tracking-[0.22em] text-ink-4 mb-4">{title}</div>
      <ul className="space-y-2.5">
        {links.map(([label, to]) => (
          <li key={to}>
            <LocaleLink to={to} className="text-sm text-ink-2 no-underline transition-colors duration-300 hover:text-c700">
              {label}
            </LocaleLink>
          </li>
        ))}
      </ul>
    </nav>
  );
}
