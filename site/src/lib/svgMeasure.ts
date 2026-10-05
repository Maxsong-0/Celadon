/**
 * 量一段 SVG 文字渲染出来有多宽（用户单位）。
 *
 * 不用 getComputedTextLength：WebKit（Safari）里它不算 letter-spacing——CLAY 帧的标题字距 0.18em、32 个字，少算约 70 个单位，
 * 页眉据此以为标题与说明放得下同一行，实际重叠 17 个单位（T-069 用 WebKit 26.5 实测到的）。getBBox().width 三个引擎都算字距
 * （WebKit 比 Chromium / Firefox 少末字后面那一份字距，约 2 个单位，在 FRAME_GAP 的余量之内；无字距的文字三家逐位相同）。
 * 元素必须在渲染树里（display: none 量出来是 0；visibility: hidden 可以）。
 */
export function measureTextWidth(el: SVGTextElement): number {
  return el.getBBox().width;
}
