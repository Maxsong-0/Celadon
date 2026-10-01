import { useEffect, useRef, useState, type MouseEvent, type ReactNode } from 'react';
import { useLocation, useNavigate, useSearchParams } from 'react-router-dom';
import { Chapter, Em, LinkArrow } from '@/components/celadon/Eyebrow';
import { DownloadGlyph, afterDownloadClick, startedPath } from '@/components/celadon/DownloadLink';
import { CHECKSUMS_FILE, RELEASE, downloadHref, type PackageId, type ReleasePackage } from '@/content/release';
import { ScrollTrigger } from '@/lib/gsap';
import { onFontsLoaded } from '@/lib/fonts';
import { scrollToAnchor } from '@/lib/anchor';
import type { Detection } from '@/lib/platform';
import { Trans, useTranslation } from 'react-i18next';
import { useLocale, useLocalePath } from '@/i18n/locale';
import { formatDate, formatNumber } from '@/lib/format';
import { useReveal } from '@/lib/reveal';
import { PLUGIN_VERSION } from '@/lib/site';
import { usePlatform } from '@/lib/usePlatform';
import { cn } from '@/lib/utils';

/*
  /download：三张平台卡 + 第一次打开的放行步骤 + 装好之后的注意事项。
  安装包信息只从 content/release.ts 读；它与插件版本（PLUGIN_VERSION）不一致时不给旧文件，只提示还没更新。
  按设计规格：认得出这台电脑用哪个包时，那一张是瓷板 + 主按钮，其余是素坯 + 文字链接（同一行最多一块瓷板）。
  识别见 lib/platform.ts：看不出 Mac 的芯片、ARM 版 Windows、系统不支持时，页首说明一句（三张卡照样能下）。
  点了下载（这里的卡片，或导航 / 首页的一键直下）→ ?started=<包>#first-launch：「第一次打开」一节顶上写「下载已开始」。
*/

interface Platform {
  /** 页内锚点：/download#mac-apple-silicon 等 */
  anchor: string;
  os: string;
}

// 芯片、系统、格式、「怎么认」在 download 命名空间 platforms.<包>。
const PLATFORMS: Record<PackageId, Platform> = {
  'mac-arm64': { anchor: 'mac-apple-silicon', os: 'Mac' },
  'mac-x64': { anchor: 'mac-intel', os: 'Mac' },
  'win-x64': { anchor: 'windows', os: 'Windows' }
};

