// 开发模式（React 的警告只在 dev 构建里出）下，三个引擎各开 / 与 /zh，滚动、缩放窗口、等字体：收集控制台警告与错误。
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const pw = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
const out = {};
for (const name of ['chromium', 'firefox', 'webkit']) {
  const browser = await pw[name].launch();
  const msgs = [];
  for (const route of ['/', '/zh']) {
    for (const width of [1440, 375]) {
      const page = await browser.newPage({ viewport: { width, height: 900 } });
      page.on('console', m => { if (['warning', 'error'].includes(m.type())) msgs.push(`${route}@${width} ${m.type()}: ${m.text().slice(0, 240)}`); });
      page.on('pageerror', e => msgs.push(`${route}@${width} pageerror: ${String(e.message).slice(0, 240)}`));
      await page.goto('http://127.0.0.1:8883' + route, { waitUntil: 'networkidle' });
      await page.evaluate(async () => { await document.fonts.ready; });
      for (const id of ['#process', '#algorithms', 'main']) {
        await page.locator(id).first().scrollIntoViewIfNeeded().catch(() => null);
        await page.waitForTimeout(400);
      }
      await page.evaluate(async () => { for (let y = 0; y < document.documentElement.scrollHeight; y += 500) { window.scrollTo(0, y); await new Promise(r => setTimeout(r, 40)); } });
      await page.setViewportSize({ width: width === 1440 ? 900 : 480, height: 900 });   // 触发 ResizeObserver
      await page.waitForTimeout(500);
      await page.close();
    }
  }
  out[name] = msgs;
  await browser.close();
}
for (const [k, v] of Object.entries(out)) {
  const uniq = [...new Set(v)];
  console.log(`${k}: ${uniq.length} 条不同的警告 / 错误`);
  uniq.slice(0, 12).forEach(m => console.log('   ', m));
}
