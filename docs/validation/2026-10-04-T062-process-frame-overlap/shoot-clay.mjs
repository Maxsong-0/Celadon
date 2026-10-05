// 逐帧截图：减少动效模式下四帧竖排；强制只显示「处理前」或「处理后」层，截 slab（瓷面）。
// 用法：node shoot-clay.mjs <baseURL> <outDir> <标签> [宽度 1440] [帧号 全部]
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { chromium } = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
const [base, outDir, label, widthArg, frameArg] = process.argv.slice(2);
const width = Number(widthArg || 1440);
const browser = await chromium.launch();
for (const lang of ['en', 'zh']) for (const layer of ['before', 'after']) {
  const ctx = await browser.newContext({ viewport: { width, height: 900 }, deviceScaleFactor: 2, reducedMotion: 'reduce' });
  const page = await ctx.newPage();
  await page.goto(base + (lang === 'zh' ? '/zh/' : '/'), { waitUntil: 'networkidle' });
  const hide = layer === 'before' ? 'after' : 'before';
  await page.addStyleTag({ content: `#process [data-${layer}] { opacity: 1 !important; visibility: visible !important; } #process [data-${hide}] { opacity: 0 !important; }` });
  const slabs = page.locator('#process .slab');
  const n = await slabs.count();
  for (let i = 0; i < n; i++) {
    if (frameArg !== undefined && Number(frameArg) !== i) continue;
    await slabs.nth(i).scrollIntoViewIfNeeded();
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(300);
    await slabs.nth(i).screenshot({ path: `${outDir}/${label}-${width}-${lang}-${layer}-${i}.png`, animations: 'disabled' });
  }
  await ctx.close();
}
await browser.close();