export default function Download() {
  const root = useRef<HTMLElement>(null);
  const { t } = useTranslation('download');
  const locale = useLocale();
  useReveal(root);
  usePageSetup(t('pageTitle'));
  const detection = usePlatform();
  const navigate = useNavigate();
  const [params] = useSearchParams();
  // 安装包与插件版本对不上（改了 .jucer 还没出包）时，宁可不给，也不给错文件。
  const ready = RELEASE.version === PLUGIN_VERSION;
  const started = ready ? (RELEASE.packages.find(p => p.id === params.get('started')) ?? null) : null;
  const matched = detection.kind === 'match' ? detection.id : null;
  const localize = useLocalePath();
  const onDownload = (pkg: ReleasePackage) => navigate(localize(startedPath(pkg)), { replace: true });

  return (
    <main ref={root}>
      <section className="section pt-32 sm:pt-40">
        <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
          <header className="max-w-3xl">
            <div data-reveal>
              <Chapter n="取" label={t('chapter.label')} en={t('chapter.en')} />
            </div>
            <h1 data-reveal className="mt-8 font-serif text-[clamp(2rem,4.2vw,3.5rem)] font-medium leading-[1.12] tracking-tight text-ink">
              <Trans t={t} i18nKey="heading" components={{ em: <Em /> }} />
            </h1>
            <p data-reveal className="mt-6 measure text-[17px] leading-[1.85] text-ink-2">
              {ready ? t('intro', { version: RELEASE.version, date: formatDate(RELEASE.date, locale) }) : t('introStale', { version: PLUGIN_VERSION })}
            </p>
          </header>

          <DeviceNotice detection={detection} />

          {ready ? (
            <div className="mt-16 grid gap-5 xl:grid-cols-3">
              {RELEASE.packages.map(pkg => (
                <PackageCard
                  key={pkg.id}
                  pkg={pkg}
                  platform={PLATFORMS[pkg.id]}
                  badge={matched === pkg.id ? (pkg.id === 'win-x64' ? t('badgeComputer') : t('badgeMac')) : null}
                  onDownload={onDownload}
                />
              ))}
            </div>
          ) : (
            <StaleNotice />
          )}
        </div>
      </section>

      <section id="first-launch" className="section-last scroll-mt-16">
        <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
          {started ? <StartedNotice pkg={started} /> : null}
          <div className="max-w-3xl">
            <div data-reveal>
              <Kicker label={t('firstLaunch.kicker')} en={t('firstLaunch.kickerEn')} />
            </div>
            <h2 data-reveal className="mt-8 font-serif text-[clamp(1.75rem,3.4vw,2.75rem)] font-medium leading-[1.15] tracking-tight text-ink">
              <Trans t={t} i18nKey="firstLaunch.title" components={{ em: <Em /> }} />
            </h2>
            <p data-reveal className="mt-6 measure text-[17px] leading-[1.85] text-ink-2">
              {t('firstLaunch.body')}
            </p>
          </div>

          <div className="mt-16 grid gap-14 md:grid-cols-2 md:gap-16">
            <Steps title="Mac" list="macSteps" />
            <Steps title="Windows" list="winSteps" />
          </div>

          <div className="mt-24 sm:mt-28">
            <div data-reveal>
              <Kicker label={t('after.kicker')} en={t('after.kickerEn')} />
            </div>
            <ul className="rule-list mt-10">
              <AfterRow label={t('after.rescanLabel')}>{t('after.rescan')}</AfterRow>
              <AfterRow label={t('after.twiceLabel')}>
                <Trans t={t} i18nKey="after.twice" components={{ code: <Code /> }} />
              </AfterRow>
              {ready ? (
                <AfterRow label={t('after.verifyLabel')}>
                  <Trans
                    t={t}
                    i18nKey="after.verify"
                    values={{ file: CHECKSUMS_FILE }}
                    components={{
                      code: <Code />,
                      sums: (
                        <a
                          href={downloadHref(CHECKSUMS_FILE)}
                          download
                          className="font-mono text-[0.92em] text-c700 underline decoration-c300 decoration-1 underline-offset-4 transition-colors duration-300 ease-glaze hover:decoration-c600"
                        />
                      )
                    }}
                  />
                </AfterRow>
              ) : null}
              <AfterRow label={t('after.uninstallLabel')}>
                <Trans t={t} i18nKey="after.uninstall" components={{ code: <Code /> }} />
              </AfterRow>
            </ul>
          </div>

          <div data-reveal className="mt-14 flex flex-wrap gap-x-10 gap-y-4">
            <LinkArrow to="/docs/install">{t('installLink')}</LinkArrow>
            <LinkArrow to="/changelog">{t('changesLink')}</LinkArrow>
          </div>
        </div>
      </section>
    </main>
  );
}

