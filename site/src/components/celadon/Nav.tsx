import { useEffect, useRef, useState } from 'react';
import { Link, NavLink, useLocation } from 'react-router-dom';
import { gsap, useGSAP } from '@/lib/gsap';
import { cn } from '@/lib/utils';
import Mark from './Mark';

const items = [
  { label: '工艺', to: '/#process' },
  { label: '算法', to: '/algorithms' },
  { label: '说明', to: '/docs' },
  { label: '获取', to: '/#get' }
];

/** 浮在页面顶部的「岛」式导航：玻璃胶囊，滚动后缩紧；手机端汉堡变叉、整屏展开、链接逐条浮起。 */
export default function Nav() {
  const [open, setOpen] = useState(false);
  const [scrolled, setScrolled] = useState(false);
  const overlay = useRef<HTMLDivElement>(null);
  const location = useLocation();

  useEffect(() => {
    const onScroll = () => setScrolled(window.scrollY > 24);
    onScroll();
    window.addEventListener('scroll', onScroll, { passive: true });
    return () => window.removeEventListener('scroll', onScroll);
  }, []);

  useEffect(() => setOpen(false), [location]);

  useGSAP(
    () => {
      const el = overlay.current;
      if (!el) return;
      const links = el.querySelectorAll('[data-link]');
      if (open) {
        gsap.set(el, { visibility: 'visible' });
        gsap.fromTo(el, { opacity: 0 }, { opacity: 1, duration: 0.5, ease: 'power2.out' });
        gsap.fromTo(links, { y: 48, opacity: 0 }, { y: 0, opacity: 1, stagger: 0.07, duration: 0.8, ease: 'power3.out', delay: 0.1 });
        document.documentElement.style.overflow = 'hidden';
      } else {
        gsap.to(el, { opacity: 0, duration: 0.35, ease: 'power2.in', onComplete: () => gsap.set(el, { visibility: 'hidden' }) });
        document.documentElement.style.overflow = '';
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
          <Link
            to="/docs"
            className="hidden md:inline-flex items-center gap-2 rounded-full bg-c700 pl-3.5 pr-1.5 py-1 text-[13px] text-paper no-underline transition-transform duration-500 ease-glaze hover:bg-c800 active:scale-[0.98]"
          >
            开始使用
            <span className="grid size-6 place-items-center rounded-full bg-white/15">
              <svg viewBox="0 0 16 16" className="size-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
                <path d="M4 12 12 4M6 4h6v6" />
              </svg>
            </span>
          </Link>
          <button
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
            {[...items, { label: '开始使用', to: '/docs' }].map(it => (
              <li key={it.label} data-link>
                <Link to={it.to} className="block font-serif text-4xl font-medium text-ink no-underline py-2" onClick={() => setOpen(false)}>
                  {it.label}
                </Link>
              </li>
            ))}
          </ul>
          <p data-link className="mt-12 text-sm text-ink-3">VST3 · AU · macOS 12+ 通用二进制 · Windows 10 / 11 x64</p>
        </div>
      </div>
    </>
  );
}
