import { lazy, Suspense, useCallback, useEffect, useLayoutEffect, useRef, useState, type ComponentType } from 'react';
import { useLocation } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import type { SearchDialogProps } from '@/components/docs/Search';
import { gsap, useGSAP } from '@/lib/gsap';
import { tracks } from '@/content/listen';
import { cn, prefersReducedMotion } from '@/lib/utils';
import Mark from './Mark';
import DownloadLink, { DownloadGlyph } from './DownloadLink';
import { LocaleLink, useLocale, useLocalePath, useSwitchLocale } from '@/i18n/locale';
import { HTML_LANG, localizePath, stripLocale, type Locale } from '@/i18n/paths';

// 搜索弹层单独成包：第一次打开时才拉（指针移到搜索按钮上就先预取），首屏不背它。
// 包没拉到（断网）就当没打开：立刻关掉，别让页面卡在锁住滚动的状态。
const SearchDialog = lazy<ComponentType<SearchDialogProps>>(() =>
  import('@/components/docs/Search').catch(() => ({
    default: function SearchUnavailable({ onClose }: SearchDialogProps) {
      useEffect(onClose, [onClose]);
      return null;
    }
  }))
);
const preloadSearch = () => {
  import('@/components/docs/Search').catch(() => undefined);
};

// 「试听」一节只在有曲目时渲染（content/listen.json 为空时整节不显示），导航项跟着它：没有曲目就不放这条，免得点了原地不动。
const items = [
  { key: 'process', to: '/#process' },
  ...(tracks.length ? [{ key: 'listen', to: '/#listen' } as const] : []),
  { key: 'algorithms', to: '/algorithms' },
  { key: 'docs', to: '/docs' },
  { key: 'changelog', to: '/changelog' }
] as const;

/** 语言开关上写的是「另一种」语言，用那种语言自己的写法（中文用户在英文页认得出「中文」）。 */
const SWITCH: Record<Locale, { text: string; label: string }> = {
  en: { text: 'EN', label: 'Switch to English' },
  zh: { text: '中文', label: '切换到中文' }
};

const SITE_ORIGIN = 'https://celadonaudio.com';   // 与 scripts/site-data.mjs 的 ORIGIN 一致（check-i18n 核对）

/** Mac / iPad 上提示 ⌘K，其余（Windows / Linux）提示 Ctrl K；两种组合键在哪儿都认。 */
const IS_MAC =
  typeof navigator !== 'undefined' &&
  /mac|iphone|ipad|ipod/i.test((navigator as Navigator & { userAgentData?: { platform?: string } }).userAgentData?.platform || navigator.platform || navigator.userAgent);
const SHORTCUT = IS_MAC ? '⌘K' : 'Ctrl K';

/**
 * 每次换路径或语言：标题与摘要先恢复成本语言的站点默认（说明页等在自己的 effect 里再覆盖成本页的），
 * 规范地址改成当前路径，hreflang 指向同一页的两种语言（x-default = 英文）。
 * 导航排在各页面之前，它的 layout effect 先跑，页面自己的设置一定落在后面。
 * 这几条 <link> 由 index.html 里的小脚本在首屏按路径注入，这里只在站内跳转时跟着改。
 */
function useHeadSync(pathname: string) {
  const { t } = useTranslation();
  const title = t('site.title');
  const description = t('site.description');
  useLayoutEffect(() => {
    document.title = title;
    document.querySelector<HTMLMetaElement>('meta[name="description"]')?.setAttribute('content', description);
    const page = stripLocale(pathname).replace(/\/+$/, '') || '/';
    const url = (locale: Locale) => SITE_ORIGIN + localizePath(page, locale);
    headLink('canonical', null).href = SITE_ORIGIN + (pathname.replace(/\/+$/, '') || '/');
    headLink('alternate', 'x-default').href = url('en');
    for (const locale of ['en', 'zh'] as const) headLink('alternate', HTML_LANG[locale]).href = url(locale);
  }, [pathname, title, description]);
}

