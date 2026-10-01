// 构建后生成 dist/sitemap.xml（ADR-024）：全部页面 × 两种语言，每条带 xhtml:link 互指（en / zh-CN / x-default）。
// 页面清单来自 src/content/index.ts（site-data.mjs 解析），加页不用再手改站点地图。
// 不写 lastmod：构建机上没有每页的真实修改时间，写构建日期等于每次都说全站改过。
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { HREFLANG, LOCALES, ORIGIN, SITE, allPaths, localizedPath } from './site-data.mjs';

const paths = allPaths();
const url = p => `${ORIGIN}${p}`;

const entries = paths.flatMap(p => {
  const links = [
    ...LOCALES.map(l => `    <xhtml:link rel="alternate" hreflang="${HREFLANG[l]}" href="${url(localizedPath(p, l))}"/>`),
    `    <xhtml:link rel="alternate" hreflang="x-default" href="${url(p)}"/>`
  ].join('\n');
  return LOCALES.map(l => `  <url>\n    <loc>${url(localizedPath(p, l))}</loc>\n${links}\n  </url>`);
});

const xml = `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:xhtml="http://www.w3.org/1999/xhtml">
${entries.join('\n')}
</urlset>
`;
const out = path.join(SITE, 'dist', 'sitemap.xml');
writeFileSync(out, xml);
console.log(`gen-sitemap：${entries.length} 条（${paths.length} 页 × ${LOCALES.length} 种语言）→ dist/sitemap.xml`);