function PackageCard({
  pkg,
  platform,
  badge,
  onDownload
}: {
  pkg: ReleasePackage;
  platform: Platform;
  badge: string | null;
  onDownload: (pkg: ReleasePackage) => void;
}) {
  const { t } = useTranslation('download');
  const chip = t(`platforms.${pkg.id}.chip`);
  const featured = badge !== null;
  const href = downloadHref(pkg.file);
  const onClick = (e: MouseEvent<HTMLAnchorElement>) => afterDownloadClick(e, () => onDownload(pkg));
  // 两种状态的 DOM 结构完全一样，只换类名：异步猜出芯片后切换不会重建节点（入场动画的状态留在原节点上）。
  // 素坯卡也留同样 6px 的外框（透明），三张卡的瓷面上下左右对齐，瓷板只多出一圈托。
  return (
    <article id={platform.anchor} data-reveal aria-labelledby={`${platform.anchor}-title`} className={cn('scroll-mt-28 p-1.5', featured && 'slab')}>
      <div className={cn('@container h-full', featured ? 'slab-core' : 'biscuit')}>
        <div className="flex h-full flex-col gap-8 p-7 sm:p-8 @2xl:grid @2xl:grid-cols-2 @2xl:gap-12">
          <div>
            <div className="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-2">
              <h2 id={`${platform.anchor}-title`} className="flex items-baseline gap-3">
                <span className="display text-[clamp(2rem,3vw,2.5rem)] leading-none text-ink">{platform.os}</span>{' '}
                <span className="font-serif text-[17px] text-ink-2">{chip}</span>
              </h2>
              {featured ? (
                <p className="flex items-center gap-2 text-[12px] tracking-[0.12em] text-c700">
                  <span className="size-1.5 rounded-full bg-c500" aria-hidden="true" />
                  {badge}
                </p>
              ) : null}
            </div>
            <p className="mt-4 text-[14px] leading-relaxed text-ink-3">{t(`platforms.${pkg.id}.system`)}</p>
            <p className="mt-1 text-[14px] leading-relaxed text-ink-3">{t(`platforms.${pkg.id}.formats`)}</p>
            <div className="mt-8 flex flex-wrap items-center gap-x-6 gap-y-4">
              {/* 三张卡的按钮字一样，读屏时补上平台；下载图标悬停时往下沉 2px（其余链接的 ↗ 是斜移） */}
              {featured ? (
                <a href={href} download onClick={onClick} className="group btn-island-primary no-underline">
                  {t('card.download')}
                  <span className="sr-only">{t('card.srPlatform', { os: platform.os, chip })}</span>
                  <span className="btn-island-icon bg-white/15 group-hover:translate-x-0 group-hover:translate-y-0.5">
                    <DownloadGlyph />
                  </span>
                </a>
              ) : (
                <a href={href} download onClick={onClick} className="link-arrow">
                  <span>
                    {t('card.download')}
                    <span className="sr-only">{t('card.srPlatform', { os: platform.os, chip })}</span>
                  </span>
                  <DownloadGlyph className="[.link-arrow:hover>&]:[transform:translateY(2px)]" />
                </a>
              )}
              <Size bytes={pkg.bytes} />
            </div>
          </div>

          <div className="flex flex-1 flex-col">
            <dl className="mb-6 border-t border-line pt-5 text-[13px]">
              <div className="grid grid-cols-[4.5rem_1fr] items-baseline gap-3">
                <dt className="text-[12px] tracking-[0.2em] text-ink-4">{t('card.file')}</dt>
                <dd className="font-mono text-[11.5px] leading-[1.7] text-ink-2" translate="no">
                  <FileName name={pkg.file} />
                </dd>
              </div>
              <div className="mt-4">
                <dt className="flex items-center justify-between gap-3">
                  <span className="text-[12px] tracking-[0.2em] text-ink-4">SHA-256</span>
                  <CopyButton value={pkg.sha256} what={t('card.copyWhat', { os: platform.os, chip })} />
                </dt>
                <dd className="mt-2">
                  <Hash value={pkg.sha256} />
                </dd>
              </div>
            </dl>
            <p className="mt-auto border-t border-line pt-5 text-[13px] leading-[1.75] text-ink-3">
              <span className="mr-2 text-[12px] tracking-[0.2em] text-ink-4">{t('card.how')}</span>
              {t(`platforms.${pkg.id}.how`)}
            </p>
          </div>
        </div>
      </div>
    </article>
  );
}

function StaleNotice() {
  const { t } = useTranslation('download');
  return (
    <div data-reveal className="biscuit mt-16 max-w-3xl p-8 sm:p-10">
      <p className="font-serif text-[clamp(1.375rem,2.4vw,1.75rem)] font-medium leading-snug text-ink">{t('stale.title', { version: PLUGIN_VERSION })}</p>
      <p className="mt-4 measure text-[15.5px] leading-[1.8] text-ink-2">{t('stale.body')}</p>
      <div className="mt-6">
        <LinkArrow to="/changelog">{t('stale.link')}</LinkArrow>
      </div>
    </div>
  );
}

/**
 * 页首一句话：认不准或用不了时说清楚为什么、该怎么选。认得准（match）或认不出（unknown）时不显示。
 * 识别可能在首帧之后才改判（异步问 CPU 架构），所以这块不进入场动画（data-reveal 只处理挂载时就在的元素）。
 */
function DeviceNotice({ detection }: { detection: Detection }) {
  const { t } = useTranslation('download');
  const locale = useLocale();
  let title: ReactNode;
  let body: ReactNode;
  switch (detection.kind) {
    case 'mac':
      title = t('notice.macTitle');
      body = <Trans t={t} i18nKey="notice.macBody" components={{ ui: <Ui /> }} />;
      break;
    case 'win-arm':
      title = t('notice.winArmTitle');
      body = t('notice.winArmBody');
      break;
    case 'unsupported':
      title = t(`unsupported.${detection.reason}`);
      body = `${t('requirements')}${locale === 'zh' ? '' : ' '}${t('notice.unsupportedTail')}`;
      break;
    default:
      return null;
  }
  return (
    <div role="note" className="biscuit mt-12 max-w-3xl p-7 sm:p-8">
      <p className="font-serif text-[clamp(1.25rem,2.2vw,1.5rem)] font-medium leading-snug text-ink">{title}</p>
      <p className="mt-3 measure text-[15.5px] leading-[1.8] text-ink-2">{body}</p>
    </div>
  );
}

