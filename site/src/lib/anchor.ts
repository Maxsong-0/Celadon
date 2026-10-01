import { ScrollTrigger } from '@/lib/gsap';
import { prefersReducedMotion } from '@/lib/utils';

const USER_INPUT = ['wheel', 'touchstart', 'keydown', 'pointerdown'] as const;

/**
 * 滚到地址里的 #锚点，给用 ScrollTrigger 的页面（首页、下载、更新日志）用。
 * 平滑滚动在这些页面上很容易半路停下：ScrollTrigger 每次重新测量（字体晚到、改窗口大小）都会把滚动位置写回测量开始时的值，
 * 字体换上后的版面变化也会打断它；钉住的段落量好之后目标位置还会变。所以：
 * - 先等布局稳定、测量一次再滚；进页面时（新开、或从别的页面跳过来）直接跳到位，与浏览器自己处理锚点一样；
 *   只有已在本页时改锚点（smooth = true）才平滑滚过去；
 * - 之后每次重新测量完再对准一次（直接跳），直到用户自己滚动、按键或点按为止。
 * 返回取消函数（给 effect 的清理用）。
 */
export function scrollToAnchor(hash: string, smooth = false): () => void {
  if (!hash) return () => undefined;
  let id = hash.slice(1);
  try {
    id = decodeURIComponent(id);
  } catch {
    // 手敲坏了的百分号编码：原样找，找不到就不滚
  }
  let active = true;
  let first = true;
  const go = () => {
    if (!active) return;
    const behavior: ScrollBehavior = first && smooth && !prefersReducedMotion() ? 'smooth' : ('instant' as ScrollBehavior);
    first = false;
    document.getElementById(id)?.scrollIntoView({ behavior, block: 'start' });
  };
  const timer = window.setTimeout(() => {
    ScrollTrigger.refresh(); // 测量完会派发 refresh，那里已经滚过就不再滚
    if (first) go();
  }, 80);
  ScrollTrigger.addEventListener('refresh', go);
  const stop = () => {
    if (!active) return;
    active = false;
    window.clearTimeout(timer);
    ScrollTrigger.removeEventListener('refresh', go);
    for (const ev of USER_INPUT) window.removeEventListener(ev, stop);
  };
  for (const ev of USER_INPUT) window.addEventListener(ev, stop, { passive: true });
  return stop;
}
