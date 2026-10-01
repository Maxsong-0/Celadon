import { lazy, Suspense, useCallback, useEffect, useLayoutEffect, useRef, useState, type ComponentType } from 'react';
import { Link, NavLink, useLocation } from 'react-router-dom';
import type { SearchDialogProps } from '@/components/docs/Search';
import { gsap, useGSAP } from '@/lib/gsap';
import { tracks } from '@/content/listen';
import { cn, prefersReducedMotion } from '@/lib/utils';
import Mark from './Mark';
import DownloadLink, { DownloadGlyph } from './DownloadLink';
import { PACKAGE_LABEL } from '@/lib/platform';

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
  { label: '工艺', to: '/#process' },
  ...(tracks.length ? [{ label: '试听', to: '/#listen' }] : []),
  { label: '算法', to: '/algorithms' },
  { label: '说明', to: '/docs' },
  { label: '版本', to: '/changelog' }
];

const SITE_ORIGIN = 'https://celadon.maxsong.me';

/** Mac / iPad 上提示 ⌘K，其余（Windows / Linux）提示 Ctrl K；两种组合键在哪儿都认。 */
const IS_MAC =
  typeof navigator !== 'undefined' &&
  /mac|iphone|ipad|ipod/i.test((navigator as Navigator & { userAgentData?: { platform?: string } }).userAgentData?.platform || navigator.platform || navigator.userAgent);
const SHORTCUT = IS_MAC ? '⌘K' : 'Ctrl K';

let siteHead: { title: string; description: string } | null = null;

/**
 * 每次换路径：标题与摘要先恢复成站点默认（说明页等在自己的 effect 里再覆盖成本页的），规范地址改成当前路径。
 * 导航排在各页面之前，它的 layout effect 先跑，页面自己的设置一定落在后面。
 * 规范地址的那条 <link> 由 index.html 里的小脚本在首屏按路径注入（全站只有一条），这里只在站内跳转时跟着改。
 */
function useHeadSync(pathname: string) {
  useLayoutEffect(() => {
    const desc = document.querySelector<HTMLMetaElement>('meta[name="description"]');
    siteHead ??= { title: document.title, description: desc?.content ?? '' };
    document.title = siteHead.title;
    desc?.setAttribute('content', siteHead.description);
    let canonical = document.querySelector<HTMLLinkElement>('link[rel="canonical"]');
    if (!canonical) {
      canonical = document.createElement('link');
      canonical.rel = 'canonical';
      document.head.appendChild(canonical);
    }
    canonical.href = SITE_ORIGIN + (pathname.replace(/\/+$/, '') || '/');
  }, [pathname]);
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

  const isActive = (to: string) => (to.startsWith('/#') ? false : location.pathname.startsWith(to));

  return (
    <>
      <header className="fixed inset-x-0 top-0 z-40 flex justify-center px-4 pt-4 sm:pt-5 pointer-events-none">
        <nav
          aria-label="主导航"
          className={cn(
            'pointer-events-auto flex items-center gap-1 rounded-full bg-paper/70 pl-3 pr-1.5 py-1.5 ring-1 ring-ink/8 backdrop-blur-xl transition-all duration-700 ease-glaze',
            scrolled ? 'shadow-[0_10px_40px_-18px_rgba(13,40,48,0.35)]' : 'shadow-none'
          )}
        >
          <Link to="/" className="flex items-center gap-2.5 pr-3 no-underline" aria-label="Celadon 首页">
            <Mark className="size-7" />
            <span className="font-display text-[15px] tracking-tight text-ink">Celadon</span>
            <span className="hidden sm:inline font-serif text-[13px] text-ink-3 tracking-[0.2em] pl-1">青瓷</span>
          </Link>
          <ul className="hidden md:flex items-center gap-0.5">
            {items.map(it => (
              <li key={it.to}>
                {it.to.startsWith('/#') ? (
                  <Link to={it.to} className="block rounded-full px-3.5 py-1.5 text-[13px] text-ink-2 no-underline transition-colors duration-300 hover:bg-ink/5 hover:text-ink">
                    {it.label}
                  </Link>
                ) : (
                  <NavLink
                    to={it.to}
                    className={cn(
                      'block rounded-full px-3.5 py-1.5 text-[13px] no-underline transition-colors duration-300 hover:bg-ink/5 hover:text-ink',
                      isActive(it.to) ? 'bg-ink text-paper hover:bg-ink hover:text-paper' : 'text-ink-2'
                    )}
                  >
                    {it.label}
                  </NavLink>
                )}
              </li>
            ))}
          </ul>
          {/* 桌面：放大镜 + 快捷键提示 */}
          <button
            type="button"
            onClick={e => openSearch(e.currentTarget)}
            onPointerEnter={preloadSearch}
            onFocus={preloadSearch}
            aria-label="搜索说明与算法"
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
                下载
                {pkg ? <span className="sr-only"> {PACKAGE_LABEL[pkg.id]}</span> : null}
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
            aria-label="搜索说明与算法"
            aria-haspopup="dialog"
            className="grid size-9 place-items-center rounded-full text-ink-2 hover:bg-ink/5 md:hidden"
          >
            <SearchGlyph className="size-[17px]" />
          </button>
          <button
            ref={menuButton}
            type="button"
            aria-label={open ? '关闭菜单' : '打开菜单'}
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
          <p className="eyebrow mb-8 w-max"><span className="eyebrow-dot" />Celadon · 青瓷</p>
          <ul className="space-y-2">
            {[...items, { label: '下载', to: '/download' }].map(it => (
              <li key={it.label} data-link>
                <Link to={it.to} className="block font-serif text-4xl font-medium text-ink no-underline py-2" onClick={() => setOpen(false)}>
                  {it.label}
                </Link>
              </li>
            ))}
            <li data-link>
              <button type="button" onClick={() => openSearch(menuButton.current)} onPointerDown={preloadSearch} className="flex items-center gap-4 py-2 font-serif text-4xl font-medium text-ink">
                搜索
                <SearchGlyph className="size-7 text-ink-3" />
              </button>
            </li>
          </ul>
          <p data-link className="mt-12 text-sm text-ink-3">VST3 · AU · macOS 12+（Apple Silicon / Intel）· Windows 10 / 11 x64</p>
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
