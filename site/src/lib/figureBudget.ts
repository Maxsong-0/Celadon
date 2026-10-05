/**
 * 首页示意图里「一句话最多能占多大的地方」的预算：组件按它排版，site/scripts/check-svg-text.mjs 在构建时按它核对 i18n 文案，
 * 不用等运行时横扫才发现某句话长到压图形或被裁。宽度是按字体名义比例的估算（西文 latinEm × 字号，中日韩 1 × 字号），
 * 偏保守；实际宽度由 WrappedText / Frame 在浏览器里量。
 */

/** 估算一段字的宽度（SVG 用户单位）。 */
export function estimateWidth(text: string, fontSize: number, latinEm: number): number {
  let width = 0;
  for (const ch of text) width += (/[⺀-鿿가-힯豈-﫿＀-￯]/.test(ch) ? 1 : latinEm) * fontSize;
  return width;
}

/** AlgoTeaser 三张小图的图注：viewBox 宽 280、右边留 16；9px 等宽（JetBrains Mono 0.6 em）；maxLines 是图上给图注留的行数。 */
export const ALGO_CAPTION = {
  fontSize: 9,
  latinEm: 0.6,
  viewBoxWidth: 280,
  marginRight: 16,
  balance: { x: 20, maxLines: 1 }, // 柱子的下沿在 y = 100，图注贴着底边只有一行的地方
  limit: { x: 16, maxLines: 2 }, // 曲线平台在 y = 78
  color: { x: 16, maxLines: 2 } // 两条曲线最低到 y = 84
} as const;

/** 图注能用的宽度（用户单位）：从 x 到右边距。 */
export const captionMaxWidth = (x: number): number => ALGO_CAPTION.viewBoxWidth - ALGO_CAPTION.marginRight - x;

/**
 * ProcessSection 四道工序的页眉：标题左起，说明右对齐，两边各留 margin。说明（12px 无衬线）自己比整条页眉还宽就越界，
 * 所以预算 = 整条页眉 − slack（Inter 实测约 0.51 em，估算取 0.55 em，再留 slack 余量）。标题放不下时 Frame 会上移一行，不受这条约束。
 */
export const PROCESS_FRAME = {
  viewBoxWidth: 640,
  margin: 40,
  noteFontSize: 12,
  noteLatinEm: 0.55,
  noteSlack: 20
} as const;

export const processFrameNoteBudget = (): number => PROCESS_FRAME.viewBoxWidth - 2 * PROCESS_FRAME.margin - PROCESS_FRAME.noteSlack;
