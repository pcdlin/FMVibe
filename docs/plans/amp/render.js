// Renders drafts.html's figures to PNGs beside it at 2x: node render.js
const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ deviceScaleFactor: 2, viewport: { width: 1200, height: 900 } });
  await page.goto('file://' + path.join(__dirname, 'drafts.html'));
  for (const id of ['a-row', 'a-model', 'a-cab', 'a-off', 'b', 'c'])
    await page.locator('#' + id).screenshot({ path: path.join(__dirname, `draft-${id}.png`) });
  await browser.close();
})();
