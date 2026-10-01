import {
  useEffect,
  useId,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
  type MouseEvent as ReactMouseEvent,
  type ReactNode,
  type RefObject
} from 'react';
import { createPortal } from 'react-dom';
import { useLocation, useNavigate } from 'react-router-dom';
import { LocaleLink, useLocalePath } from '@/i18n/locale';
import { stripLocale } from '@/i18n/paths';
import { gsap, useGSAP } from '@/lib/gsap';
import { cn, prefersReducedMotion } from '@/lib/utils';
import { allPages, loadMarkdown, mdSections, type SitePage } from '@/content';

/** 索引里的一条：一页的导语（id = null，结果链到页首），或一页里的一个 h2 小节（链到 #锚点）。 */
interface Entry {
  key: string;
  page: SitePage;
  id: string | null;
  section: string | null;
  text: string;
  /** 小写副本，匹配只看它们 */
  lc: { title: string; summary: string; section: string; text: string };
  /** 侧栏顺序：同分时按它排 */
  order: number;
}

function makeEntry(page: SitePage, order: number, id: string | null, section: string | null, text: string): Entry {
  return {
    key: id === null ? page.path : `${page.path}#${id}`,
    page,
    id,
    section,
    text,
    lc: { title: page.title.toLowerCase(), summary: page.summary.toLowerCase(), section: (section ?? '').toLowerCase(), text: text.toLowerCase() },
    order
  };
}

/** 正文到之前就能搜的部分：每页的标题与摘要（content/index.ts 同步可得）。 */
const pageEntries: Entry[] = allPages.map((p, i) => makeEntry(p, i * 100, null, null, ''));
const DOCS_COUNT = allPages.filter(p => p.kind === 'docs').length;
const ALGO_COUNT = allPages.length - DOCS_COUNT;

/** 不输入时列出全部页面，按分区分两组；index 是在整张列表里的位置（键盘 ↑↓ 用）。 */
const browseGroups = (['docs', 'algorithms'] as const).map(kind => ({
  kind,
  label: kind === 'docs' ? '使用说明' : '公开算法',
  rows: pageEntries.flatMap((entry, index) => (entry.page.kind === kind ? [{ entry, index }] : []))
}));

function entriesOf(page: SitePage, i: number, md: string): Entry[] {
  const sections = mdSections(md);
  const intro = sections[0]?.id === null ? sections[0].text : '';
  const out = [makeEntry(page, i * 100, null, null, intro)];
  sections.forEach((s, j) => {
    if (s.id !== null) out.push(makeEntry(page, i * 100 + j + 1, s.id, s.title, s.text));
  });
  return out;
}

// 正文只在第一次打开时拉（每页一个小包，全部并行），之后重开直接用；有页没拉到就把缓存作废，下次打开再试。
let corpus: Promise<{ entries: Entry[]; complete: boolean }> | null = null;
function loadCorpus() {
  if (!corpus) {
    const pending = Promise.all(
      allPages.map((page, i) =>
        loadMarkdown(page.kind, page.file).then(
          md => ({ list: entriesOf(page, i, md), ok: true }),
          () => ({ list: [pageEntries[i]], ok: false })
        )
      )
    ).then(parts => {
      const complete = parts.every(p => p.ok);
      if (!complete) corpus = null;
      return { entries: parts.flatMap(p => p.list), complete };
    });
    corpus = pending;
  }
  return corpus;
}

/** 按空白切词；每个词原样当子串匹配（中文不分词，逐字串比），只剩标点的词丢掉。 */
function tokenize(q: string): string[] {
  const words = q.trim().toLowerCase().split(/\s+/).filter(w => /[\p{L}\p{N}]/u.test(w));
  return [...new Set(words)].slice(0, 6);
}

function occurrences(hay: string, t: string) {
  let n = 0;
  for (let i = hay.indexOf(t); i >= 0 && n < 5; i = hay.indexOf(t, i + t.length)) n++;
  return n;
}

