export interface PageMeta {
  /** 路由末段；'' = 该分区首页 */
  slug: string;
  title: string;
  summary: string;
  /** Markdown 文件名（不含扩展名） */
  file: string;
}
export interface NavGroup {
  group: string;
  pages: PageMeta[];
}

export const docsNav: NavGroup[] = [
  {
    group: '开始',
    pages: [
      { slug: '', title: '快速上手', summary: '插到总轨、播放、拧 GLAZE——三步。', file: 'quickstart' },
      { slug: 'install', title: '安装与兼容', summary: 'macOS（Apple Silicon / Intel）、Windows x64、宿主、延迟。', file: 'install' },
      { slug: 'interface', title: '界面总览', summary: '顶栏、表、频谱仪、GLAZE、四张卡片各是什么。', file: 'interface' }
    ]
  },
  {
    group: '处理',
    pages: [
      { slug: 'glaze', title: 'GLAZE 与风格', summary: '大旋钮的含义、四种风格、聆听与重新聆听。', file: 'glaze' },
      { slug: 'balance', title: 'BALANCE 卡片', summary: '回补、抑制、提亮、速度与 27 段修正曲线。', file: 'balance' },
      { slug: 'clay', title: 'CLAY 卡片', summary: '低切、收单、加宽各自的自动 / 关 / 手动。', file: 'clay' },
      { slug: 'color', title: 'COLOR 卡片', summary: '音调、粘合、热度、干湿、输出。', file: 'color' },
      { slug: 'limit', title: 'LIMIT 卡片', summary: '增益、上限、削波、释放、预读与试听差值。', file: 'limit' },
      { slug: 'advanced', title: '高级面板', summary: '推响不刺、目标频谱与参考学习、发行安全。', file: 'advanced' }
    ]
  },
  {
    group: '判断',
    pages: [
      { slug: 'listening', title: '监听方式', summary: 'A / B、等响、试听差值、归一化预听。', file: 'listening' },
      { slug: 'meters', title: '表与频谱仪', summary: '每个读数是什么、该看多少。', file: 'meters' },
      { slug: 'controls', title: '手感与键位', summary: '拖动、精调、双击、直接输入、缩放、语言。', file: 'controls' },
      { slug: 'faq', title: '常见问题', summary: '声音没变、校准丢了、宿主没看到新版本……', file: 'faq' }
    ]
  }
];

export const algoNav: NavGroup[] = [
  {
    group: '总览',
    pages: [
      { slug: '', title: '信号链与设计原则', summary: '固定顺序、延迟补偿旁通、零差、逐位契约、真峰值口径。', file: 'overview' }
    ]
  },
  {
    group: '四道工序',
    pages: [
      { slug: 'balance', title: 'BALANCE · 动态频谱平衡', summary: '27 段、参考轮廓、死区、自动许可、上行掩蔽转移。', file: 'balance' },
      { slug: 'clay', title: 'CLAY · 修坯', summary: 'M/S 三级、先试后用、峰值守护。', file: 'clay' },
      { slug: 'color', title: 'COLOR · 染色', summary: 'Tone、分频 Glue、保留低频主体的 Heat。', file: 'color' },
      { slug: 'limit', title: 'LIMIT · 真峰值限幅', summary: '8× 检测 1× 施加、双包络、三角攻击沿、周期释放。', file: 'limit' }
    ]
  },
  {
    group: '编排与验证',
    pages: [
      { slug: 'glaze', title: 'GLAZE · 决策与校准', summary: '画像 → 方案 → 校准 → 保护；存档与重开。', file: 'glaze' },
      { slug: 'verification', title: '性能与验证', summary: '预算口径、实测数字、测试与逐位契约的编译前提。', file: 'verification' }
    ]
  }
];

export function flatten(nav: NavGroup[]): PageMeta[] {
  return nav.flatMap(g => g.pages);
}

// Markdown 正文按需加载（每页一个小包）。
const docsFiles = import.meta.glob('./docs/*.md', { query: '?raw', import: 'default' }) as Record<string, () => Promise<string>>;
const algoFiles = import.meta.glob('./algorithms/*.md', { query: '?raw', import: 'default' }) as Record<string, () => Promise<string>>;

export function loadMarkdown(kind: 'docs' | 'algorithms', file: string): Promise<string> {
  const table = kind === 'docs' ? docsFiles : algoFiles;
  const loader = table[`./${kind}/${file}.md`];
  if (!loader) return Promise.reject(new Error(`没有这一页：${kind}/${file}`));
  return loader();
}

// —— 以下供全站搜索（components/docs/Search）与本页目录（components/docs/Toc）使用 ——

/**
 * 标题锚点 id。说明页 h2 / h3 的 id、搜索结果的深链、本页目录三处共用这一个函数，链接才一定对得上。
 * 中文照留，标点与空白折成一个「-」。
 */
export function slugify(text: string) {
  return text
    .trim()
    .toLowerCase()
    .replace(/[`*_~]/g, '')
    .replace(/[^\p{L}\p{N}]+/gu, '-')
    .replace(/^-+|-+$/g, '');
}

/** 可检索的一页：在 PageMeta 之外带上分区、侧栏分组与站内路径。 */
export interface SitePage extends PageMeta {
  kind: 'docs' | 'algorithms';
  /** 分区名：使用说明 / 公开算法 */
  kindLabel: string;
  /** 侧栏分组：开始 / 处理 / 判断 / 总览 …… */
  group: string;
  /** 站内路径（不带 #），如 /docs、/docs/install */
  path: string;
}

function sitePages(nav: NavGroup[], kind: SitePage['kind'], kindLabel: string, base: string): SitePage[] {
  return nav.flatMap(g => g.pages.map(p => ({ ...p, kind, kindLabel, group: g.group, path: p.slug ? `${base}/${p.slug}` : base })));
}

/** 说明 + 算法的全部页面，按侧栏顺序。 */
export const allPages: SitePage[] = [...sitePages(docsNav, 'docs', '使用说明', '/docs'), ...sitePages(algoNav, 'algorithms', '公开算法', '/algorithms')];

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
