// AlgoTeaser 的 COLOR / LIMIT 小图（英文图注折两行）改前 / 改后：图注与曲线的距离。
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const pw = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
const browser = await pw.chromium.launch();
for (const [tag, port] of [['old', 8881], ['new', 8882]]) {
  for (const [lang, route] of [['en', '/'], ['zh', '/zh']]) {
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 3, reducedMotion: 'reduce' });
    const page = await ctx.newPage();
    await page.goto(`http://127.0.0.1:${port}${route}`, { waitUntil: 'networkidle' });
    await page.evaluate(async () => { await document.fonts.ready; });
    const svgs = page.locator('section svg[viewBox="0 0 280 120"]');
    for (const [i, name] of [[1, 'limit'], [2, 'color']]) {
      const el = svgs.nth(i);
      await el.scrollIntoViewIfNeeded();
      await page.waitForTimeout(500);
      await el.screenshot({ path: `shots/algo-${name}-${lang}-${tag}.png` });
    }
    await ctx.close();
  }
}
await browser.close();
console.log('ok');
