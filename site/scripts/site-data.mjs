// 构建脚本共用（check-i18n / gen-sitemap / gen-zh-html）：站点地址、页面清单、文案读取。
// 页面清单的唯一真源是 src/content/index.ts 的 docsNav / algoNav；这里按固定写法解析它，不另抄一份。
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const ORIGIN = 'https://celadon.maxsong.me';
export const LOCALES = ['en', 'zh'];
/** 与 src/i18n/paths.ts 的 HTML_LANG 一致（ADR-024：英文在 /，中文在 /zh/） */
export const HREFLANG = { en: 'en', zh: 'zh-CN' };

export const read = rel => readFileSync(path.join(SITE, rel), 'utf8');
export const readJson = rel => JSON.parse(read(rel));

/** docsNav / algoNav：分组键与页面（slug + file）。写法变了解析不到就直接失败，不静默少页。 */
export function readNav() {
  const src = read('src/content/index.ts');
  const block = name => {
    const m = src.match(new RegExp(`export const ${name}: NavGroup\\[\\] = \\[([\\s\\S]*?)\\n\\];`));
    if (!m) throw new Error(`src/content/index.ts 里找不到 ${name}`);
    const groups = [...m[1].matchAll(/group: '([a-z]+)'/g)].map(x => x[1]);
    const pages = [...m[1].matchAll(/\{ slug: '([a-z0-9-]*)', file: '([a-z0-9-]+)' \}/g)].map(x => ({ slug: x[1], file: x[2] }));
    const declared = (m[1].match(/slug:/g) ?? []).length;
    if (!pages.length || pages.length !== declared) throw new Error(`${name}：解析到 ${pages.length} 页，源码里有 ${declared} 处 slug（写法变了，改这里的解析）`);
    return { groups, pages };
  };
  return { docs: block('docsNav'), algorithms: block('algoNav') };
}

/** 全部页面路径（不带语言前缀）：首页、下载、更新日志、说明、算法。 */
export function allPaths(nav = readNav()) {
  const section = (base, pages) => pages.map(p => (p.slug ? `${base}/${p.slug}` : base));
  return ['/', '/download', '/changelog', ...section('/docs', nav.docs.pages), ...section('/algorithms', nav.algorithms.pages)];
}

/** 与 src/i18n/paths.ts 的 localizePath 同一规则 */
export const localizedPath = (p, locale) => (locale === 'zh' ? (p === '/' ? '/zh' : `/zh${p}`) : p);