/** 「第一次打开」一节顶上：刚点了下载（一键直下跳过来，或点了上面的卡片）。下载被拦或没开始时还能再点一次。 */
function StartedNotice({ pkg }: { pkg: ReleasePackage }) {
  const { t } = useTranslation(['download', 'common']);
  return (
    <div role="status" className="slab mb-16 max-w-3xl p-1.5 sm:mb-20">
      <div className="slab-core p-7 sm:p-8">
        <p className="flex items-center gap-2 text-[12px] tracking-[0.12em] text-c700">
          <span className="size-1.5 rounded-full bg-c500" aria-hidden="true" />
          {t('started.tag')}
        </p>
        <p className="mt-4 font-serif text-[clamp(1.25rem,2.2vw,1.5rem)] font-medium leading-snug text-ink">
          Celadon {RELEASE.version} · {t(`common:pkg.${pkg.id}`)}
        </p>
        <p className="mt-3 measure text-[15.5px] leading-[1.8] text-ink-2">
          <Trans
            t={t}
            i18nKey="started.body"
            values={{ file: pkg.file, size: (pkg.bytes / 1e6).toFixed(1) }}
            components={{
              code: <Code />,
              retry: (
                <a
                  href={downloadHref(pkg.file)}
                  download
                  className="text-c700 underline decoration-c300 decoration-1 underline-offset-4 transition-colors duration-300 ease-glaze hover:decoration-c600 [&:lang(zh)]:mx-1"
                />
              )
            }}
          />
        </p>
      </div>
    </div>
  );
}

/** 放行步骤：文字在 download 命名空间 macSteps / winSteps（数组），里面的 <ui>…</ui> 是界面上的字。 */
function Steps({ title, list }: { title: string; list: 'macSteps' | 'winSteps' }) {
  const { t } = useTranslation('download');
  const steps = t(list, { returnObjects: true });
  return (
    <div data-reveal>
      <h3 className="display text-[28px] leading-none text-ink">{title}</h3>
      <ol className="rule-list mt-6">
        {steps.map((step, i) => (
          <li key={i} className="grid grid-cols-[2.5rem_1fr] items-baseline gap-3 py-5">
            <span className="font-mono text-[12px] tracking-wider text-c700" aria-hidden="true">
              {String(i + 1).padStart(2, '0')}
            </span>
            <p className="text-[15.5px] leading-[1.8] text-ink-2">
              <Trans t={t} defaults={step} components={{ ui: <Ui /> }} />
            </p>
          </li>
        ))}
      </ol>
    </div>
  );
}

function AfterRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <li data-reveal className="grid gap-2 py-6 md:grid-cols-12 md:items-baseline md:gap-8">
      <p className="font-serif text-[17px] font-medium text-ink md:col-span-3">{label}</p>
      <p className="measure text-[15.5px] leading-[1.8] text-ink-2 md:col-span-9">{children}</p>
    </li>
  );
}

/** 章节标记去掉序号：细线 + 标签 + 英文斜体，给页内的小节用（英文页：标签大写、斜体为空不画，同 Chapter）。 */
function Kicker({ label, en }: { label: string; en: string }) {
  return (
    <p className="flex items-center gap-4 text-ink-3">
      <span className="h-px w-12 bg-ink/20" aria-hidden="true" />
      <span className="text-[13px] tracking-[0.2em] [&:lang(en)]:uppercase">{label}</span>
      {en ? (
        <span className="display text-[13px] italic text-ink-4" style={{ fontVariationSettings: '"opsz" 14, "SOFT" 50' }}>
          {en}
        </span>
      ) : null}
    </p>
  );
}

/** 界面上的字（按钮名、菜单路径）：加深，一眼能对上屏幕；中文再加「」，英文不加引号。 */
function Ui({ children }: { children?: ReactNode }) {
  return useLocale() === 'zh' ? <span className="font-medium text-ink">「{children}」</span> : <span className="font-medium text-ink">{children}</span>;
}

function Code({ children }: { children?: ReactNode }) {
  return (
    <code translate="no" className="rounded bg-c100/60 px-1.5 py-0.5 font-mono text-[0.86em] text-c800">
      {children}
    </code>
  );
}

