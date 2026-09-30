import { useEffect, useMemo, useRef, useState } from 'react';
import { Link, useLocation, useParams } from 'react-router-dom';
import Markdown from '@/components/docs/Markdown';
import Toc from '@/components/docs/Toc';
import { Eyebrow } from '@/components/celadon/Eyebrow';
import { algoNav, docsNav, flatten, loadMarkdown, mdHeadings, type PageMeta } from '@/content';
import { cn, prefersReducedMotion } from '@/lib/utils';

/**
 * 说明页 / 算法页的版式：左侧栏（分区导航）· 正文 · 右侧本页目录（xl 以上）。
 * 正文列在 lg 以上约 44 rem（15.5 px 正文约 44 字一行），加上目录列也不会窄过 34 em 的阅读下限；lg 以下目录收起。
 */
export default function DocsLayout({ kind }: { kind: 'docs' | 'algorithms' }) {
  const nav = kind === 'docs' ? docsNav : algoNav;
  const base = kind === 'docs' ? '/docs' : '/algorithms';
  const label = kind === 'docs' ? '使用说明' : '公开算法';
  const params = useParams();
  const slug = (params['*'] ?? '').replace(/\/+$/, '');
  const pages = useMemo(() => flatten(nav), [nav]);
  const index = pages.findIndex(p => p.slug === slug);
  const page: PageMeta | undefined = pages[index];
  const group = page ? nav.find(g => g.pages.includes(page))?.group : undefined;
  const prev = index > 0 ? pages[index - 1] : undefined;
  const next = index >= 0 && index < pages.length - 1 ? pages[index + 1] : undefined;
  const { pathname, hash } = useLocation();

  const [source, setSource] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const headings = useMemo(() => (source ? mdHeadings(source) : []), [source]);
  const pills = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!page) return;
    let alive = true;
    setSource(null);
    setError(null);
    loadMarkdown(kind, page.file)
      .then(s => alive && setSource(s))
      .catch(e => alive && setError(String(e)));
    return () => {
      alive = false;
    };
  }, [kind, page]);

  // 标题与摘要随页面换；导航（Nav）在每次换路径时先把它们恢复成站点默认，这里再覆盖成本页的。
  useEffect(() => {
    document.title = page ? `${page.title} · ${label} · Celadon` : `${label} · Celadon`;
    if (page) document.querySelector('meta[name="description"]')?.setAttribute('content', `${page.title}：${page.summary}（Celadon 青瓷 · ${label}）`);
  }, [page, label, pathname]);

  useEffect(() => {
    if (!source || !hash) return;
    let id = hash.slice(1);
    try {
      id = decodeURIComponent(id);
    } catch {
      // 手敲坏了的百分号编码：原样找，找不到就不滚（别让整页因为一个锚点报错）
    }
    const t = window.setTimeout(
      () => document.getElementById(id)?.scrollIntoView({ behavior: prefersReducedMotion() ? 'auto' : 'smooth', block: 'start' }),
      60
    );
    return () => window.clearTimeout(t);
  }, [source, hash]);

  // 手机上的横排页签：把当前页滚到中间，排在后面的页（如「常见问题」）打开时也看得见自己在哪。
  useEffect(() => {
    const row = pills.current;
    const pill = row?.querySelector<HTMLElement>('[aria-current="page"]');
    if (!row || !pill || row.scrollWidth <= row.clientWidth) return;
    row.scrollTo({ left: pill.offsetLeft - (row.clientWidth - pill.offsetWidth) / 2 });
  }, [slug]);

  return (
    <main className="mx-auto max-w-[1280px] px-5 sm:px-8 pt-28 sm:pt-32 pb-24">
      <div className="grid gap-10 lg:grid-cols-[13.5rem_minmax(0,1fr)] lg:gap-14 xl:grid-cols-[13.5rem_minmax(0,1fr)_12rem] xl:gap-16">
        <aside className="min-w-0">
          {/* 矮屏（笔记本 768 高）上侧栏比视口长时自己滚，不把底下几项藏起来 */}
          <div className="lg:sticky lg:top-28 lg:-mx-1 lg:max-h-[calc(100dvh-8.5rem)] lg:overflow-y-auto lg:overscroll-contain lg:px-1 lg:pb-6 lg:[scrollbar-width:thin]">
            <Eyebrow>{label}</Eyebrow>
            <nav aria-label={label} className="mt-6">
              {/* 手机：一行页签横向滚 */}
              <div ref={pills} className="relative -mx-5 flex gap-2 overflow-x-auto px-5 pb-3 [scrollbar-width:none] sm:-mx-8 sm:px-8 lg:hidden">
                {pages.map(p => {
                  const active = p.slug === slug;
                  return (
                    <Link
                      key={p.slug}
                      to={p.slug ? `${base}/${p.slug}` : base}
                      aria-current={active ? 'page' : undefined}
                      className={cn(
                        'shrink-0 rounded-full px-3.5 py-1.5 text-[13px] no-underline ring-1 transition-colors duration-300 ease-glaze',
                        active ? 'bg-ink text-paper ring-ink' : 'bg-panel/70 text-ink-2 ring-ink/8 hover:bg-panel'
                      )}
                    >
                      {p.title}
                    </Link>
                  );
                })}
              </div>
              {/* 桌面：分组列表 */}
              <div className="hidden lg:block space-y-7">
                {nav.map(g => (
                  <div key={g.group}>
                    <div className="mb-3 text-[11px] tracking-[0.22em] text-ink-4">{g.group}</div>
                    <ul className="space-y-0.5 border-l border-line">
                      {g.pages.map(p => {
                        const active = p.slug === slug;
                        return (
                          <li key={p.slug}>
                            <Link
                              to={p.slug ? `${base}/${p.slug}` : base}
                              aria-current={active ? 'page' : undefined}
                              className={cn(
                                'relative -ml-px block border-l py-1.5 pl-4 text-[13.5px] no-underline transition-colors duration-300 ease-glaze',
                                active ? 'border-c600 font-medium text-ink' : 'border-transparent text-ink-3 hover:border-line-2 hover:text-ink'
                              )}
                            >
                              {p.title}
                            </Link>
                          </li>
                        );
                      })}
                    </ul>
                  </div>
                ))}
              </div>
            </nav>
            <p className="mt-10 hidden text-[12px] leading-relaxed text-ink-4 lg:block">
              {kind === 'docs' ? (
                <>
                  说明文字与插件里的悬停提示一致。想看算法：<Link to="/algorithms" className="text-c700 underline decoration-c300">公开算法</Link>。
                </>
              ) : (
                <>
                  只公开算法描述、结构与常数，不贴源码。想看怎么用：<Link to="/docs" className="text-c700 underline decoration-c300">使用说明</Link>。
                </>
              )}
            </p>
          </div>
        </aside>

        <article className="min-w-0 lg:max-w-[44rem]">
          {!page ? (
            <header>
              <p className="text-[12px] tracking-[0.2em] text-c700">{label}</p>
              <h1 className="mt-4 font-serif text-[clamp(2rem,4vw,3.2rem)] font-medium leading-[1.12] tracking-tight text-ink">没有这一页</h1>
              <p className="mt-5 text-[17px] leading-[1.75] text-ink-2">
                地址可能改过了。回到<Link to={base} className="mx-1 text-c700 underline decoration-c300 underline-offset-4">{label}</Link>首页，或按 ⌘K / Ctrl K 搜一下。
              </p>
            </header>
          ) : (
            <>
              <header className="mb-12">
                <p className="text-[12px] tracking-[0.2em] text-c700">
                  {label}
                  {group ? <span className="text-ink-4"> · {group}</span> : null}
                </p>
                <h1 className="mt-4 font-serif text-[clamp(2rem,4vw,3.2rem)] font-medium leading-[1.12] tracking-tight text-ink">{page.title}</h1>
                <p className="mt-5 max-w-[var(--measure)] text-[17px] leading-[1.75] text-ink-2">{page.summary}</p>
                <div className="hairline mt-10" />
              </header>
              {error ? (
                <p className="rounded-2xl bg-warn-soft px-5 py-4 text-[15px] leading-relaxed text-ink-2 ring-1 ring-warn/25">这一页没载入：{error}。刷新再试一次。</p>
              ) : null}
              {source === null && !error ? <Skeleton /> : null}
              {source ? <Markdown source={source} /> : null}
              <nav className="mt-24 grid gap-3 sm:grid-cols-2" aria-label="上一页与下一页">
                {prev ? <Pager to={prev.slug ? `${base}/${prev.slug}` : base} dir="prev" title={prev.title} /> : <span className="hidden sm:block" aria-hidden="true" />}
                {next ? <Pager to={next.slug ? `${base}/${next.slug}` : base} dir="next" title={next.title} /> : null}
              </nav>
            </>
          )}
        </article>

        {headings.length > 1 && page ? <Toc key={`${kind}/${page.file}`} items={headings} className="hidden xl:block" /> : null}
      </div>
    </main>
  );
}

