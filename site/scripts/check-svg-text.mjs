// 官网 SVG 文字的构建期检查（不开浏览器，随 `pnpm run build` 跑）：
//   ① src/lib/wrapWords.ts 的折行规则（纯函数）：西文按词、中日韩按字、标点不落行首、左括号不落行尾、数字与单位粘住、
//      折行后各行拼回去等于原文；
//   ② 首页示意图里文案的宽度预算（src/lib/figureBudget.ts）：页眉说明不越过整条页眉、图注折出的行数不超过图上留的行数。
// 运行时的实测（真实字体、屏宽 320 ～ 1920、三个浏览器引擎）在 docs/validation/2026-10-04-T062-process-frame-overlap/sweep.mjs。
// 用 vite 的 ssrLoadModule 直接读 .ts 源码（不依赖 Node 的类型剥离）；失败退出码 1。
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer } from 'vite';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const failures = [];
let checks = 0;
const check = (ok, message) => {
  checks++;
  if (!ok) failures.push(message);
};
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const show = value => JSON.stringify(value).replace(/ /g, '␣');

const server = await createServer({
  root: SITE,
  configFile: false,
  appType: 'custom',
  logLevel: 'silent',
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  optimizeDeps: { noDiscovery: true, include: [] }
});
let wrap;
let budget;
try {
  wrap = await server.ssrLoadModule('/src/lib/wrapWords.ts');
  budget = await server.ssrLoadModule('/src/lib/figureBudget.ts');
} finally {
  await server.close();
}
const { wrapWords, tokenize } = wrap;

/* ---------------- ① 折行规则 ---------------- */

const CJK = /[⺀-鿿가-힯豈-﫿＀-￯]/;
const NBSP = ' ';
// 假的量宽度：西文 6、中日韩 12（与浏览器无关，只测折行逻辑）
const width = s => [...s].reduce((sum, ch) => sum + (CJK.test(ch) ? 12 : 6), 0);
const wrapAt = (text, maxWidth) => wrapWords(text, maxWidth, width);
// 原文里的连续空白合成一个空格、数字与单位之间换成不换行空格之后的样子：折行后的各行直接拼起来应当等于它
const collapsed = text => text.replace(/(\d)[ \t]+(?=[A-Za-z%°µ])/g, `$1${NBSP}`).replace(/[ \t\r\n]+/g, ' ').trim();

let lines = wrapAt('Low cut 25 Hz', 62);
check(same(lines, ['Low cut ', `25${NBSP}Hz`]), `「Low cut 25 Hz」在 62 宽应折成 ["Low cut ", "25␣Hz"]（数字和单位粘住、行尾带空格），实际 ${show(lines)}`);
lines = wrapAt('Low cut 25 Hz', 80);
check(same(lines, [`Low cut 25${NBSP}Hz`]), `放得下应是一行，实际 ${show(lines)}`);
lines = wrapAt('低切 25 Hz', 62);
check(same(lines, [`低切 25${NBSP}Hz`]), `中文短标签应是一行，实际 ${show(lines)}`);

// 中日韩文字没有空格：按字断，行尾不补空格，拼回去等于原文
const cjkText = '两级滑动平均让拐角连续且不再出现阶梯状的增益跳变现象';
lines = wrapAt(cjkText, 120);
check(lines.length === 3 && lines.every(l => width(l) <= 120) && lines.join('') === cjkText && lines.every(l => !l.includes(' ')),
  `中文长句应按字折成 3 行、每行不超宽、不带空格，实际 ${show(lines)}`);

// 标点不落行首、左括号不落行尾
const punct = '光滑，连续。更多的细节（含 25 Hz）在这里；还有——省略……以及·间隔';
for (const maxWidth of [36, 48, 60, 72, 96]) {
  lines = wrapAt(punct, maxWidth);
  check(lines.every(l => l.trim() === '' || !/^[，。、；：！？）］｝〉》」』】〕…—·]/.test(l)), `maxWidth ${maxWidth}：有一行以不能在行首的标点开头 ${show(lines)}`);
  check(lines.every(l => !/[（［｛〈《「『【〔]$/.test(l.trimEnd())), `maxWidth ${maxWidth}：有一行以左括号结尾 ${show(lines)}`);
  check(lines.join('') === collapsed(punct), `maxWidth ${maxWidth}：折行后拼回去应等于原文 ${show(lines)}`);
}

lines = wrapAt('Supercalifragilisticexpialidocious', 20);
check(same(lines, ['Supercalifragilisticexpialidocious']), `单个词比 maxWidth 还宽时独占一行，实际 ${show(lines)}`);
check(same(wrapAt('', 50), ['']) && same(wrapAt('   ', 50), ['']), '空串与全空白应得到 [""]');
check(same(wrapAt('CLAY的低切', 1000), ['CLAY的低切']), '西文与中文紧挨着时不凭空补空格');
check(tokenize('A  B\nC').length === 3 && tokenize('A  B\nC').slice(1).every(t => t.space), '连续空白合成一个词间空格');

/* ---------------- ② 文案预算 ---------------- */

const readJson = file => JSON.parse(fs.readFileSync(path.join(SITE, file), 'utf8'));
const C = budget.ALGO_CAPTION;
const F = budget.PROCESS_FRAME;
// 与 AlgoTeaser.tsx 的 items 同一顺序：BALANCE、LIMIT、COLOR
const slots = [C.balance, C.limit, C.color];
const slotNames = ['BALANCE', 'LIMIT', 'COLOR'];

for (const lang of ['en', 'zh']) {
  const home = readJson(`src/i18n/locales/${lang}/home.json`);
  for (const key of ['balanceNote', 'clayNote', 'colorNote', 'limitNote']) {
    const text = home.process.visual[key];
    const estimate = budget.estimateWidth(text, F.noteFontSize, F.noteLatinEm);
    check(typeof text === 'string' && estimate <= budget.processFrameNoteBudget(),
      `${lang}/home.json process.visual.${key}：估算宽度 ${Math.round(estimate)} 超过页眉说明的预算 ${budget.processFrameNoteBudget()}（会越过页眉左边线，请缩短）`);
  }
  home.algorithms.items.forEach((item, i) => {
    const slot = slots[i];
    const folded = wrapWords(item.art, budget.captionMaxWidth(slot.x), s => budget.estimateWidth(s, C.fontSize, C.latinEm));
    check(folded.length <= slot.maxLines,
      `${lang}/home.json algorithms.items[${i}].art（${slotNames[i]} 图注）：估算折成 ${folded.length} 行，这张图只留了 ${slot.maxLines} 行（会压到图形，请缩短或在 figureBudget.ts 里腾出位置）`);
  });
}

if (failures.length) {
  console.error(`check-svg-text：${failures.length} 项失败（共 ${checks} 项）`);
  for (const f of failures) console.error('  - ' + f);
  process.exit(1);
}
console.log(`check-svg-text：通过（${checks} 项：折行规则、页眉说明与图注的文案预算）`);
