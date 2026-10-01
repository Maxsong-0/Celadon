import { useEffect, useRef } from 'react';
import { useLocation } from 'react-router-dom';
import { Chapter, Em, LinkArrow } from '@/components/celadon/Eyebrow';
import { CHANGELOG, type ReleaseNote } from '@/content/changelog';
import { ScrollTrigger } from '@/lib/gsap';
import { onFontsLoaded } from '@/lib/fonts';
import { scrollToAnchor } from '@/lib/anchor';
import { useReveal } from '@/lib/reveal';
import { PLUGIN_VERSION } from '@/lib/site';

/*
  /changelog：版本时间线，新的在上。左栏版本号（大字）+ 日期，桌面上随滚动钉住；右栏一句话 + 细线条目。
  内容只从 content/changelog.ts 读；version 与 PLUGIN_VERSION 相同的那条标「当前版本」。
*/
export default function Changelog() {
  const root = useRef<HTMLElement>(null);
  useReveal(root);
  usePageSetup('更新日志 · Celadon');
  const current = CHANGELOG.find(n => n.version === PLUGIN_VERSION);

  return (
    <main ref={root}>
      <section className="section-last pt-32 sm:pt-40">
        <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
          <header className="max-w-3xl">
            <div data-reveal>
              <Chapter n="记" label="更新日志" en="Changelog" />
            </div>
            <h1 data-reveal className="mt-8 font-serif text-[clamp(2rem,4.2vw,3.5rem)] font-medium leading-[1.12] tracking-tight text-ink">
              每一窑，<Em>改了什么</Em>。
            </h1>
            <p data-reveal className="mt-6 measure text-[17px] leading-[1.85] text-ink-2">
              新的在上。只记听得见、用得着的变化——听感、功能、兼容、修复。版本号就是插件顶栏上显示的那个。
              {current ? null : <>插件已经到 {PLUGIN_VERSION}，这一版的记录还在写。</>}
            </p>
          </header>

          <ol className="mt-20 space-y-20 sm:mt-24 sm:space-y-28" aria-label="版本">
            {CHANGELOG.map(note => (
              <Entry key={note.id} note={note} current={note === current} />
            ))}
          </ol>
        </div>
      </section>
    </main>
  );
}

const ISO_DAY = /^\d{4}-\d{2}-\d{2}$/;

function Entry({ note, current }: { note: ReleaseNote; current: boolean }) {
  return (
    <li id={note.id} data-reveal className="grid scroll-mt-28 gap-7 md:grid-cols-12 md:gap-10">
      <div className="md:sticky md:top-28 md:col-span-4 md:self-start lg:col-span-3">
        <div className="flex items-baseline justify-between gap-4 md:block">
          <h2 className="display text-[clamp(2.5rem,4.6vw,3.75rem)] leading-none tabular-nums text-ink">{note.version ?? note.label}</h2>
          <p className="font-mono text-[12px] tracking-wider text-ink-3 md:mt-4">
            {/* 单日写成 <time>；合并条目是区间，照原样写 */}
            {ISO_DAY.test(note.date) ? <time dateTime={note.date}>{note.date}</time> : note.date}
          </p>
        </div>
        {current ? (
          <p className="mt-3 flex items-center gap-2 text-[13px] tracking-[0.12em] text-c700">
            <span className="size-1.5 rounded-full bg-c500" aria-hidden="true" />
            当前版本
          </p>
        ) : note.note ? (
          <p className="mt-3 text-[13px] tracking-[0.12em] text-ink-4">{note.note}</p>
        ) : null}
        {current ? (
          <div className="mt-6 hidden md:block">
            <LinkArrow to="/download">下载这一版</LinkArrow>
          </div>
        ) : null}
      </div>

      <div className="md:col-span-8 lg:col-span-8 lg:col-start-5">
        <p className="font-serif text-[clamp(1.375rem,2.2vw,1.75rem)] font-medium leading-snug text-ink">{note.title}</p>
        <ul className="rule-list mt-8">
          {note.items.map((item, i) => (
            <li key={i} className="grid grid-cols-[3.25rem_1fr] items-baseline gap-4 py-5 sm:grid-cols-[4.5rem_1fr] sm:gap-6">
              <span className="text-[12px] tracking-[0.2em] text-ink-4">{item.kind}</span>
              <p className="measure text-[15.5px] leading-[1.8] text-ink-2">{item.text}</p>
            </li>
          ))}
        </ul>
        {current ? (
          <div className="mt-8 md:hidden">
            <LinkArrow to="/download">下载这一版</LinkArrow>
          </div>
        ) : null}
      </div>
    </li>
  );
}

/** 页面标题、带 #版本 进来时滚到那一版、字体到位后让滚动触发器重新量一遍位置。 */
function usePageSetup(title: string) {
  const { hash, key } = useLocation();

  useEffect(() => {
    const prev = document.title;
    document.title = title;
    return () => {
      document.title = prev;
    };
  }, [title]);

  // 平滑滚动会被字体到位后的 ScrollTrigger 重新测量打断（原来带锚点进来停在页首），由 lib/anchor 处理。
  useEffect(() => scrollToAnchor(hash), [hash, key]);

  // 字体换上后触发器位置会变，重新量（思源的声明是异步插入的，见 lib/fonts）。
  useEffect(() => onFontsLoaded(() => ScrollTrigger.refresh()), []);
}
