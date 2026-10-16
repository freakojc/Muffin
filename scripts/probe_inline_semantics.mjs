import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {createHash} from 'node:crypto';
import {browserLayoutFont, loadBrowserLayoutFont} from './browser_layout_font.mjs';
const {chromium} = await import(process.env.MUFFIN_PLAYWRIGHT_MODULE ? pathToFileURL(path.resolve(process.env.MUFFIN_PLAYWRIGHT_MODULE)).href : 'playwright');
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const font = browserLayoutFont(root);
const iconSource = fs.readFileSync(path.join(root, 'src/theme/MarkdownAlertIcon.h'), 'utf8').replace(/\r\n/g,'\n');
const paths = Object.fromEntries([...iconSource.matchAll(/case AlertKind::(\w+):\s*return "([^"]+)"/g)].map(m => [m[1].toUpperCase(), m[2]]));
const cases = [
  {id:'nested-wrappers', width:280, markdown:'**bold *nested*** end',
   html:'<span md-inline="strong"><strong>bold <span md-inline="em"><em>nested</em></span></strong></span> end',
   rules:'span[md-inline=strong]::before{content:"["}strong::before{content:"{"}em::before{content:"("}em::after{content:")"}strong::after{content:"}"}span[md-inline=strong]::after{content:"]"}'},
  {id:'adjacent-ancestors', width:280, markdown:'**one**_two_',
   html:'<span md-inline="strong"><strong>one</strong></span><span md-inline="em"><em>two</em></span>',
   rules:'strong::before{content:"A"}strong::after{content:"B"}em::before{content:"C"}em::after{content:"D"}'},
  {id:'wrapped-inline-shape', width:92, markdown:'prefix **bold** suffix',
   html:'prefix <span md-inline="strong"><strong>bold</strong></span> suffix',
   rules:'strong::before{content:"";display:inline-block;width:12px;height:12px;margin-right:6px;background:red}strong::after{content:"!";color:blue}'},
  {id:'nested-link', width:280, markdown:'[**bold**](https://example.com)',
   html:'<span md-inline="link"><a href="https://example.com"><span md-inline="strong"><strong>bold</strong></span></a></span>',
   rules:'a::before{content:"["}a::after{content:"]"}strong::before{content:"{"}strong::after{content:"}"}'},
  {id:'hidden-generation', width:280, markdown:'**bold** *italic*',
   html:'<span md-inline="strong"><strong>bold</strong></span> <span md-inline="em"><em>italic</em></span>',
   rules:'strong::before{content:none}strong::after{content:"hidden";display:none}em::before{content:normal}'},
];
for (const [kind, svgPath] of Object.entries(paths)) cases.push({
  id:`alert-${kind.toLowerCase()}`, width:280, markdown:`> [!${kind}]\n> Body text.`, alert:true,
  html:`<blockquote class="md-alert md-alert-${kind.toLowerCase()}"><p><span class="md-alert-text-container"><span class="md-alert-text md-alert-text-${kind.toLowerCase()}"><svg viewBox="0 0 16 16" width="16" height="16"><path d="${svgPath}"/></svg>${kind}</span></span><br>Body text.</p></blockquote>`,
  rules:'.md-alert-text svg{width:1em;height:1em;margin-right:.5em;fill:currentColor;vertical-align:middle}.md-alert-text-container::before{content:"["}.md-alert-text::before{content:"{"}.md-alert-text::after{content:"}"}.md-alert-text-container::after{content:"]"}',
});
const browser = await chromium.launch({headless:true, executablePath:process.env.CHROME_EXECUTABLE});
try {
  const page = await browser.newPage({viewport:{width:400,height:600}});
  for (const c of cases) {
    c.css = `html,body{margin:0}#write{width:${c.width}px;padding:0;margin:0;max-width:none;font-family:MuffinFixtureSans;font-size:16px;line-height:24px;color:#123456}p,blockquote{margin:0;padding:0;border:0;line-height:24px}strong{font-weight:700}${c.rules}`;
    await page.setContent(`<!doctype html><style>${font.css}${c.css}</style><content><div id="write">${c.alert ? c.html : `<p>${c.html}</p>`}</div></content>`);
    await loadBrowserLayoutFont(page,font);
    c.expected = await page.evaluate(() => {
      const p = document.querySelector('#write p'), origin = p.getBoundingClientRect();
      const walker = document.createTreeWalker(p,NodeFilter.SHOW_TEXT), characters = [];
      while (walker.nextNode()) for(let i=0;i<walker.currentNode.length;i++) {
        const range = new Range(); range.setStart(walker.currentNode,i); range.setEnd(walker.currentNode,i+1);
        const r = range.getBoundingClientRect();
        characters.push({text:walker.currentNode.data[i], x:r.x-origin.x, y:r.y-origin.y});
      }
      const content = (el,pseudo) => {
        const style = getComputedStyle(el,pseudo), text = style.content;
        if (style.display === 'none' || text === 'none' || text === 'normal') return '';
        const literal = JSON.parse(text);
        return literal === '' && style.display === 'inline-block' && parseFloat(style.width) > 0 ? '\u00a0' : literal;
      };
      const display = node => node.nodeType === Node.TEXT_NODE ? node.data : node.localName === 'svg' ? '\u00a0' : node.tagName === 'BR' ? '\n' :
        content(node,'::before') + [...node.childNodes].map(display).join('') + content(node,'::after');
      const svg = p.querySelector('svg'), r = svg?.getBoundingClientRect();
      return {height:origin.height, characters, display:display(p), icon:r ? {x:r.x-origin.x, y:r.y-origin.y, width:r.width, height:r.height} : null};
    });
    delete c.rules; delete c.html;
  }
  fs.writeFileSync(path.join(root,'tests/fixtures/theme/inline-semantics-browser.json'),JSON.stringify({browser:await browser.version(),font:font.metadata,
    iconSourceSha256:createHash('sha256').update(iconSource).digest('hex'),cases},null,2)+'\n');
} finally {await browser.close();}
