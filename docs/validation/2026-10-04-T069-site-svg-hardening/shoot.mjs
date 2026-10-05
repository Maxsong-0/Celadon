// CLAY 帧（减少动效模式：四帧竖排，每帧一张 SVG）在三个引擎里改前 / 改后的页眉：用来看 WebKit 的标题与说明重叠。
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const pw = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
const jobs = [
  ['webkit', 'old', 8881, 1440], ['webkit', 'new', 8882, 1440], ['webkit', 'old', 8881, 375], ['webkit', 'new', 8882, 375],
  ['firefox', 'new', 8882, 1440], ['chromium', 'new', 8882, 1440]
];
for (const [name, tag, port, width] of jobs) {
  const browser = await pw[name].launch();
  const ctx = await browser.newContext({ viewport: { width, height: 900 }, reducedMotion: 'reduce', deviceScaleFactor: 1 });
  const page = await ctx.newPage();
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'networkidle' });
  await page.evaluate(async () => { await document.fonts.ready; });
  const svgs = page.locator('#process svg[viewBox="0 0 640 440"]');
  console.log(name, tag, width, 'svg 数', await svgs.count());
  const clay = svgs.nth(1);
  await clay.scrollIntoViewIfNeeded();
  await page.waitForTimeout(600);
  const box = await clay.boundingBox();
  await page.screenshot({ path: `shots/${name}-clay-${tag}-${width}.png`, clip: { x: box.x, y: box.y, width: box.width, height: Math.min(box.height, Math.round(box.width * 0.2)) } });
  await browser.close();
}
