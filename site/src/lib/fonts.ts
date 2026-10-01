/**
 * 思源宋体 / 黑体（fontsource，按 unicode-range 各拆成约 100 个子集）光 @font-face 声明就有约 200 KB，
 * 放在首屏样式表里会挡住首次渲染。改为入口一执行就异步插入（Vite 把它们拆成单独的样式表，不阻塞渲染）：
 * 先用系统字体出字，子集到了再换上（font-display: swap，与原来一样）。Fraunces、JetBrains Mono 的声明很小，照旧留在首屏。
 * 要按字体量尺寸的地方先等这里：首屏标题的逐字动画等 cjkFontFaces，ScrollTrigger 的重新测量用 onFontsLoaded。
 */
export const cjkFontFaces: Promise<void> = Promise.all([
  import('@fontsource-variable/noto-serif-sc'),
  import('@fontsource-variable/noto-sans-sc')
]).then(
  () => undefined,
  () => undefined // 没拉到就一直用系统字体，不报错
);

/**
 * 字体换上后回调：思源的声明到了、排版用到的子集下完时回调，之后再有字体下完（如出现新的字）也会回调。
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
  cjkFontFaces.then(() => fonts.ready).then(fire);
  return () => {
    alive = false;
    fonts.removeEventListener('loadingdone', fire);
  };
}
