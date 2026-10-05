// 折行后复制出来的文字（F8）：选中 AlgoTeaser 的英文图注与 CLAY 帧的「Low cut 25 Hz」标签，读 selection.toString()。
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const pw = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
for (const [name, port] of [['chromium', 8882], ['firefox', 8882], ['webkit', 8882], ['chromium', 8881]]) {
  const browser = await pw[name].launch();
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, reducedMotion: 'reduce' });
  const page = await ctx.newPage();
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'networkidle' });
  await page.evaluate(async () => { await document.fonts.ready; });
  await page.waitForTimeout(500);
  const r = await page.evaluate(() => {
    const read = el => { const range = document.createRange(); range.selectNodeContents(el); const s = getSelection(); s.removeAllRanges(); s.addRange(range); return s.toString(); };
    const caps = [...document.querySelectorAll('section svg[viewBox="0 0 280 120"] text')].filter(t => t.querySelector('tspan'));
    const lowcut = [...document.querySelectorAll('#process svg text')].find(t => /Low cut/.test(t.textContent));
    return { captions: caps.map(t => ({ lines: t.querySelectorAll('tspan').length, copied: read(t) })), lowcut: lowcut ? { lines: lowcut.querySelectorAll('tspan').length, copied: read(lowcut) } : null };
  });
  console.log(`${name}:${port}`, JSON.stringify(r).replace(/ /g, '␣'));
  await browser.close();
}