/** 大小：1 MB = 10^6 字节（与 macOS 访达一致），一位小数；悬停看精确字节数。 */
function Size({ bytes }: { bytes: number }) {
  const { t } = useTranslation('download');
  const locale = useLocale();
  return (
    <span className="font-display text-[22px] leading-none tabular-nums text-ink" title={t('card.bytes', { bytes: formatNumber(bytes, locale) })}>
      {(bytes / 1e6).toFixed(1)}
      <span className="ml-1 text-[0.45em] tracking-[0.12em] text-ink-3">MB</span>
    </span>
  );
}

/** 文件名在连字符后允许换行，窄卡片里不从单词中间断开。 */
function FileName({ name }: { name: string }) {
  const parts = name.split('-');
  return (
    <>
      {parts.map((part, i) => (
        <span key={i}>
          {part}
          {i < parts.length - 1 ? (
            <>
              -<wbr />
            </>
          ) : null}
        </span>
      ))}
    </>
  );
}

const HEX8 = /[0-9a-f]{8}/g;

/**
 * 64 位十六进制按 8 位一组排，每组定宽 9ch → 一行 4 组、两行排完（窄处自动 3 组一行）。
 * 组与组之间没有真空格：点一下全选、手动复制出来仍是连续的一串。
 */
function Hash({ value }: { value: string }) {
  const { t } = useTranslation('download');
  const groups = value.match(HEX8) ?? [value];
  return (
    <code translate="no" title={t('card.selectAll')} className="block max-w-[36.5ch] select-all font-mono text-[11.5px] leading-[1.75] text-ink-2">
      {groups.map((g, i) => (
        <span key={i} className="inline-block w-[9ch]">
          {g}
        </span>
      ))}
    </code>
  );
}

function CopyButton({ value, what }: { value: string; what: string }) {
  const [state, setState] = useState<'idle' | 'done' | 'failed'>('idle');
  const { t } = useTranslation('download');
  const timer = useRef(0);
  useEffect(() => () => window.clearTimeout(timer.current), []);

  const onClick = async () => {
    const ok = await copyText(value);
    setState(ok ? 'done' : 'failed');
    window.clearTimeout(timer.current);
    timer.current = window.setTimeout(() => setState('idle'), 2000);
  };

  return (
    <>
      <button
        type="button"
        onClick={onClick}
        aria-label={t('copy.label', { what })}
        className={cn(
          'inline-flex items-center gap-1.5 rounded-full px-2 py-0.5 -mr-2 text-[12px] tracking-[0.12em] transition-colors duration-300 ease-glaze',
          state === 'done' ? 'text-c700' : state === 'failed' ? 'text-warn' : 'text-ink-3 hover:text-c700'
        )}
      >
        {state === 'done' ? <CheckGlyph /> : <CopyGlyph />}
        {state === 'done' ? t('copy.done') : state === 'failed' ? t('copy.failed') : t('copy.copy')}
      </button>
      <span className="sr-only" aria-live="polite">
        {state === 'done' ? t('copy.done') : state === 'failed' ? t('copy.failed') : ''}
      </span>
    </>
  );
}

/** 先用剪贴板接口；不在安全上下文（如内网 http 验收）或被拒绝时，退回选中文本框再复制。 */
async function copyText(text: string): Promise<boolean> {
  try {
    if (window.isSecureContext && navigator.clipboard) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // 落到下面的旧办法
  }
  const area = document.createElement('textarea');
  area.value = text;
  area.setAttribute('readonly', '');
  area.style.cssText = 'position:fixed;top:0;left:0;width:1px;height:1px;opacity:0;pointer-events:none';
  document.body.appendChild(area);
  area.select();
  let ok = false;
  try {
    ok = document.execCommand('copy');
  } catch {
    ok = false;
  }
  area.remove();
  return ok;
}

/** 页面标题、带 #锚点进来时滚到位置、字体到位后让滚动触发器重新量一遍位置。 */
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


function CopyGlyph() {
  return (
    <svg viewBox="0 0 16 16" className="size-3" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <rect x="5.5" y="5.5" width="8" height="8" rx="1.8" />
      <path d="M10.5 3.5v-.3A1.7 1.7 0 0 0 8.8 1.5H4.2a1.7 1.7 0 0 0-1.7 1.7v4.6a1.7 1.7 0 0 0 1.7 1.7h.3" />
    </svg>
  );
}

function CheckGlyph() {
  return (
    <svg viewBox="0 0 16 16" className="size-3" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="m3.5 8.5 3 3 6-7" />
    </svg>
  );
}
