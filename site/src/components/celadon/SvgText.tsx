import { useLayoutEffect, useRef, useState, type CSSProperties } from 'react';

/**
 * SVG 1.1 的 <text> 不会自动换行，而图里标签的宽度随语言变（英文常比中文宽一倍以上）。
 * 这里放得下就一行，放不下按词折成几行；宽度在 SVG 用户单位下量（getComputedTextLength），
 * 与屏幕缩放无关——index.css 里 svg text 的 geometricPrecision 保证每个字形的步进不被取整。
 */

/**
 * 贪心折行：按空白拆词，一行装不下就换行；单个词比 maxWidth 还宽时独占一行（不拆词）。量宽度的办法由调用方给。
 * 数字和后面的单位（25 Hz、3.8 dB、10 s）粘成一个词，不会在它们中间断开。
 */
export function wrapWords(text: string, maxWidth: number, widthOf: (line: string) => number): string[] {
  const lines: string[] = [];
  let line = '';
  const words = text.replace(/(\d)[ \t]+(?=[A-Za-z%°µ])/g, '$1 ').split(/[ \t\r\n]+/).filter(Boolean);
  for (const word of words) {
    const candidate = line ? `${line} ${word}` : word;
    if (line && widthOf(candidate) > maxWidth) {
      lines.push(line);
      line = word;
    } else {
      line = candidate;
    }
  }
  lines.push(line);
  return lines;
}

type WrappedTextProps = {
  text: string;
  x: number;
  /** 基线：grow = 'down' 时是第一行，'up' 时是最后一行（贴着图底的图注向上长）。 */
  y: number;
  /** 可用宽度（用户单位）。 */
  maxWidth: number;
  lineHeight: number;
  grow?: 'down' | 'up';
  style?: CSSProperties;
};

/** 放不下就按词折行的 SVG 文字。字体加载完、所在 SVG 的尺寸变了（从 display: none 变成可见）都重排。 */
export function WrappedText({ text, x, y, maxWidth, lineHeight, grow = 'down', style }: WrappedTextProps) {
  const probe = useRef<SVGTextElement>(null);
  const [lines, setLines] = useState<string[]>([text]);

  useLayoutEffect(() => {
    const el = probe.current;
    const svg = el?.ownerSVGElement;
    if (!el || !svg) return;
    const layout = () => {
      const next = wrapWords(text, maxWidth, line => {
        el.textContent = line;
        return el.getComputedTextLength();
      });
      el.textContent = '';
      setLines(prev => (prev.length === next.length && prev.every((line, i) => line === next[i]) ? prev : next));
    };
    layout(); // 首次绘制前就排好，不闪
    const resize = new ResizeObserver(layout);
    resize.observe(svg);
    document.fonts.addEventListener('loadingdone', layout);
    return () => {
      resize.disconnect();
      document.fonts.removeEventListener('loadingdone', layout);
    };
  }, [text, maxWidth]);

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