/**
 * 打分：每个词都要在这一条里出现（与）。页名命中最重，其次小节名、摘要，正文最轻（出现多次略加）。
 * 小节条目里页名只算上下文：能凑齐「每个词都出现」，但至少要有一个词落在这一节自己的标题或正文里，
 * 否则只是页名撞上，交给那一页的导语条目去排。
 */
function rank(entries: Entry[], terms: string[]): Entry[] {
  const phrase = terms.join(' ');
  const scored: { entry: Entry; score: number }[] = [];
  for (const e of entries) {
    let total = 0;
    let own = false;
    let ok = true;
    for (const t of terms) {
      let s = 0;
      let mine = false;
      const inText = occurrences(e.lc.text, t);
      if (inText) {
        s += 1 + 0.25 * (inText - 1);
        mine = true;
      }
      if (e.id === null) {
        if (e.lc.title.includes(t)) s += e.lc.title.startsWith(t) ? 14 : 10;
        if (e.lc.summary.includes(t)) s += 4;
        if (s > 0) mine = true;
      } else {
        if (e.lc.section.includes(t)) {
          s += e.lc.section.startsWith(t) ? 9 : 7;
          mine = true;
        }
        if (e.lc.title.includes(t)) s += 1.5;
      }
      if (s === 0) {
        ok = false;
        break;
      }
      own ||= mine;
      total += s;
    }
    if (!ok || !own) continue;
    if (e.id === null && e.lc.title === phrase) total += 10;
    scored.push({ entry: e, score: total });
  }
  scored.sort((a, b) => b.score - a.score || a.entry.order - b.entry.order);
  return scored.slice(0, 40).map(s => s.entry);
}

const escapeRe = (s: string) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/** 高亮：把所有词（长的先配）包成 <mark>。 */
function highlighter(terms: string[]): (s: string) => ReactNode {
  if (!terms.length) return s => s;
  const re = new RegExp(`(${[...terms].sort((a, b) => b.length - a.length).map(escapeRe).join('|')})`, 'gi');
  return s =>
    s.split(re).map((part, i) =>
      i % 2 ? (
        <mark key={i} className="rounded-[3px] bg-c100 px-px font-medium text-c800">
          {part}
        </mark>
      ) : (
        part
      )
    );
}

/** 摘录：从最早出现的词往前留 20 个字，截一段；前后被截断就加省略号。 */
function excerpt(text: string, lcText: string, terms: string[], width = 120) {
  let at = -1;
  for (const t of terms) {
    const i = lcText.indexOf(t);
    if (i >= 0 && (at < 0 || i < at)) at = i;
  }
  const start = at < 0 ? 0 : Math.max(0, at - 20);
  const end = Math.min(text.length, start + width);
  return `${start > 0 ? '…' : ''}${text.slice(start, end)}${end < text.length ? '…' : ''}`;
}

/** 结果第二行：页面条目优先给摘要（摘要里有词或正文里没有时），小节条目给正文摘录；小节没正文就退回页面摘要。 */
function subline(e: Entry, terms: string[]) {
  if (e.id === null) {
    const useText = e.text && !terms.some(t => e.lc.summary.includes(t)) && terms.some(t => e.lc.text.includes(t));
    return useText ? excerpt(e.text, e.lc.text, terms) : e.page.summary;
  }
  return e.text ? excerpt(e.text, e.lc.text, terms) : e.page.summary;
}

const hrefOf = (e: Entry) => (e.id === null ? e.page.path : `${e.page.path}#${e.id}`);

/** 地址栏里的 #锚点是百分号编码的；手敲坏了的编码不解，原样比。 */
function safeDecode(s: string) {
  try {
    return decodeURIComponent(s);
  } catch {
    return s;
  }
}

const SUGGESTIONS = ['等响', '真峰值', '重新聆听', '延迟', '单声道'];

export interface SearchDialogProps {
  open: boolean;
  onClose: () => void;
  /** 关掉后焦点回到这里（打开它的那个按钮，或按 ⌘K 时原本有焦点的元素） */
  returnFocus: RefObject<HTMLElement | null>;
}

