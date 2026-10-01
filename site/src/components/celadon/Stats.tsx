import { useId, useRef } from 'react';
import { Chapter } from './Eyebrow';
import Tnum from './Tnum';
import { gsap, ScrollTrigger, useGSAP } from '@/lib/gsap';
import { useReveal } from '@/lib/reveal';
import { cn, prefersReducedMotion } from '@/lib/utils';

interface Stat {
  value: number;
  unit: string;
  label: string;
  note: string;
  /** 千分位分隔（只给大整数） */
  grouped?: boolean;
}

// 数字与注释都来自验证记录；改之前先查记录。
const stats: Stat[] = [
  { value: 27, unit: '段', label: '动态频谱平衡', note: '1/3 倍频程，40 Hz ～ 16 kHz' },
  { value: 8, unit: '×', label: '真峰值插值', note: '8 相 × 64 抽头，限的峰 = 量的峰' },
  { value: 1.3, unit: '%', label: '音频线程预算', note: '48 kHz · M2 Pro · Release · 界面关' },
  { value: 134597, unit: '', label: '自动测试断言', note: 'Linux GCC 与 MSVC + Wine 全套 · 0 失败', grouped: true },
  { value: 0.35, unit: 'dB', label: '真峰值重建余量', note: '16× 参考检测最坏仍低于 Ceiling' },
  { value: 28.8, unit: 'ms', label: '总延迟', note: '48 kHz · Lookahead 2 ms · 宿主自动补偿' }
];

// 小数位照目标值本身（1.3 → 1 位、0.35 → 2 位），计数途中也一直是这么多位。格式器只建一次，不在每帧里建。
const formats = stats.map(s => {
  const decimals = (String(s.value).split('.')[1] ?? '').length;
  return new Intl.NumberFormat('en-US', { minimumFractionDigits: decimals, maximumFractionDigits: decimals, useGrouping: s.grouped ?? false });
});
const finals = stats.map((s, i) => formats[i].format(s.value));

/**
 * 一行数字、细线分隔：手机 2 列、平板 3 列、桌面 6 列一行。上下两根通栏细线不动；每行行首一格贴左（与章节标记对齐），
 * 其余各格左边一根竖线，换行处一根横线。每一格左右内距之和相同，数字按格宽（cqi）取字号时整行一样大。
 */
const cellClasses = stats.map((_, i) =>
  cn(
    'border-line py-8 lg:py-10',
    i % 2 === 0 ? 'pr-7' : 'border-l pl-5 pr-2',
    i >= 2 && 'border-t',
    i % 3 === 0 ? 'md:border-l-0 md:pl-0 md:pr-8' : 'md:border-l md:pl-6 md:pr-2',
    i >= 3 ? 'md:border-t' : 'md:border-t-0',
    i === 0 ? 'lg:border-l-0 lg:pl-0 lg:pr-8' : 'lg:border-l lg:pl-6 lg:pr-2',
    'lg:border-t-0'
  )
);

/** 改 React 渲染出的那个文本节点本身（不换节点），不和 React 的对账打架。 */
function setText(el: HTMLElement, text: string) {
  const node = el.firstChild;
  if (node && node.nodeType === Node.TEXT_NODE) node.nodeValue = text;
  else el.textContent = text;
}

