// 构建后由 dist/index.html 生成 dist/index.zh.html：/zh/… 的页面回落到它（deploy/static-server.py、public/_redirects）。
// 为什么要第二份：分享卡片抓取器（微信、Telegram、X、Slack 等）不跑脚本，只读首屏 HTML 的 <title> 与 og:*；
// 只有一份英文 HTML 时，中文页链接分享出去显示的是英文标题和英文卡片。运行时的标题 / 描述 / hreflang 仍由 Nav 的 useHeadSync 同步。
// 文字来自 zh/common.json（site.title / site.description，与运行时同一份）与 src/i18n/share.json；每处替换必须恰好命中一次，否则构建失败。
import { readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { ORIGIN, SITE, readJson } from './site-data.mjs';

const site = readJson('src/i18n/locales/zh/common.json').site;
const share = readJson('src/i18n/share.json').zh;
const esc = s => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

const src = path.join(SITE, 'dist', 'index.html');
let html = readFileSync(src, 'utf8');

function swap(re, value, label) {
  const hits = html.match(new RegExp(re.source, 'g'))?.length ?? 0;
  if (hits !== 1) throw new Error(`gen-zh-html：${label} 在 dist/index.html 里命中 ${hits} 次（应为 1）`);
  html = html.replace(re, (_, head, tail) => `${head}${esc(value)}${tail}`);
}
const meta = (key, value, name = 'property') => swap(new RegExp(`(<meta ${name}="${key.replace(/[.:]/g, '\\$&')}" content=")[^"]*(")`), value, key);

swap(/(<html lang=")[^"]*(")/, 'zh-CN', '<html lang>');
swap(/(<title>)[^<]*(<\/title>)/, site.title, '<title>');
meta('description', site.description, 'name');
meta('og:site_name', share.siteName);
meta('og:locale', 'zh_CN');
meta('og:locale:alternate', 'en_US');
meta('og:url', `${ORIGIN}/zh`);
meta('og:title', share.title);
meta('og:description', share.description);
meta('og:image', `${ORIGIN}/og.png`);
meta('og:image:alt', share.imageAlt);
meta('twitter:title', share.title, 'name');
meta('twitter:description', share.twitterDescription, 'name');
meta('twitter:image', `${ORIGIN}/og.png`, 'name');
meta('twitter:image:alt', share.imageAlt, 'name');

writeFileSync(path.join(SITE, 'dist', 'index.zh.html'), html);
console.log('gen-zh-html：dist/index.zh.html（中文标题、描述与分享卡片）');
