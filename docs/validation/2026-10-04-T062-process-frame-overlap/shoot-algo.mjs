// AlgoTeaser 三张小图（viewBox 0 0 280 120）与 Bento 小图逐张截图。用法：node shoot-algo.mjs <baseURL> <outDir> <标签> <宽度>
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { chromium } = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
const [base, outDir, label, widthArg] = process.argv.slice(2);
const width = Number(widthArg || 1440);
const browser = await chromium.launch();
for (const lang of ['en', 'zh']) {
  const ctx = await browser.newContext({ viewport: { width, height: 900 }, deviceScaleFactor: 3 });
  const page = await ctx.newPage();
  await page.goto(base + (lang === 'zh' ? '/zh/' : '/'), { waitUntil: 'networkidle' });
  await page.evaluate(() => document.fonts.ready);
  const figs = page.locator('svg[viewBox="0 0 280 120"], svg[viewBox="0 0 200 72"]');
  const n = await figs.count();
  for (let i = 0; i < n; i++) {
    await figs.nth(i).scrollIntoViewIfNeeded();
    await page.waitForTimeout(250);
    await figs.nth(i).screenshot({ path: `${outDir}/${label}-${width}-${lang}-fig${i}.png`, animations: 'disabled' });
  }
  await ctx.close();
}
await browser.close();
