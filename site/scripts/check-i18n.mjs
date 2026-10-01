// 双语一致性检查（ADR-024），接在 pnpm build 最前面：有一条不过就构建失败，不出半套语言的站。
//   1. 界面短句：两种语言的命名空间文件相同、key 集合相同（复数后缀 _one / _other 等归并后比），插值 {{x}} 与 <标签> 相同；
//   2. Markdown：说明 / 算法两种语言成对，与导航页面一一对应；同篇二级标题数相同、示意图引用相同；
//   3. 导航：每页在两种语言的 docs 命名空间里都有标题与摘要，每个分组都有名字；
//   4. 英文资源里不得出现汉字（JSON、Markdown 去掉代码后、changelog.ts 的 en 文案、share.json）；
//   5. index.html 写死的英文标题 / 描述 / 分享卡片与文案一致（gen-zh-html 按同一份文案出中文版）；
//   6. 对外文案不提任何对标插件（ADR-023），两种语言都查。
import { existsSync, readdirSync } from 'node:fs';
import path from 'node:path';
import { LOCALES, SITE, read, readJson, readNav } from './site-data.mjs';

const errors = [];
const fail = msg => errors.push(msg);

// 汉字与全角标点（中文引号、顿号、全角括号等）
const CJK = /[　-〿㐀-䶿一-鿿＀-￯]/;
const PLURAL = /_(zero|one|two|few|many|other)$/;

function flatten(obj, prefix = '', out = new Map()) {
  for (const [k, v] of Object.entries(obj)) {
    const key = prefix ? `${prefix}.${k}` : k;
    if (v && typeof v === 'object') flatten(v, key, out);
    else out.set(key, String(v));
  }
  return out;
}

// 去重后比（复数各形式里同一个插值会出现多次）
const tokens = (s, re) => [...new Set([...s.matchAll(re)].map(m => m[1]))].sort().join(',');
const vars = s => tokens(s, /\{\{\s*([\w.]+)[^}]*\}\}/g);
const tags = s => tokens(s, /<\/?([\w-]+)\s*\/?>/g);

// ---- 1. 界面短句 ----
const LOCALE_DIR = 'src/i18n/locales';
const nsFiles = Object.fromEntries(LOCALES.map(l => [l, readdirSync(path.join(SITE, LOCALE_DIR, l)).filter(f => f.endsWith('.json')).sort()]));
if (nsFiles.en.join() !== nsFiles.zh.join()) fail(`命名空间文件不同：en [${nsFiles.en}] / zh [${nsFiles.zh}]`);

const locales = {};
for (const file of nsFiles.en) {
  const ns = file.replace(/\.json$/, '');
  const byLocale = {};
  for (const l of LOCALES) {
    if (!existsSync(path.join(SITE, LOCALE_DIR, l, file))) continue;
    // 复数各形式归并到同一个基础 key；插值与标签取各形式的并集比较
    const merged = new Map();
    for (const [key, value] of flatten(readJson(`${LOCALE_DIR}/${l}/${file}`))) {
      const base = key.replace(PLURAL, '');
      const prev = merged.get(base);
      merged.set(base, prev === undefined ? value : `${prev}\n${value}`);
      if (l === 'en' && CJK.test(value)) fail(`${l}/${file} ${key} 有汉字：${value}`);
    }
    byLocale[l] = merged;
  }
  locales[ns] = byLocale;
  const [en, zh] = [byLocale.en, byLocale.zh];
  if (!en || !zh) continue;
  for (const k of en.keys()) if (!zh.has(k)) fail(`${ns}：zh 缺 ${k}`);
  for (const k of zh.keys()) if (!en.has(k)) fail(`${ns}：en 缺 ${k}`);
  for (const [k, ev] of en) {
    const zv = zh.get(k);
    if (zv === undefined) continue;
    if (vars(ev) !== vars(zv)) fail(`${ns} ${k}：插值不同（en ${vars(ev) || '无'} / zh ${vars(zv) || '无'}）`);
    if (tags(ev) !== tags(zv)) fail(`${ns} ${k}：标签不同（en ${tags(ev) || '无'} / zh ${tags(zv) || '无'}）`);
  }
}

