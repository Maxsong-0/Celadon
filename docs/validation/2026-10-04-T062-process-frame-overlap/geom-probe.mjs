// 实验：SVG 文字在 Chromium 里宽度随缩放比浮动（字形步进取整）。加 text-rendering: geometricPrecision 后用户单位下的宽度是否恒定？
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { chromium } = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
const base = process.argv[2];
const browser = await chromium.launch();
const widths = [320, 375, 414, 768, 1024, 1440];
const results = {};
for (const mode of ['default', 'geometric']) {
  for (const w of widths) {
    const page = await browser.newPage({ viewport: { width: w, height: 900 } });
    await page.goto(base + '/', { waitUntil: 'networkidle' });
    if (mode === 'geometric') await page.addStyleTag({ content: 'svg text { text-rendering: geometricPrecision !important; }' });
    await page.evaluate(async () => { await document.fonts.ready; });
    await page.locator('#process').scrollIntoViewIfNeeded();
    await page.waitForTimeout(500);
    const r = await page.evaluate(() => {
      const out = {};
      const pick = (label, el) => { if (el) out[label] = +el.getComputedTextLength().toFixed(2); };
      const frames = document.querySelectorAll('#process [data-visual]');
      // 标题（等宽 12px，字距 0.18em）与说明（sans 12px）：CLAY / COLOR / LIMIT 三帧
      for (const i of [1, 2, 3]) {
        const f = document.querySelector(`#process [data-visual="${i}"]`);
        if (!f) continue;
        const [title, note] = f.querySelectorAll(':scope > text');
        pick(`frame${i}.title`, title); pick(`frame${i}.note`, note);
      }
      // LIMIT 帧底部链路说明（10px 等宽）
      const limit = document.querySelector('#process [data-visual="3"]');
      if (limit) { const t = [...limit.querySelectorAll(':scope > text')].find(e => e.getAttribute('y') === '412' && e.getAttribute('x') === '60'); pick('limitChain', t); }
      // AlgoTeaser 图注（9px 等宽）
      document.querySelectorAll('section svg[viewBox="0 0 280 120"] text').forEach((t, k) => pick(`algo${k}`, t));
      // Bento ArtRef
      document.querySelectorAll('svg[viewBox="0 0 200 72"] text').forEach((t, k) => pick(`bento${k}`, t));
      return out;
    });
    (results[mode] ||= {})[w] = r;
    await page.close();
  }
}
await browser.close();
const keys = [...new Set(Object.values(results.default).flatMap(o => Object.keys(o)))];
for (const k of keys) {
  const row = mode => widths.map(w => (results[mode][w][k] ?? '-')).join(' / ');
  console.log(k.padEnd(14), 'default  :', row('default'));
  console.log(' '.repeat(14), 'geometric:', row('geometric'));
}
