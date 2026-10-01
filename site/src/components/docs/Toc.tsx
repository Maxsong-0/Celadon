import { useEffect, useRef, useState } from 'react';
import { gsap, useGSAP } from '@/lib/gsap';
import { cn, prefersReducedMotion } from '@/lib/utils';

export interface TocItem {
  id: string;
  title: string;
}

type Where = 'above' | 'inside' | 'below';

/**
 * 本页目录：列出正文全部 h2，滚动时高亮正在读的那一节。
 * 判定只用 IntersectionObserver（不挂 scroll 事件）：视口上部 104 px ～ 42 % 这条带子是「正在读」的位置——
 * 带子里有标题就取最靠上的一个；没有就取已经滚过带子的最后一个；都没有说明还在导语里，不高亮。
 * 点击是普通锚点：浏览器自己平滑滚动（减少动效时瞬移），h2 的 scroll-mt 让标题停在导航条下面。
 */
export default function Toc({ items, className }: { items: TocItem[]; className?: string }) {
  const [active, setActive] = useState<string | null>(null);
  const list = useRef<HTMLDivElement>(null);
  const marker = useRef<HTMLSpanElement>(null);
  const markerShown = useRef(false);

  useEffect(() => {
    const targets = items.map(it => document.getElementById(it.id)).filter((el): el is HTMLElement => el !== null);
    if (!targets.length) return;
    const where = new Map<Element, Where>();
    const io = new IntersectionObserver(
      entries => {
        for (const e of entries) {
          const bandTop = e.rootBounds?.top ?? 0;
          where.set(e.target, e.isIntersecting ? 'inside' : e.boundingClientRect.top < bandTop ? 'above' : 'below');
        }
        let next: string | null = null;
        for (const el of targets) {
          const w = where.get(el);
          if (w === 'inside') {
            next = el.id;
            break;
          }
          if (w === 'above') next = el.id;
        }
        setActive(next);
      },
      { rootMargin: '-104px 0px -58% 0px', threshold: 0 }
    );
    for (const el of targets) io.observe(el);
    return () => io.disconnect();
  }, [items]);

  // 左侧细线上的一小段釉青标记：滑到当前项（GSAP；减少动效时直接到位）。第一次出现不滑，原地淡入。
  useGSAP(
    () => {
      const root = list.current;
      const m = marker.current;
      if (!root || !m) return;
      const reduce = prefersReducedMotion();
      const index = active === null ? -1 : items.findIndex(it => it.id === active);
      const link = index >= 0 ? root.querySelectorAll<HTMLElement>('a[data-toc]')[index] : undefined;
      if (!link) {
        markerShown.current = false;
        gsap.to(m, { opacity: 0, duration: reduce ? 0 : 0.3, ease: 'power2.out', overwrite: true });
        return;
      }
      const to = { y: link.offsetTop, height: link.offsetHeight };
      if (reduce || !markerShown.current) {
        gsap.set(m, to);
        gsap.to(m, { opacity: 1, duration: reduce ? 0 : 0.4, ease: 'power2.out', overwrite: 'auto' });
      } else {
        gsap.to(m, { ...to, opacity: 1, duration: 0.5, ease: 'power3.out', overwrite: true });
      }
      markerShown.current = true;
    },
    { dependencies: [active, items], scope: list }
  );

  const toTop = () => window.scrollTo({ top: 0, behavior: prefersReducedMotion() ? 'auto' : 'smooth' });

  return (
    <nav aria-label="本页目录" className={className}>
      <div className="sticky top-28 max-h-[calc(100dvh-8.5rem)] overflow-y-auto overscroll-contain pb-8 [scrollbar-width:thin]">
        <p className="mb-4 flex items-baseline gap-2 text-[11px] tracking-[0.22em] text-ink-4">
          本页目录
          <span className="display text-[12px] italic tracking-normal" style={{ fontVariationSettings: '"opsz" 14, "SOFT" 50' }}>
            On this page
          </span>
        </p>
        <div ref={list} className="relative border-l border-line">
          <span ref={marker} aria-hidden="true" className="absolute -left-px top-0 h-0 w-px bg-c600 opacity-0" />
          <ol>
            {items.map((it, i) => {
              const on = it.id === active;
              return (
                <li key={`${it.id}-${i}`}>
                  <a
                    data-toc
                    href={`#${it.id}`}
                    aria-current={on ? 'location' : undefined}
                    className={cn(
                      'block py-[7px] pl-4 text-[13px] leading-[1.55] no-underline transition-colors duration-300 ease-glaze',
                      on ? 'text-ink' : 'text-ink-3 hover:text-ink'
                    )}
                  >
                    {it.title}
                  </a>
                </li>
              );
            })}
          </ol>
        </div>
        <button type="button" onClick={toTop} className="group mt-7 inline-flex items-center gap-1.5 text-[12px] tracking-[0.12em] text-ink-4 transition-colors duration-300 hover:text-ink">
          <svg viewBox="0 0 16 16" className="size-3 transition-transform duration-500 ease-glaze group-hover:-translate-y-0.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
            <path d="M8 13V3M3.5 7.5 8 3l4.5 4.5" />
          </svg>
          回到顶部
        </button>
      </div>
    </nav>
  );
}