/** 找到（没有就建）一条 <link rel=… hreflang=…>。 */
function headLink(rel: string, hreflang: string | null): HTMLLinkElement {
  const selector = hreflang ? `link[rel="${rel}"][hreflang="${hreflang}"]` : `link[rel="${rel}"]:not([hreflang])`;
  let link = document.head.querySelector<HTMLLinkElement>(selector);
  if (!link) {
    link = document.createElement('link');
    link.rel = rel;
    if (hreflang) link.hreflang = hreflang;
    document.head.appendChild(link);
  }
  return link;
}

/** 手机菜单或搜索打开时锁住页面滚动；原本有滚动条的，锁住期间留着滚动条那一栏，页面不左右跳。 */
function useScrollLock(locked: boolean) {
  useLayoutEffect(() => {
    if (!locked) return;
    const html = document.documentElement;
    const hadBar = window.innerWidth > html.clientWidth;
    html.style.overflow = 'hidden';
    if (hadBar) html.style.setProperty('scrollbar-gutter', 'stable');
    return () => {
      html.style.overflow = '';
      html.style.removeProperty('scrollbar-gutter');
    };
  }, [locked]);
}

/** 浮在页面顶部的「岛」式导航：玻璃胶囊，滚动后缩紧；手机端汉堡变叉、整屏展开、链接逐条浮起。⌘K / Ctrl K 在任何页面打开全站搜索。 */
export default function Nav() {
  const [open, setOpen] = useState(false);
  const [scrolled, setScrolled] = useState(false);
  const [searchOpen, setSearchOpen] = useState(false);
  const [searchMounted, setSearchMounted] = useState(false);
  const overlay = useRef<HTMLDivElement>(null);
  const menuButton = useRef<HTMLButtonElement>(null);
  const returnFocus = useRef<HTMLElement | null>(null);
  const location = useLocation();
  const { t } = useTranslation();
  const locale = useLocale();
  const localize = useLocalePath();
  const switchLocale = useSwitchLocale();
  const other = SWITCH[locale === 'zh' ? 'en' : 'zh'];

  useHeadSync(location.pathname);
  useScrollLock(open || searchOpen);

  useEffect(() => {
    const onScroll = () => setScrolled(window.scrollY > 24);
    onScroll();
    window.addEventListener('scroll', onScroll, { passive: true });
    return () => window.removeEventListener('scroll', onScroll);
  }, []);

  useEffect(() => {
    setOpen(false);
    setSearchOpen(false);
  }, [location]);

  /** 打开搜索；from = 关掉后焦点要回去的地方（菜单里点的就回到汉堡按钮，菜单那时已经收起）。 */
  const openSearch = useCallback((from: HTMLElement | null) => {
    returnFocus.current = from && overlay.current?.contains(from) ? menuButton.current : from;
    setOpen(false);
    setSearchMounted(true);
    setSearchOpen(true);
  }, []);
  const closeSearch = useCallback(() => setSearchOpen(false), []);

  // ⌘K / Ctrl K 开关搜索（在任何页面、任何焦点下都生效）；Esc 收起手机菜单。
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.isComposing) return;
      if ((e.metaKey || e.ctrlKey) && !e.altKey && !e.shiftKey && (e.key.toLowerCase() === 'k' || e.code === 'KeyK')) {
        e.preventDefault();
        if (searchOpen) {
          setSearchOpen(false);
        } else {
          const cur = document.activeElement;
          openSearch(cur instanceof HTMLElement && cur !== document.body ? cur : null);
        }
      } else if (e.key === 'Escape' && open) {
        setOpen(false);
        menuButton.current?.focus();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, searchOpen, openSearch]);

  useGSAP(
    () => {
      const el = overlay.current;
      if (!el) return;
      const links = el.querySelectorAll('[data-link]');
      const reduce = prefersReducedMotion();
      if (open) {
        gsap.set(el, { visibility: 'visible' });
        if (reduce) {
          gsap.set(el, { opacity: 1 });
          gsap.set(links, { y: 0, opacity: 1 });
        } else {
          gsap.fromTo(el, { opacity: 0 }, { opacity: 1, duration: 0.5, ease: 'power2.out', overwrite: true });
          gsap.fromTo(links, { y: 48, opacity: 0 }, { y: 0, opacity: 1, stagger: 0.07, duration: 0.8, ease: 'power3.out', delay: 0.1, overwrite: true });
        }
      } else if (reduce) {
        gsap.set(el, { opacity: 0, visibility: 'hidden' });
      } else {
        gsap.to(el, { opacity: 0, duration: 0.35, ease: 'power2.out', overwrite: true, onComplete: () => gsap.set(el, { visibility: 'hidden' }) });
      }
    },
    { dependencies: [open], scope: overlay }
  );

  const isActive = (to: string) => (to.startsWith('/#') ? false : location.pathname.startsWith(localize(to)));

  return (
    <>
      <header className="fixed inset-x-0 top-0 z-40 flex justify-center px-4 pt-4 sm:pt-5 pointer-events-none">
        <nav
          aria-label={t('nav.label')}
          className={cn(
            'pointer-events-auto flex items-center gap-1 rounded-full bg-paper/70 pl-3 pr-1.5 py-1.5 ring-1 ring-ink/8 backdrop-blur-xl transition-all duration-700 ease-glaze',
            scrolled ? 'shadow-[0_10px_40px_-18px_rgba(13,40,48,0.35)]' : 'shadow-none'
          )}
        >
          <LocaleLink to="/" className="flex items-center gap-2.5 pr-3 no-underline" aria-label={t('nav.home')}>
            <Mark className="size-7" />
            <span className="font-display text-[15px] tracking-tight text-ink">Celadon</span>
            <span className="hidden sm:inline font-serif text-[13px] text-ink-3 tracking-[0.2em] pl-1" lang="zh-CN">
              青瓷
            </span>
          </LocaleLink>
          <ul className="hidden md:flex items-center gap-0.5">
            {items.map(it => (
              <li key={it.to}>
                <LocaleLink
                  to={it.to}
                  aria-current={isActive(it.to) ? 'page' : undefined}
                  className={cn(
                    'block rounded-full px-3.5 py-1.5 text-[13px] no-underline transition-colors duration-300 hover:bg-ink/5 hover:text-ink',
                    isActive(it.to) ? 'bg-ink text-paper hover:bg-ink hover:text-paper' : 'text-ink-2'
                  )}
                >
                  {t(`nav.${it.key}`)}
                </LocaleLink>
              </li>
            ))}
          </ul>
          {/* 语言开关：切到同一页的另一种语言，并记住选择（ADR-024） */}
          <button
            type="button"
            onClick={switchLocale}
            lang={locale === 'zh' ? HTML_LANG.en : HTML_LANG.zh}
            aria-label={other.label}
            className="hidden rounded-full px-3 py-1.5 text-[13px] text-ink-2 transition-colors duration-300 hover:bg-ink/5 hover:text-ink md:block"
          >
            {other.text}
          </button>
          {/* 桌面：放大镜 + 快捷键提示 */}
          <button
            type="button"
            onClick={e => openSearch(e.currentTarget)}
            onPointerEnter={preloadSearch}
            onFocus={preloadSearch}
            aria-label={t('nav.searchLabel')}
            aria-haspopup="dialog"
            aria-keyshortcuts="Meta+K Control+K"
            className="mr-1 hidden items-center gap-2 rounded-full py-1.5 pl-3 pr-1.5 text-ink-3 transition-colors duration-300 hover:bg-ink/5 hover:text-ink md:inline-flex"
          >
            <SearchGlyph className="size-[15px]" />
            <kbd className="rounded-md bg-paper/80 px-1.5 font-sans text-[11px] leading-5 tracking-wide text-ink-4 ring-1 ring-ink/10">{SHORTCUT}</kbd>
          </button>
          {/* 认得出这台电脑就直接下对应的安装包（见 DownloadLink），认不出去下载页 */}
          <DownloadLink className="hidden md:inline-flex items-center gap-2 rounded-full bg-c700 pl-3.5 pr-1.5 py-1 text-[13px] text-paper no-underline transition-transform duration-500 ease-glaze hover:bg-c800 active:scale-[0.98]">
            {pkg => (
              <>
                {t('nav.download')}
                {pkg ? <span className="sr-only"> {t(`pkg.${pkg.id}`)}</span> : null}
                <span className="grid size-6 place-items-center rounded-full bg-white/15">
                  {pkg ? (
                    <DownloadGlyph />
                  ) : (
                    <svg viewBox="0 0 16 16" className="size-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
                      <path d="M4 12 12 4M6 4h6v6" />
                    </svg>
                  )}
                </span>
              </>
            )}
          </DownloadLink>
          {/* 手机：放大镜 + 汉堡 */}
          <button
            type="button"
            onClick={e => openSearch(e.currentTarget)}
            onPointerDown={preloadSearch}
            aria-label={t('nav.searchLabel')}
            aria-haspopup="dialog"
            className="grid size-9 place-items-center rounded-full text-ink-2 hover:bg-ink/5 md:hidden"
          >
            <SearchGlyph className="size-[17px]" />
          </button>
          <button
            ref={menuButton}
            type="button"
            aria-label={open ? t('nav.closeMenu') : t('nav.openMenu')}
            aria-expanded={open}
            onClick={() => setOpen(v => !v)}
            className="md:hidden relative grid size-9 place-items-center rounded-full hover:bg-ink/5"
          >
            <span className={cn('absolute h-px w-4 bg-ink transition-transform duration-500 ease-glaze', open ? 'rotate-45' : '-translate-y-[3px]')} />
            <span className={cn('absolute h-px w-4 bg-ink transition-transform duration-500 ease-glaze', open ? '-rotate-45' : 'translate-y-[3px]')} />
          </button>
        </nav>
      </header>

      <div ref={overlay} className="fixed inset-0 z-30 invisible bg-paper/85 backdrop-blur-2xl md:hidden" aria-hidden={!open}>
        <div className="flex h-full flex-col justify-center px-8">
          <p className="eyebrow mb-8 w-max">
            <span className="eyebrow-dot" />
            {t('nav.menuTag')}
          </p>
          <ul className="space-y-2">
            {[...items, { key: 'download', to: '/download' } as const].map(it => (
              <li key={it.key} data-link>
                <LocaleLink to={it.to} className="block font-serif text-4xl font-medium text-ink no-underline py-2" onClick={() => setOpen(false)}>
                  {t(`nav.${it.key}`)}
                </LocaleLink>
              </li>
            ))}
            <li data-link>
              <button type="button" onClick={() => openSearch(menuButton.current)} onPointerDown={preloadSearch} className="flex items-center gap-4 py-2 font-serif text-4xl font-medium text-ink">
                {t('nav.search')}
                <SearchGlyph className="size-7 text-ink-3" />
              </button>
            </li>
            <li data-link>
              <button
                type="button"
                onClick={switchLocale}
                lang={locale === 'zh' ? HTML_LANG.en : HTML_LANG.zh}
                aria-label={other.label}
                className="py-2 font-serif text-4xl font-medium text-ink-3"
              >
                {other.text}
              </button>
            </li>
          </ul>
          <p data-link className="mt-12 text-sm text-ink-3">
            {t('nav.platforms')}
          </p>
        </div>
      </div>

      {searchMounted ? (
        <Suspense fallback={null}>
          <SearchDialog open={searchOpen} onClose={closeSearch} returnFocus={returnFocus} />
        </Suspense>
      ) : null}
    </>
  );
}

function SearchGlyph({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 20 20" className={className} fill="none" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" aria-hidden="true">
      <circle cx="8.75" cy="8.75" r="5.75" />
      <path d="m13 13 4.25 4.25" />
    </svg>
  );
}
