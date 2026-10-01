import type { Locale } from '@/i18n/paths';

/*
  使用说明与公开算法的结构（ADR-024 双语）：这里只有路由末段、Markdown 文件名与侧栏分组的键；
  页面标题、摘要、分组名在 i18n 的 docs 命名空间（pages.<kind>.<file>、groups.<键>），组件里用 usePageText 取当前语言的；
  正文是 content/<kind>/<语言>/<file>.md，两种语言文件名相同（scripts/check-i18n.mjs 核对成对）。
*/
export type Kind = 'docs' | 'algorithms';
export type GroupKey = 'start' | 'process' | 'judge' | 'overview' | 'stages' | 'orchestration';

export interface PageMeta {
  /** 路由末段；'' = 该分区首页 */
  slug: string;
  /** Markdown 文件名（不含扩展名），也是 docs 命名空间里 pages.<kind>.<file> 的键 */
  file: string;
}
export interface NavGroup {
  group: GroupKey;
  pages: PageMeta[];
}

export const docsNav: NavGroup[] = [
  {
    group: 'start',
    pages: [
      { slug: '', file: 'quickstart' },
      { slug: 'install', file: 'install' },
      { slug: 'interface', file: 'interface' }
    ]
  },
  {
    group: 'process',
    pages: [
      { slug: 'glaze', file: 'glaze' },
      { slug: 'balance', file: 'balance' },
      { slug: 'clay', file: 'clay' },
      { slug: 'color', file: 'color' },
      { slug: 'limit', file: 'limit' },
      { slug: 'advanced', file: 'advanced' }
    ]
  },
  {
    group: 'judge',
    pages: [
      { slug: 'listening', file: 'listening' },
      { slug: 'meters', file: 'meters' },
      { slug: 'controls', file: 'controls' },
      { slug: 'faq', file: 'faq' }
    ]
  }
];

export const algoNav: NavGroup[] = [
  { group: 'overview', pages: [{ slug: '', file: 'overview' }] },
  {
    group: 'stages',
    pages: [
      { slug: 'balance', file: 'balance' },
      { slug: 'clay', file: 'clay' },
      { slug: 'color', file: 'color' },
      { slug: 'limit', file: 'limit' }
    ]
  },
  {
    group: 'orchestration',
    pages: [
      { slug: 'glaze', file: 'glaze' },
      { slug: 'verification', file: 'verification' }
    ]
  }
];

export function flatten(nav: NavGroup[]): PageMeta[] {
  return nav.flatMap(g => g.pages);
}

// Markdown 正文按需加载（每页每种语言一个小包）。
const docsFiles = import.meta.glob('./docs/*/*.md', { query: '?raw', import: 'default' }) as Record<string, () => Promise<string>>;
const algoFiles = import.meta.glob('./algorithms/*/*.md', { query: '?raw', import: 'default' }) as Record<string, () => Promise<string>>;

export function loadMarkdown(kind: Kind, file: string, locale: Locale): Promise<string> {
  const table = kind === 'docs' ? docsFiles : algoFiles;
  const loader = table[`./${kind}/${locale}/${file}.md`];
  if (!loader) return Promise.reject(new Error(`${kind}/${locale}/${file}.md`));
  return loader();
}

// —— 以下供全站搜索（components/docs/Search）与本页目录（components/docs/Toc）使用 ——

/**
 * 标题锚点 id。说明页 h2 / h3 的 id、搜索结果的深链、本页目录三处共用这一个函数，链接才一定对得上。
 * 中文照留（中文页的锚点是中文），英文转小写；标点与空白折成一个「-」。
 */