/**
 * ⌘K / Ctrl K 全站搜索：命令面板式弹层。
 * 索引 = 说明与算法全部页面的标题、摘要 + 正文按 h2 切的小节（第一次打开时懒加载），结果深链到 /docs/xxx#小节。
 * 无障碍：role=dialog + aria-modal，页面其余部分 inert，Tab 在弹层里循环，Esc 关闭，关闭后焦点回到触发处；
 * 输入框是 combobox，↑↓ 移动 aria-activedescendant，Enter 打开（⌘ / Ctrl + Enter 新标签页）；输入法组字时的回车不算。
 * 页面滚动由导航（Nav）在打开期间锁住；开合动画用 GSAP，减少动效时直接到位。
 */
export default function SearchDialog({ open, onClose, returnFocus }: SearchDialogProps) {
  const root = useRef<HTMLDivElement>(null);
  const panel = useRef<HTMLDivElement>(null);
  const input = useRef<HTMLInputElement>(null);
  const list = useRef<HTMLDivElement>(null);
  const byKeyboard = useRef(false);
  const uid = useId();
  const listId = `${uid}-list`;
  const optionId = (i: number) => `${uid}-opt-${i}`;
  const navigate = useNavigate();
  const localize = useLocalePath();
  const location = useLocation();

  const [query, setQuery] = useState('');
  const [active, setActive] = useState(0);
  const [entries, setEntries] = useState<Entry[]>(pageEntries);
  /** 全部正文已到齐 */
  const [loaded, setLoaded] = useState(false);
  /** 正在拉正文 */
  const [pending, setPending] = useState(false);

  // 第一次打开时拉全部正文；没拉全（断网等）下次打开再补。
  useEffect(() => {
    if (!open || loaded) return;
    let alive = true;
    setPending(true);
    loadCorpus().then(({ entries: next, complete }) => {
      if (!alive) return;
      setEntries(next);
      setLoaded(complete);
      setPending(false);
    });
    return () => {
      alive = false;
    };
  }, [open, loaded]);

  const terms = useMemo(() => tokenize(query), [query]);
  const hits = useMemo(() => (terms.length ? rank(entries, terms) : null), [entries, terms]);
  const mark = useMemo(() => highlighter(terms), [terms]);
  const items = hits ?? pageEntries;
  const count = items.length;
  const activeIndex = count ? Math.min(active, count - 1) : -1;

  // 页面其余部分设为 inert（读屏与 Tab 都进不去）；关闭时解除。
  useLayoutEffect(() => {
    if (!open) return;
    const app = document.getElementById('root');
    if (app) app.inert = true;
    return () => {
      if (app) app.inert = false;
    };
  }, [open]);

  // 关闭后把焦点还给触发处。不能放在上面的 layout effect 里：它的清理跑在 React 提交中途，提交收尾时 React 会把焦点
  // 「恢复」到提交前的输入框上，随后弹层隐藏，焦点掉到 body。普通 effect 的清理在提交之后，这时 #root 也已解除 inert。
  useEffect(() => {
    if (!open) return;
    return () => {
      const back = returnFocus.current;
      if (back?.isConnected) back.focus({ preventScroll: true });
    };
  }, [open, returnFocus]);

  // 开合：遮罩淡入，面板从上方 12 px 落下并略放大到 1；关时 0.2 s 收起再隐藏。每次打开都把上次的词选中，直接打字即替换。
  useGSAP(
    () => {
      const el = root.current;
      const box = panel.current;
      if (!el || !box) return;
      const veil = el.querySelector('[data-veil]');
      const reduce = prefersReducedMotion();
      if (open) {
        gsap.set(el, { visibility: 'visible' });
        if (reduce) {
          gsap.set([veil, box], { opacity: 1, y: 0, scale: 1, overwrite: true });
        } else {
          gsap.fromTo(veil, { opacity: 0 }, { opacity: 1, duration: 0.35, ease: 'power2.out', overwrite: true });
          gsap.fromTo(box, { opacity: 0, y: -12, scale: 0.985 }, { opacity: 1, y: 0, scale: 1, duration: 0.5, ease: 'power3.out', overwrite: true });
        }
        input.current?.focus({ preventScroll: true });
        input.current?.select();
      } else if (reduce) {
        gsap.set(el, { visibility: 'hidden', overwrite: true });
      } else {
        gsap.to(veil, { opacity: 0, duration: 0.2, ease: 'power2.out', overwrite: true });
        gsap.to(box, { opacity: 0, y: -6, scale: 0.99, duration: 0.2, ease: 'power2.out', overwrite: true, onComplete: () => gsap.set(el, { visibility: 'hidden' }) });
      }
    },
    { dependencies: [open], scope: root }
  );

  // 焦点万一落到弹层外（比如 body），Esc 照样能关。
  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && !e.isComposing && !root.current?.contains(e.target as Node)) onClose();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  // 键盘移动时把选中项滚进可视区（鼠标悬停选中时不滚，免得列表在指针下乱跳）。optionId 只由 uid 决定，不必进依赖。
  useEffect(() => {
    if (!byKeyboard.current) return;
    byKeyboard.current = false;
    document.getElementById(optionId(activeIndex))?.scrollIntoView({ block: 'nearest' });
  }, [activeIndex]);

  const onQuery = (value: string) => {
    setQuery(value);
    setActive(0);
    list.current?.scrollTo({ top: 0 });
  };

  const pick = (entry: Entry, newTab: boolean, anchor?: HTMLAnchorElement | null) => {
    if (newTab) {
      if (anchor) window.open(anchor.href, '_blank', 'noopener');
      return;
    }
    const here = stripLocale(location.pathname).replace(/\/+$/, '') || '/';
    const sameSpot = here === entry.page.path && safeDecode(location.hash.slice(1)) === (entry.id ?? '');
    onClose();
    if (!sameSpot) {
      navigate(localize(hrefOf(entry)));
      return;
    }
    // 就在这一处：路由不会变，自己滚过去（等弹层解锁滚动之后）。
    window.setTimeout(() => {
      const behavior: ScrollBehavior = prefersReducedMotion() ? 'auto' : 'smooth';
      const target = entry.id ? document.getElementById(entry.id) : null;
      if (target) target.scrollIntoView({ behavior, block: 'start' });
      else window.scrollTo({ top: 0, behavior });
    }, 0);
  };

  const onRowClick = (e: ReactMouseEvent<HTMLAnchorElement>, entry: Entry) => {
    // ⌘ / Ctrl / Shift / 中键：交给浏览器开新标签，弹层不动。
    if (e.metaKey || e.ctrlKey || e.shiftKey || e.altKey || e.button !== 0) return;
    e.preventDefault();
    pick(entry, false);
  };

  const onHover = (i: number) => {
    byKeyboard.current = false;
    if (i !== activeIndex) setActive(i);
  };

  const trapTab = (e: ReactKeyboardEvent) => {
    const box = panel.current;
    if (!box) return;
    const nodes = [...box.querySelectorAll<HTMLElement>('input, button, [href]')].filter(n => n.tabIndex >= 0 && n.getClientRects().length > 0);
    if (!nodes.length) return;
    const first = nodes[0];
    const last = nodes[nodes.length - 1];
    const cur = document.activeElement;
    if (e.shiftKey && (cur === first || !box.contains(cur))) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (cur === last || !box.contains(cur))) {
      e.preventDefault();
      first.focus();
    }
  };

  const onKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    // 中文输入法组字时的方向键、回车、Esc 都是给输入法的（Safari 组字结束那一下 keyCode 是 229）。
    if (e.nativeEvent.isComposing || e.nativeEvent.keyCode === 229) return;
    if (e.key === 'Escape') {
      e.preventDefault();
      onClose();
      return;
    }
    if (e.key === 'Tab') {
      trapTab(e);
      return;
    }
    if (e.target !== input.current || !count) return;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      byKeyboard.current = true;
      setActive((activeIndex + (e.key === 'ArrowDown' ? 1 : -1) + count) % count);
    } else if (e.key === 'Enter' && activeIndex >= 0) {
      e.preventDefault();
      const anchor = document.getElementById(optionId(activeIndex))?.querySelector('a');
      pick(items[activeIndex], e.metaKey || e.ctrlKey, anchor);
    }
  };

  const status = pending ? '正在载入正文……' : hits ? `${hits.length} 条结果` : `说明 ${DOCS_COUNT} 页 · 算法 ${ALGO_COUNT} 页`;

  const row = (entry: Entry, i: number, browse: boolean) => (
    <ResultRow
      key={entry.key}
      id={optionId(i)}
      entry={entry}
      selected={i === activeIndex}
      title={browse ? entry.page.title : mark(entry.page.title)}
      section={entry.section === null ? null : mark(entry.section)}
      sub={browse ? entry.page.summary : mark(subline(entry, terms))}
      browse={browse}
      onHover={() => onHover(i)}
      onClick={e => onRowClick(e, entry)}
    />
  );

  return createPortal(
    <div ref={root} className="invisible fixed inset-0 z-50" onKeyDown={onKeyDown}>
      <div data-veil aria-hidden="true" className="absolute inset-0 bg-ink/20 backdrop-blur-[2px]" onClick={onClose} />
      <div
        ref={panel}
        role="dialog"
        aria-modal="true"
        aria-label="搜索说明与算法"
        tabIndex={-1}
        className="relative mx-3 mt-3 flex max-h-[calc(100dvh-1.5rem)] flex-col overflow-hidden rounded-[1.25rem] bg-panel shadow-glaze ring-1 ring-ink/8 outline-none sm:mx-auto sm:mt-[12vh] sm:max-h-[min(38rem,76vh)] sm:w-[min(40rem,calc(100vw-4rem))]"
      >
        <div className="flex items-center gap-3 border-b border-line pl-4 pr-3 sm:pl-5">
          <SearchGlyph className="size-[18px] shrink-0 text-ink-3" />
          <input
            ref={input}
            type="text"
            role="combobox"
            aria-expanded={count > 0}
            aria-controls={listId}
            aria-autocomplete="list"
            aria-activedescendant={activeIndex >= 0 ? optionId(activeIndex) : undefined}
            aria-label="搜索说明与算法"
            placeholder="搜索说明与算法……"
            autoComplete="off"
            autoCapitalize="off"
            spellCheck={false}
            enterKeyHint="go"
            value={query}
            onChange={e => onQuery(e.target.value)}
            className="h-14 min-w-0 flex-1 bg-transparent text-[17px] text-ink outline-none placeholder:text-ink-4"
          />
          {query ? (
            <button
              type="button"
              aria-label="清空"
              onClick={() => {
                onQuery('');
                input.current?.focus();
              }}
              className="grid size-7 shrink-0 place-items-center rounded-full text-ink-4 transition-colors duration-300 hover:bg-ink/5 hover:text-ink"
            >
              <svg viewBox="0 0 16 16" className="size-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" aria-hidden="true">
                <path d="M4 4l8 8M12 4l-8 8" />
              </svg>
            </button>
          ) : null}
          <button
            type="button"
            aria-label="关闭搜索"
            onClick={onClose}
            className="shrink-0 rounded-md px-2 py-1 text-[12px] text-ink-3 ring-1 ring-ink/10 transition-colors duration-300 hover:bg-ink/5 hover:text-ink"
          >
            <span className="hidden sm:inline">esc</span>
            <span className="sm:hidden">取消</span>
          </button>
        </div>

        {hits !== null && hits.length === 0 ? (
          <div className="px-6 py-12 text-center">
            {!pending ? (
              <>
                <p className="font-serif text-[18px] text-ink">没找到「{query.trim()}」</p>
                <p className="mt-2 text-[13px] text-ink-3">换个说法，或者试试这几个：</p>
                <div className="mt-5 flex flex-wrap justify-center gap-2">
                  {SUGGESTIONS.map(s => (
                    <button
                      key={s}
                      type="button"
                      onClick={() => {
                        onQuery(s);
                        input.current?.focus();
                      }}
                      className="rounded-full px-3 py-1 text-[13px] text-ink-2 ring-1 ring-ink/10 transition-colors duration-300 hover:bg-ink/5 hover:text-ink"
                    >
                      {s}
                    </button>
                  ))}
                </div>
              </>
            ) : (
              <p className="text-[13px] text-ink-3">正在载入正文……</p>
            )}
          </div>
        ) : (
          <div ref={list} id={listId} role="listbox" aria-label={hits ? '搜索结果' : '全部页面'} className="min-h-0 flex-1 overflow-y-auto overscroll-contain p-2">
            {hits
              ? hits.map((entry, i) => row(entry, i, false))
              : browseGroups.map(g => (
                  <div key={g.kind} role="group" aria-labelledby={`${uid}-${g.kind}`}>
                    <div id={`${uid}-${g.kind}`} role="presentation" className="px-3 pb-1.5 pt-3 text-[11px] tracking-[0.22em] text-ink-4">
                      {g.label}
                    </div>
                    {g.rows.map(({ entry, index }) => row(entry, index, true))}
                  </div>
                ))}
          </div>
        )}

        <div className="flex items-center justify-between gap-4 border-t border-line px-4 py-2.5 text-[11.5px] text-ink-4 sm:px-5">
          <div className="hidden items-center gap-4 sm:flex">
            <span className="flex items-center gap-1.5">
              <Kbd>↑</Kbd>
              <Kbd>↓</Kbd>
              选择
            </span>
            <span className="flex items-center gap-1.5">
              <Kbd>↵</Kbd>
              打开
            </span>
            <span className="flex items-center gap-1.5">
              <Kbd>esc</Kbd>
              关闭
            </span>
          </div>
          <span className="tabular-nums">{status}</span>
        </div>
      </div>
      <p className="sr-only" aria-live="polite">
        {hits ? `${hits.length} 条结果` : ''}
      </p>
    </div>,
    document.body
  );
}

