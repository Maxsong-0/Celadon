import { useEffect, useMemo, useState } from 'react';
import { Link, useLocation, useParams } from 'react-router-dom';
import Markdown from '@/components/docs/Markdown';
import { Eyebrow } from '@/components/celadon/Eyebrow';
import { algoNav, docsNav, flatten, loadMarkdown, type PageMeta } from '@/content';
import { cn } from '@/lib/utils';

export default function DocsLayout({ kind }: { kind: 'docs' | 'algorithms' }) {
  const nav = kind === 'docs' ? docsNav : algoNav;
  const base = kind === 'docs' ? '/docs' : '/algorithms';
  const label = kind === 'docs' ? '使用说明' : '公开算法';
  const params = useParams();
  const slug = (params['*'] ?? '').replace(/\/+$/, '');
  const pages = useMemo(() => flatten(nav), [nav]);
  const index = pages.findIndex(p => p.slug === slug);
  const page: PageMeta | undefined = pages[index];
  const prev = index > 0 ? pages[index - 1] : undefined;
  const next = index >= 0 && index < pages.length - 1 ? pages[index + 1] : undefined;
  const { hash } = useLocation();

  const [source, setSource] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

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

  useEffect(() => {
    document.title = page ? `${page.title} · ${label} · Celadon` : `${label} · Celadon`;
  }, [page, label]);

  useEffect(() => {
    if (!source || !hash) return;
    const id = decodeURIComponent(hash.slice(1));
    const t = window.setTimeout(() => document.getElementById(id)?.scrollIntoView({ behavior: 'smooth', block: 'start' }), 60);
    return () => window.clearTimeout(t);
  }, [source, hash]);

  return (
    <main className="mx-auto max-w-[1280px] px-5 sm:px-8 pt-28 sm:pt-32 pb-24">
      <div className="grid gap-10 lg:grid-cols-12">
        <aside className="lg:col-span-3">
          <div className="lg:sticky lg:top-28">
            <Eyebrow>{label}</Eyebrow>
            <nav aria-label={label} className="mt-6">
              {/* 手机：一行胶囊横向滚 */}
              <div className="flex gap-2 overflow-x-auto pb-3 lg:hidden -mx-5 px-5">
                {pages.map(p => (
                  <Link
                    key={p.slug}
                    to={`${base}/${p.slug}`}
                    className={cn('shrink-0 rounded-full px-3.5 py-1.5 text-[13px] no-underline ring-1 transition-colors', p.slug === slug ? 'bg-ink text-paper ring-ink' : 'bg-white/60 text-ink-2 ring-ink/8 hover:bg-white')}
                  >
                    {p.title}
                  </Link>
                ))}
              </div>
              {/* 桌面：分组列表 */}
              <div className="hidden lg:block space-y-7">
                {nav.map(g => (
                  <div key={g.group}>
                    <div className="text-[10px] uppercase tracking-[0.22em] text-ink-4 mb-3">{g.group}</div>
                    <ul className="space-y-0.5 border-l border-line">
                      {g.pages.map(p => {
                        const active = p.slug === slug;
                        return (
                          <li key={p.slug}>
                            <Link
                              to={`${base}/${p.slug}`}
                              className={cn(
                                'relative block -ml-px border-l py-1.5 pl-4 text-[13.5px] no-underline transition-colors duration-300',
                                active ? 'border-c600 text-ink font-medium' : 'border-transparent text-ink-3 hover:text-ink hover:border-line-2'
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
            <div className="hidden lg:block mt-10 text-[12px] leading-relaxed text-ink-4">
              {kind === 'docs' ? (
                <>
                  文案与插件里的悬停提示同源（`Source/ui/HelpText.cpp`）。想看算法：<Link to="/algorithms" className="text-c700 underline decoration-c300">公开算法</Link>。
                </>
              ) : (
                <>
                  只公开算法描述、结构与常数，不贴源码。想看怎么用：<Link to="/docs" className="text-c700 underline decoration-c300">使用说明</Link>。
                </>
              )}
            </div>
          </div>
        </aside>

        <article className="lg:col-span-8 lg:col-start-5 min-w-0">
          {!page ? (
            <div className="prose-celadon">
              <h1>没有这一页</h1>
              <p>
                回到 <Link to={base}>{label}</Link> 首页。
              </p>
            </div>
          ) : (
            <>
              <header className="mb-10">
                <div className="text-[11px] uppercase tracking-[0.22em] text-c600">{label}</div>
                <h1 className="mt-3 font-serif text-[clamp(2rem,4vw,3.2rem)] font-semibold leading-[1.12] tracking-tight text-ink">{page.title}</h1>
                <p className="mt-4 text-[16px] leading-relaxed text-ink-3">{page.summary}</p>
                <div className="hairline mt-8" />
              </header>
              {error ? <p className="warn">{error}</p> : null}
              {source === null && !error ? <Skeleton /> : null}
              {source ? <Markdown source={source} /> : null}
              <nav className="mt-20 grid gap-4 sm:grid-cols-2" aria-label="上一页 / 下一页">
                {prev ? (
                  <Link to={`${base}/${prev.slug}`} className="bezel no-underline group">
                    <div className="bezel-core p-5">
                      <div className="text-[10px] uppercase tracking-[0.2em] text-ink-4">← 上一页</div>
                      <div className="mt-1.5 font-serif text-lg font-semibold text-ink group-hover:text-c700 transition-colors">{prev.title}</div>
                    </div>
                  </Link>
                ) : (
                  <span />
                )}
                {next ? (
                  <Link to={`${base}/${next.slug}`} className="bezel no-underline group sm:text-right">
                    <div className="bezel-core p-5">
                      <div className="text-[10px] uppercase tracking-[0.2em] text-ink-4">下一页 →</div>
                      <div className="mt-1.5 font-serif text-lg font-semibold text-ink group-hover:text-c700 transition-colors">{next.title}</div>
                    </div>
                  </Link>
                ) : null}
              </nav>
            </>
          )}
        </article>
      </div>
    </main>
  );
}

function Skeleton() {
  return (
    <div className="space-y-4 animate-pulse" aria-hidden="true">
      {[92, 100, 78, 96, 60, 88, 100, 70].map((w, i) => (
        <div key={i} className="h-3.5 rounded-full bg-paper-3" style={{ width: `${w}%` }} />
      ))}
    </div>
  );
}