// ---- 2. Markdown 与 3. 导航 ----
const nav = readNav();
const stripCode = md => md.replace(/^(```|~~~)[\s\S]*?^\1/gm, '').replace(/`[^`\n]*`/g, '');
const h2Count = md => (stripCode(md).match(/^## /gm) ?? []).length;
const figureRefs = md => tokens(md, /\]\(figure:([\w-]+)\)/g);

for (const kind of ['docs', 'algorithms']) {
  const want = nav[kind].pages.map(p => `${p.file}.md`).sort();
  const files = {};
  for (const l of LOCALES) {
    const dir = path.join(SITE, 'src/content', kind, l);
    files[l] = existsSync(dir) ? readdirSync(dir).filter(f => f.endsWith('.md')).sort() : [];
    for (const f of want) if (!files[l].includes(f)) fail(`content/${kind}/${l}/ 缺 ${f}（导航里有）`);
    for (const f of files[l]) if (!want.includes(f)) fail(`content/${kind}/${l}/${f} 不在导航里`);
  }
  for (const f of want) {
    const [en, zh] = LOCALES.map(l => (files[l].includes(f) ? read(`src/content/${kind}/${l}/${f}`) : null));
    if (en === null || zh === null) continue;
    if (h2Count(en) !== h2Count(zh)) fail(`content/${kind}/*/${f}：二级标题数不同（en ${h2Count(en)} / zh ${h2Count(zh)}）`);
    if (figureRefs(en) !== figureRefs(zh)) fail(`content/${kind}/*/${f}：示意图引用不同（en ${figureRefs(en) || '无'} / zh ${figureRefs(zh) || '无'}）`);
    stripCode(en)
      .split('\n')
      .forEach((line, i) => {
        if (CJK.test(line)) fail(`content/${kind}/en/${f}（去掉代码后约第 ${i + 1} 行）有汉字：${line.trim().slice(0, 80)}`);
      });
  }
  for (const l of LOCALES) {
    const docs = locales.docs?.[l];
    if (!docs) continue;
    for (const p of nav[kind].pages)
      for (const field of ['title', 'summary']) if (!docs.get(`pages.${kind}.${p.file}.${field}`)) fail(`${l}/docs.json 缺 pages.${kind}.${p.file}.${field}`);
    for (const g of nav[kind].groups) if (!docs.get(`groups.${g}`)) fail(`${l}/docs.json 缺 groups.${g}`);
  }
}

// ---- 4. changelog.ts 与 share.json 的英文 ----
const changelog = read('src/content/changelog.ts');
for (const m of changelog.matchAll(/\ben:\s*(['"`])((?:\\.|(?!\1)[^\\])*)\1/g)) if (CJK.test(m[2])) fail(`changelog.ts 的 en 文案有汉字：${m[2].slice(0, 80)}`);

const share = readJson('src/i18n/share.json');
const shareKeys = l => Object.keys(share[l] ?? {}).sort().join();
if (shareKeys('en') !== shareKeys('zh')) fail(`share.json 两种语言的字段不同：en [${shareKeys('en')}] / zh [${shareKeys('zh')}]`);
for (const [k, v] of Object.entries(share.en ?? {})) if (CJK.test(v)) fail(`share.json en.${k} 有汉字：${v}`);

// ---- 5. index.html 的英文头部 ----
const html = read('index.html');
const unescape = s => s.replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const attr = (re, label) => {
  const m = html.match(re);
  if (!m) fail(`index.html 找不到 ${label}`);
  return m ? unescape(m[1]) : null;
};
const meta = (key, name = 'property') => attr(new RegExp(`<meta ${name}="${key}" content="([^"]*)"`), `<meta ${name}="${key}">`);
const site = { title: locales.common?.en?.get('site.title'), description: locales.common?.en?.get('site.description') };
const expect = [
  [attr(/<html lang="([^"]*)"/, '<html lang>'), 'en', '<html lang>'],
  [attr(/<title>([^<]*)<\/title>/, '<title>'), site.title, '<title>（common site.title）'],
  [meta('description', 'name'), site.description, 'description（common site.description）'],
  [meta('og:site_name'), share.en?.siteName, 'og:site_name'],
  [meta('og:title'), share.en?.title, 'og:title'],
  [meta('og:description'), share.en?.description, 'og:description'],
  [meta('og:image:alt'), share.en?.imageAlt, 'og:image:alt'],
  [meta('twitter:title', 'name'), share.en?.title, 'twitter:title'],
  [meta('twitter:description', 'name'), share.en?.twitterDescription, 'twitter:description'],
  [meta('twitter:image:alt', 'name'), share.en?.imageAlt, 'twitter:image:alt']
];
for (const [got, want, label] of expect) if (got !== null && got !== want) fail(`index.html ${label} 与文案不一致：\n    现在 ${got}\n    应为 ${want}`);

// ---- 6. ADR-023：不提对标插件 ----
const BANNED = /gullfoss|god particle|invisible limiter|soundtheory|cradle|sonible|对标|参考链/i;
const walk = dir =>
  readdirSync(path.join(SITE, dir), { withFileTypes: true }).flatMap(e => (e.isDirectory() ? walk(path.join(dir, e.name)) : [path.join(dir, e.name)]));
for (const rel of [...walk('src'), 'index.html']) {
  if (!/\.(tsx?|json|md|html|css)$/.test(rel)) continue;
  read(rel)
    .split('\n')
    .forEach((line, i) => {
      if (BANNED.test(line)) fail(`${rel}:${i + 1} 提到了对标插件（ADR-023）：${line.trim().slice(0, 80)}`);
    });
}

if (errors.length) {
  console.error(`check-i18n：${errors.length} 处不通过\n  - ${errors.join('\n  - ')}`);
  process.exit(1);
}
const pages = nav.docs.pages.length + nav.algorithms.pages.length;
console.log(`check-i18n：通过（${nsFiles.en.length} 个命名空间、${pages} 篇 Markdown × 2 种语言、index.html 头部、ADR-023）`);