function ResultRow({
  id,
  entry,
  selected,
  title,
  section,
  sub,
  browse,
  onHover,
  onClick
}: {
  id: string;
  entry: Entry;
  selected: boolean;
  title: ReactNode;
  section: ReactNode | null;
  sub: ReactNode;
  browse: boolean;
  onHover: () => void;
  onClick: (e: ReactMouseEvent<HTMLAnchorElement>) => void;
}) {
  return (
    <div id={id} role="option" aria-selected={selected} onMouseMove={onHover} className={cn('rounded-xl transition-colors duration-200 ease-glaze', selected && 'bg-c100/40')}>
      <LocaleLink to={hrefOf(entry)} tabIndex={-1} onClick={onClick} className="flex items-start gap-4 px-3 py-2.5 no-underline">
        <span className="min-w-0 flex-1">
          <span className={cn('block font-serif text-[15px] leading-snug', selected ? 'text-c800' : 'text-ink')}>
            {title}
            {section !== null ? (
              <>
                <span className="mx-1.5 text-ink-4">·</span>
                <span className={selected ? 'text-c800' : 'text-ink-2'}>{section}</span>
              </>
            ) : null}
          </span>
          <span className={cn('mt-1 text-[13px] leading-[1.65] text-ink-3', browse ? 'block truncate' : 'line-clamp-2')}>{sub}</span>
        </span>
        <span className="shrink-0 pt-0.5 text-[11px] tracking-[0.18em] text-ink-4">{entry.page.kind === 'docs' ? '说明' : '算法'}</span>
      </LocaleLink>
    </div>
  );
}

function Kbd({ children }: { children: ReactNode }) {
  return <kbd className="inline-grid min-w-5 place-items-center rounded-[5px] bg-paper px-1 font-sans text-[11px] leading-[18px] text-ink-3 ring-1 ring-ink/10">{children}</kbd>;
}

function SearchGlyph({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 20 20" className={className} fill="none" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" aria-hidden="true">
      <circle cx="8.75" cy="8.75" r="5.75" />
      <path d="m13 13 4.25 4.25" />
    </svg>
  );
}
