import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const pw = require('/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');
process.env.PLAYWRIGHT_SKIP_VALIDATE_HOST_REQUIREMENTS = '1';
for (const name of ['chromium', 'firefox', 'webkit']) {
  const browser = await pw[name].launch();
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  await page.goto('http://127.0.0.1:8882/', { waitUntil: 'networkidle' });
  await page.evaluate(async () => { await document.fonts.ready; });
  await page.locator('#process').scrollIntoViewIfNeeded();
  await page.waitForTimeout(600);
  const r = await page.evaluate(() => {
    const f = document.querySelector('#process [data-visual="1"]');
    const [title, note] = f.querySelectorAll(':scope > text');
    const n = title.getNumberOfChars();
    const bb = title.getBBox(); const nb = note.getBBox();
    return {
      chars: n, titleComputed: title.getComputedTextLength(), titleBBoxW: bb.width, titleBBoxX: bb.x,
      titleEnd: title.getEndPositionOfChar(n - 1).x - title.getStartPositionOfChar(0).x,
      sub: title.getSubStringLength(0, n),
      noteComputed: note.getComputedTextLength(), noteBBoxW: nb.width, noteBBoxX: nb.x,
      letterSpacing: getComputedStyle(title).letterSpacing, y: title.getAttribute('y')
    };
  });
  console.log(name.padEnd(9), JSON.stringify(r));
  await browser.close();
}
