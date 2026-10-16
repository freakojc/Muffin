// Small offline Chromium oracle for CSS-wide and invalid variable positions.
import fs from 'node:fs';
import path from 'node:path';
import {chromium} from 'playwright';
const output = process.argv[2];
if (!output) throw new Error('Usage: node scripts/probe_background_position.mjs output-directory');
fs.mkdirSync(output, {recursive:true});
const css = fs.readFileSync('tests/fixtures/theme/vlook-fancy-dark/vlook-fancy-dark.css', 'utf8');
const start = css.indexOf('--d-bi-dk:url(') + '--d-bi-dk:'.length;
const image = css.slice(start, css.indexOf(')', start) + 1);
const browser = await chromium.launch({headless:true, ...(process.env.CHROME_EXECUTABLE ? {executablePath:process.env.CHROME_EXECUTABLE} : {})});
try {
  const page = await browser.newPage({viewport:{width:1100,height:820},deviceScaleFactor:1});
  await page.route('**/*', route => route.abort());
  const cases = [];
  for (const height of [1000000,1000020,1000040]) {
    await page.setContent(`<style>html,body{margin:0;background:#1a1915}#write{position:absolute;left:0;top:20px;width:1086px;height:${height}px;background:${image},#1a1915;background-position:var(--d-bi-pos),0 0}</style><div id="write"></div>`);
    const position = await page.locator('#write').evaluate(node => getComputedStyle(node).backgroundPosition);
    const screenshot = path.join(output, `browser-${height}.png`);
    await page.screenshot({path:screenshot});
    cases.push({height,position,screenshot});
  }
  fs.writeFileSync(path.join(output,'browser.json'),JSON.stringify({browser:await browser.version(),cases},null,2)+'\n');
} finally {await browser.close();}
