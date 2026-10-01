import { useId, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { Chapter } from './Eyebrow';
import Tnum from './Tnum';
import { gsap, ScrollTrigger, useGSAP } from '@/lib/gsap';
import { useReveal } from '@/lib/reveal';
import { cn, prefersReducedMotion } from '@/lib/utils';

interface Stat {
  value: number;
  /** 千分位分隔（只给大整数） */
  grouped?: boolean;
}

// 数字与注释都来自验证记录；改之前先查记录。单位、标签、注释在 home 命名空间 numbers.stats（同一顺序）。
const stats: Stat[] = [{ value: 27 }, { value: 8 }, { value: 1.3 }, { value: 134597, grouped: true }, { value: 0.35 }, { value: 28.8 }];

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
  const { t } = useTranslation('home');
  const texts = t('numbers.stats', { returnObjects: true });
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
          {t('chapters.numbers.label')}
        </h2>
        <div data-reveal>
          <Chapter n="柒" label={t('chapters.numbers.label')} en={t('chapters.numbers.en')} />
        </div>
        <p data-reveal className="mt-8 measure text-[15px] leading-[1.85] text-ink-2">
          {t('numbers.intro')}
        </p>
        <ul ref={list} role="list" className="mt-16 grid grid-cols-2 border-y border-line md:grid-cols-3 lg:grid-cols-6">
          {stats.map((_, i) => (
            <li key={i} className={cellClasses[i]}>
              {/* 入场动效只动格里的字，竖线与横线不跟着飘。 */}
              <div data-reveal>
                {/* 这一行是尺寸容器：数字字号按格宽（cqi）取，计数途中最宽的一串（如 100,000）也放得下；不认 cqi 的浏览器退回按视口宽取的保守字号。 */}
                <p className="@container">
                  <span className="sr-only">{texts[i].unit ? `${finals[i]} ${texts[i].unit}` : finals[i]}</span>
                  <span
                    aria-hidden="true"
                    className="display block whitespace-nowrap text-[length:clamp(1.75rem,3.6vw,3rem)] leading-none text-ink tabular-nums supports-[width:1cqi]:text-[length:clamp(1.75rem,28cqi,3.4rem)]"
                  >
                    <span data-count={i}>
                      <Tnum text={finals[i]} />
                    </span>
                    {texts[i].unit ? <span className="ml-[0.3em] text-[0.45em] text-ink-3">{texts[i].unit}</span> : null}
                  </span>
                </p>
                <p className="mt-5 text-[14px] font-medium leading-snug text-ink-2">{texts[i].label}</p>
                <p className="mt-1.5 text-[12px] leading-[1.7] text-ink-4">{texts[i].note}</p>
              </div>
            </li>
          ))}
        </ul>
      </div>
    </section>
  );
}
