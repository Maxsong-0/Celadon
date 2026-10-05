import { useLayoutEffect, useRef, useState, type CSSProperties } from 'react';
import { flushSync } from 'react-dom';
import { measureTextWidth } from '@/lib/svgMeasure';
import { wrapWords } from '@/lib/wrapWords';

/**
 * SVG 1.1 的 <text> 不会自动换行，而图里标签的宽度随语言变（英文常比中文宽一倍以上）。
 * 这里放得下就一行，放不下按词折成几行（中日韩文字按字折；折行规则见 lib/wrapWords.ts）；宽度在 SVG 用户单位下量
 * （lib/svgMeasure.ts，三个引擎一致），与屏幕缩放无关——index.css 里 svg text 的 geometricPrecision 保证每个字形的步进不被取整。
 */

type WrappedTextProps = {
  text: string;
  x: number;
  /** 基线：grow = 'down' 时是第一行，'up' 时是最后一行（贴着图底的图注向上长）。 */
  y: number;
  /** 可用宽度（用户单位）。 */
  maxWidth: number;
  lineHeight: number;
  grow?: 'down' | 'up';
  /** 这张图给这段字留的行数：折出来更多行会压到图形（横扫查得到），开发期在控制台告警。 */
  maxLines?: number;
  style?: CSSProperties;
};

/**
 * 放不下就按词折行的 SVG 文字。挂载时在首次绘制前排好；字体加载完、所在 SVG 的尺寸变了（从 display: none 变成可见）都重排，
 * 这两类回调不在 React 的提交阶段里，更新用 flushSync 让它赶在下一次绘制之前生效，不会先画一帧旧排法。
 */
export function WrappedText({ text, x, y, maxWidth, lineHeight, grow = 'down', maxLines, style }: WrappedTextProps) {
  const probe = useRef<SVGTextElement>(null);
  const [lines, setLines] = useState<string[]>([text]);
  // 量宽度的探针与显示的字共用同一个 style；字号、字体、字距一变，宽度就变，要重排（style 是每次新建的对象，所以取标量做依赖）
  const { fontSize, fontFamily, letterSpacing } = style ?? {};

  useLayoutEffect(() => {
    const el = probe.current;
    const svg = el?.ownerSVGElement;
    if (!el || !svg) return;
    const layout = (sync: boolean) => {
      const next = wrapWords(text, maxWidth, line => {
        el.textContent = line;
        return measureTextWidth(el);
      });
      el.textContent = '';
      if (import.meta.env.DEV && maxLines !== undefined && next.length > maxLines) {
        console.warn(`[WrappedText] 「${text}」折成 ${next.length} 行，超过这张图给的 ${maxLines} 行：会压到图形，请缩短文案或腾出位置`);
      }
      const apply = () => setLines(prev => (prev.length === next.length && prev.every((line, i) => line === next[i]) ? prev : next));
      if (sync) flushSync(apply);
      else apply();
    };
    layout(false); // layout effect 里的更新在绘制前同步提交，首帧就是排好的，不闪
    const onChange = () => layout(true);
    const resize = new ResizeObserver(onChange);
    resize.observe(svg);
    document.fonts.addEventListener('loadingdone', onChange);
    return () => {
      resize.disconnect();
      document.fonts.removeEventListener('loadingdone', onChange);
    };
  }, [text, maxWidth, maxLines, fontSize, fontFamily, letterSpacing]);

  const firstBaseline = grow === 'up' ? y - (lines.length - 1) * lineHeight : y;
  return (
    <>
      {/* 只用来量宽度：不显示，文字内容由 layout 临时写入 */}
      <text ref={probe} x={x} y={y} visibility="hidden" aria-hidden="true" style={style} />
      <text x={x} y={firstBaseline} style={style}>
        {lines.map((line, i) => (
          <tspan key={i} x={x} dy={i === 0 ? 0 : lineHeight}>{line}</tspan>
        ))}
      </text>
    </>
  );
}
