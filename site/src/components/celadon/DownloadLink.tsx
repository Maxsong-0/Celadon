import type { MouseEvent, ReactNode } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { RELEASE, downloadHref, type ReleasePackage } from '@/content/release';
import { LocaleLink, useLocalePath } from '@/i18n/locale';
import { PLUGIN_VERSION } from '@/lib/site';
import { usePlatform } from '@/lib/usePlatform';
import { cn } from '@/lib/utils';
import { ArrowUpRight } from './Eyebrow';

/** 认得出这台电脑用哪个包、且安装包已经是当前版本时给那个包；否则 null（去下载页自己选）。 */
export function useDirectPackage(): ReleasePackage | null {
  const detection = usePlatform();
  if (detection.kind !== 'match' || RELEASE.version !== PLUGIN_VERSION) return null;
  return RELEASE.packages.find(p => p.id === detection.id) ?? null;
}

/**
 * 「下载」按钮（导航、首页）：认得出就直接下对应的安装包，同时跳到下载页的「第一次打开」——包没签名，第一次打开一定会被系统拦，
 * 这一步得让人看到；认不出（看不出 Mac 的芯片、系统不支持、安装包还没更新）就去下载页。
 */
export default function DownloadLink({ className, children }: { className?: string; children: (pkg: ReleasePackage | null) => ReactNode }) {
  const pkg = useDirectPackage();
  const navigate = useNavigate();
  const localize = useLocalePath();
  if (!pkg) {
    return (
      <LocaleLink to="/download" className={className}>
        {children(null)}
      </LocaleLink>
    );
  }
  return (
    <a href={downloadHref(pkg.file)} download className={className} onClick={e => afterDownloadClick(e, () => navigate(localize(startedPath(pkg))))}>
      {children(pkg)}
    </a>
  );
}

/** 下载页里「下载已开始」那一屏的地址（不带语言前缀，调用处按当前语言补）。 */
export function startedPath(pkg: ReleasePackage): string {
  return `/download?started=${pkg.id}#first-launch`;
}

/** 普通左键点下载：浏览器照常下载（不拦默认行为），再执行 then；带修饰键（新标签打开等）就只交给浏览器。 */
export function afterDownloadClick(e: MouseEvent<HTMLAnchorElement>, then: () => void) {
  if (e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
  then();
}

/** 首页两个大按钮（btn-island-primary）的字与图标：认得出时「Download for Mac · Apple silicon」+ 下载图标，认不出「Download 0.1.8.6」+ 斜箭头。 */
export function DownloadButtonFace({ pkg }: { pkg: ReleasePackage | null }) {
  const { t } = useTranslation();
  if (!pkg) {
    return (
      <>
        {t('downloadButton.version', { version: PLUGIN_VERSION })}
        <span className="btn-island-icon bg-white/15">
          <ArrowUpRight />
        </span>
      </>
    );
  }
  return (
    <>
      {t('downloadButton.direct', { platform: t(`pkg.${pkg.id}`) })}
      <span className="sr-only"> ({PLUGIN_VERSION})</span>
      {/* 下载图标悬停时往下沉，不走斜箭头的斜移 */}
      <span className="btn-island-icon bg-white/15 group-hover:translate-x-0 group-hover:translate-y-0.5">
        <DownloadGlyph />
      </span>
    </>
  );
}

export function DownloadGlyph({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 16 16" className={cn('size-3.5', className)} fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M8 2.5v8M4.5 7.5 8 11l3.5-3.5M3 13.5h10" />
    </svg>
  );
}
