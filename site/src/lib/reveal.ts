import type { RefObject } from 'react';
import { gsap, ScrollTrigger, useGSAP } from '@/lib/gsap';
import { prefersReducedMotion } from '@/lib/utils';

/**
 * 全站统一的入场（设计规格-官网 第 1 节第 6 条）：scope 里带 data-reveal 的元素进视口时
 * y 24 → 0、透明度 0 → 1，0.9 s，同一批交错 0.08 s；只播一次。减少动效时什么都不做（元素直接在终态）。
 */
export function useReveal(scope: RefObject<HTMLElement | null>) {
  useGSAP(
    () => {
      const root = scope.current;
      if (!root || prefersReducedMotion()) return;
      const els = gsap.utils.toArray<HTMLElement>('[data-reveal]', root);
      if (!els.length) return;
      gsap.set(els, { autoAlpha: 0, y: 24 });
      ScrollTrigger.batch(els, {
        start: 'top 88%',
        once: true,
        onEnter: batch => gsap.to(batch, { autoAlpha: 1, y: 0, duration: 0.9, stagger: 0.08, ease: 'power3.out', overwrite: true })
      });
    },
    { scope }
  );
}