/** 柒 · 数字：全部来自验证记录。进视口时从 0 数到目标值一次；减少动效时直接是终值。 */
export default function Stats() {
  const root = useRef<HTMLElement>(null);
  const list = useRef<HTMLUListElement>(null);
  const titleId = useId();
  useReveal(root);

  useGSAP(
    () => {
      if (!list.current || prefersReducedMotion()) return;
      const counters = gsap.utils.toArray<HTMLElement>('[data-count]', list.current).map(el => {
        const i = Number(el.dataset.count);
        return { el, cells: [...el.children] as HTMLElement[], format: formats[i], to: stats[i].value, final: finals[i], v: 0, shown: '' };
      });
      if (!counters.length) return;
      type Counter = (typeof counters)[number];
      const byEl = new Map(counters.map(c => [c.el, c] as const));
      // 数字是一格一格的（Tnum，数字格同宽）：计数途中的数右对齐填进终值的那些格，位数还不够时左边的格占着位置不显示——
      // 个位、小数点、千分位与单位都不挪，整串不晃。
      const fill = (c: Counter, text: string) => {
        const padded = text.padStart(c.cells.length, ' ');
        c.cells.forEach((cell, k) => {
          const blank = padded[k] === ' ';
          setText(cell, blank ? c.final[k] : padded[k]);
          cell.style.visibility = blank ? 'hidden' : '';
        });
      };
      const paint = (group: Counter[]) => {
        for (const c of group) {
          const text = c.format.format(c.v);
          if (text !== c.shown) {
            c.shown = text;
            fill(c, text);
          }
        }
      };
      // 先归零：这时数字还在视口下方，或被入场动效藏着，看不到这一跳。
      paint(counters);
      // 与入场动效同一套分批（ScrollTrigger.batch、'top 88%'）：手机上三行各自进视口时才数，数字开始数时正好开始浮现。
      ScrollTrigger.batch(
        counters.map(c => c.el),
        {
          start: 'top 88%',
          once: true,
          onEnter: els => {
            const group = els.flatMap(el => byEl.get(el as HTMLElement) ?? []);
            if (group.length) gsap.to(group, { v: (_: number, c: Counter) => c.to, duration: 1.6, stagger: 0.08, onUpdate: () => paint(group) });
          }
        }
      );
      // 撤掉时写回终值（StrictMode 重挂、热更新时不留半截数字）。
      return () => {
        for (const c of counters) fill(c, c.final);
      };
    },
    { scope: root }
  );

  return (
    <section ref={root} aria-labelledby={titleId} className="section">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <h2 id={titleId} className="sr-only">
          数字
        </h2>
        <div data-reveal>
          <Chapter n="柒" label="数字" en="In numbers" />
        </div>
        <p data-reveal className="mt-8 measure text-[15px] leading-[1.85] text-ink-2">
          全部来自验证记录。百分比 = 单个音频线程「处理耗时 / 这一块音频的播放时长」，不是整机 CPU；测试数是 0.1.8.7 发布前在 Linux 与 Windows 上的全套结果。
        </p>
        <ul ref={list} role="list" className="mt-16 grid grid-cols-2 border-y border-line md:grid-cols-3 lg:grid-cols-6">
          {stats.map((s, i) => (
            <li key={s.label} className={cellClasses[i]}>
              {/* 入场动效只动格里的字，竖线与横线不跟着飘。 */}
              <div data-reveal>
                {/* 这一行是尺寸容器：数字字号按格宽（cqi）取，计数途中最宽的一串（如 100,000）也放得下；不认 cqi 的浏览器退回按视口宽取的保守字号。 */}
                <p className="@container">
                  <span className="sr-only">{s.unit ? `${finals[i]} ${s.unit}` : finals[i]}</span>
                  <span
                    aria-hidden="true"
                    className="display block whitespace-nowrap text-[length:clamp(1.75rem,3.6vw,3rem)] leading-none text-ink tabular-nums supports-[width:1cqi]:text-[length:clamp(1.75rem,28cqi,3.4rem)]"
                  >
                    <span data-count={i}>
                      <Tnum text={finals[i]} />
                    </span>
                    {s.unit ? <span className="ml-[0.3em] text-[0.45em] text-ink-3">{s.unit}</span> : null}
                  </span>
                </p>
                <p className="mt-5 text-[14px] font-medium leading-snug text-ink-2">{s.label}</p>
                <p className="mt-1.5 text-[12px] leading-[1.7] text-ink-4">{s.note}</p>
              </div>
            </li>
          ))}
        </ul>
      </div>
    </section>
  );
}