export function slugify(text: string) {
  return text
    .trim()
    .toLowerCase()
    .replace(/[`*_~]/g, '')
    .replace(/[^\p{L}\p{N}]+/gu, '-')
    .replace(/^-+|-+$/g, '');
}

/** 可检索的一页：在 PageMeta 之外带上分区、侧栏分组与站内路径（标题与摘要按语言另取）。 */
export interface SitePage extends PageMeta {
  kind: Kind;
  group: GroupKey;
  /** 站内路径（不带 # 与语言前缀），如 /docs、/docs/install */
  path: string;
}

function sitePages(nav: NavGroup[], kind: Kind, base: string): SitePage[] {
  return nav.flatMap(g => g.pages.map(p => ({ ...p, kind, group: g.group, path: p.slug ? `${base}/${p.slug}` : base })));
}

/** 说明 + 算法的全部页面，按侧栏顺序。 */
export const allPages: SitePage[] = [...sitePages(docsNav, 'docs', '/docs'), ...sitePages(algoNav, 'algorithms', '/algorithms')];

const ENTITIES: Record<string, string> = { '&amp;': '&', '&lt;': '<', '&gt;': '>', '&quot;': '"', '&#39;': "'", '&nbsp;': ' ' };

/**
 * Markdown 行内标记 → 纯文本，与 react-markdown 渲染后取文字的结果一致：
 * 强调、行内代码、链接只留文字；图片默认留说明文字（标题里的图片不算字，传 'drop'）。
 */
export function inlineText(md: string, images: 'alt' | 'drop' = 'alt'): string {
  return md
    .replace(/!\[([^\]]*)\]\([^)]*\)/g, images === 'alt' ? '$1' : '')
    .replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
    .replace(/<\/?[a-zA-Z][^>]*>/g, '')
    .replace(/(`+)(.+?)\1/g, '$2')
    .replace(/(\*\*|__)(.+?)\1/g, '$2')
    .replace(/~~(.+?)~~/g, '$1')
    .replace(/(^|[^\w*])\*(\S(?:.*?\S)?)\*(?!\w)/g, '$1$2')
    .replace(/(^|\W)_(\S(?:.*?\S)?)_(?!\w)/g, '$1$2')
    .replace(/&(?:amp|lt|gt|quot|#39|nbsp);/g, m => ENTITIES[m] ?? m)
    .replace(/\\([!-/:-@[-`{-~])/g, '$1');
}

/** 块级的一行 → 纯文本：去掉引用、列表、h3～h6 的记号；表格行把单元格用「·」隔开，分隔行丢掉。 */
function blockLineText(line: string): string {
  let s = line.replace(/^\s{0,3}(?:>\s?)+/, '');
  if (/^\s*([-*_])(?:\s*\1){2,}\s*$/.test(s)) return '';
  if (/^\s*\|/.test(s)) {
    if (/^\s*\|?(?:\s*:?-+:?\s*\|)+\s*(?::?-+:?\s*)?$/.test(s)) return '';
    s = s.replace(/^\s*\|/, '').replace(/\|\s*$/, '').replace(/\s*(?<!\\)\|\s*/g, ' · ');
  }
  s = s.replace(/^\s*(?:[-*+]|\d+[.)])\s+/, '').replace(/^\s{0,3}#{1,6}\s+(.*?)(?:\s+#+)?\s*$/, '$1');
  return inlineText(s).trim();
}

/** 按 h2 切出的一段。 */
export interface MdSection {
  /** h2 的锚点 id；第一个 h2 之前的导语段为 null */
  id: string | null;
  /** h2 标题的纯文本；导语段为 null */
  title: string | null;
  /** 这一段的纯文本：正文、h3 标题、列表、表格、示意图说明，拼成一行 */
  text: string;
}

const FENCE = /^\s{0,3}(`{3,}|~{3,})/;
const H2 = /^\s{0,3}##[ \t]+(.+?)(?:[ \t]+#+)?[ \t]*$/;

/** 把一页 Markdown 按 h2 切段（围栏代码块里的 # 不算标题）。说明与算法页只用 ATX 标题（## 标题），不用下划线式。 */
export function mdSections(md: string): MdSection[] {
  const out: MdSection[] = [];
  let cur: { id: string | null; title: string | null; lines: string[] } = { id: null, title: null, lines: [] };
  const flush = () => {
    const text = cur.lines.filter(Boolean).join(' ').replace(/\s+/g, ' ').trim();
    if (cur.id !== null || text) out.push({ id: cur.id, title: cur.title, text });
  };
  let fence: string | null = null;
  for (const line of md.split(/\r?\n/)) {
    const f = FENCE.exec(line);
    if (fence) {
      if (f && f[1][0] === fence[0] && f[1].length >= fence.length) fence = null;
      else cur.lines.push(line.trim());
      continue;
    }
    if (f) {
      fence = f[1];
      continue;
    }
    const h = H2.exec(line);
    if (h) {
      flush();
      const title = inlineText(h[1], 'drop').trim();
      cur = { id: slugify(title), title, lines: [] };
      continue;
    }
    cur.lines.push(blockLineText(line));
  }
  flush();
  return out;
}

/** 本页目录：一页里全部 h2 的 id 与标题（与 Markdown 组件渲染出的 h2 id 一致）。 */
export function mdHeadings(md: string): { id: string; title: string }[] {
  return mdSections(md).flatMap(s => (s.id !== null && s.title !== null ? [{ id: s.id, title: s.title }] : []));
}