/** 上一页 / 下一页：素坯（次要内容），不用瓷板。 */
function Pager({ to, dir, title }: { to: string; dir: 'prev' | 'next'; title: string }) {
  const next = dir === 'next';
  return (
    <Link
      to={to}
      rel={dir}
      className={cn(
        'biscuit group block px-6 py-5 no-underline transition-[background-color,box-shadow] duration-500 ease-glaze hover:bg-panel hover:ring-ink/12',
        next && 'sm:text-right'
      )}
    >
      <span className="block text-[12px] tracking-[0.2em] text-ink-4">{next ? '下一页' : '上一页'}</span>
      <span className={cn('mt-2 flex items-center gap-2.5 font-serif text-[19px] leading-snug text-ink transition-colors duration-300 group-hover:text-c700', next && 'sm:justify-end')}>
        {next ? null : <Arrow className="rotate-180 group-hover:-translate-x-0.5" />}
        {title}
        {next ? <Arrow className="group-hover:translate-x-0.5" /> : null}
      </span>
    </Link>
  );
}

function Arrow({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 16 16" className={cn('size-3.5 shrink-0 text-ink-4 transition-transform duration-500 ease-glaze group-hover:text-c600', className)} fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M3 8h10M9 4l4 4-4 4" />
    </svg>
  );
}

/** 正文载入前的占位：静态的几条灰线（不做脉动循环，设计规格-官网 第 1 节第 6 条）。 */
function Skeleton() {
  return (
    <div className="space-y-4" aria-hidden="true">
      {[92, 100, 78, 96, 60, 88, 100, 70].map((w, i) => (
        <div key={i} className="h-3.5 rounded-full bg-paper-3" style={{ width: `${w}%` }} />
      ))}
    </div>
  );
}
