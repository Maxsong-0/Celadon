import { localeFromPath } from '@/i18n/paths';

/**
 * 思源宋体 / 黑体（fontsource，按 unicode-range 各拆成约 100 个子集）光 @font-face 声明就有约 200 KB，
 * 只在中文页需要（ADR-024：英文页正文用 Inter，标题用 Fraunces，都在首屏样式表里）。
 * 中文时由入口或 LocaleSync 异步插入（Vite 把它们拆成单独的样式表，不阻塞渲染）：先用系统字体出字，子集到了再换上。
 * 要按字体量尺寸的地方先等 fontFacesReady：首屏标题的逐字动画、ScrollTrigger 的重新测量（onFontsLoaded）。
 */
let cjk: Promise<void> | null = null;

export function loadCjkFonts(): Promise<void> {
  return (cjk ??= Promise.all([import('@fontsource-variable/noto-serif-sc'), import('@fontsource-variable/noto-sans-sc')]).then(
    () => undefined,
    () => undefined // 没拉到就一直用系统字体，不报错
  ));
}

/** 当前语言要用的字体声明到位：中文等思源的声明插入；英文的字体声明在首屏样式表里，立即就绪。 */
export function fontFacesReady(): Promise<void> {
  if (typeof location === 'undefined') return Promise.resolve();
  return localeFromPath(location.pathname) === 'zh' ? loadCjkFonts() : Promise.resolve();
}

/**
 * 字体换上后回调：当前语言的字体声明到了、排版用到的子集下完时回调，之后再有字体下完（如出现新的字）也会回调。
 * 给 ScrollTrigger.refresh 用——换字体后行高与宽度会变，触发位置要重新量。返回取消函数。
 */
export function onFontsLoaded(cb: () => void): () => void {
  const fonts = typeof document === 'undefined' ? undefined : document.fonts;
  if (!fonts) return () => undefined;
  let alive = true;
  const fire = () => {
    if (alive) cb();
  };
  fonts.addEventListener('loadingdone', fire);
  void fontFacesReady()
    .then(() => fonts.ready)
    .then(fire);
  return () => {
    alive = false;
    fonts.removeEventListener('loadingdone', fire);
  };
}
