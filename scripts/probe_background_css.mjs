// Offline Chromium oracle for the shared native background painter. No fonts,
// network assets or VLOOK-specific selectors; CI consumes the committed JSON.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {chromium} from 'playwright';
import {PNG} from 'pngjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="10" height="6" viewBox="0 0 10 6"><path fill="red" d="M0 0h5v6H0z"/><path fill="blue" d="M5 0h5v6H5z"/></svg>';
const url = `url("data:image/svg+xml;base64,${Buffer.from(svg).toString('base64')}")`;
const base = `background-color:#a2b8c4;background-image:${url};`;
const svgUrl = source => `url("data:image/svg+xml;base64,${Buffer.from(source).toString('base64')}")`;
const cases = [
  ['svg-no-viewbox', `background:#a2b8c4 ${svgUrl(svg.replace(' viewBox="0 0 10 6"',''))} left top/60px 55px no-repeat`],
  ['svg-no-size', `background:#a2b8c4 ${svgUrl(svg.replace(' width="10" height="6"',''))} center no-repeat`],
  ['svg-aspect-none', `background:#a2b8c4 ${svgUrl(svg.replace('<svg ','<svg preserveAspectRatio="none" '))} center/60px 55px no-repeat`],
  ['svg-aspect-slice', `background:#a2b8c4 ${svgUrl(svg.replace('<svg ','<svg preserveAspectRatio="xMinYMin slice" '))} center/60px 55px no-repeat`],
  ['space-x', base + 'background-size:27px 18px;background-repeat:space no-repeat;background-position:right center'],
  ['space-both', base + 'background-size:27px 19px;background-repeat:space;background-position:70% 80%'],
  ['space-single', base + 'background-size:60px 55px;background-repeat:space;background-position:75% 25%'],
  ['space-too-large', base + 'background-size:120px 100px;background-repeat:space;background-position:center'],
  ['round-x-auto', base + 'background-size:37px auto;background-repeat:round no-repeat;background-position:left center'],
  ['round-y-auto', base + 'background-size:auto 31px;background-repeat:no-repeat round;background-position:center top'],
  ['round-both', base + 'background-size:37px 31px;background-repeat:round'],
  ['round-space', base + 'background-size:37px 21px;background-repeat:round space'],
  ['space-round', base + 'background-size:27px 31px;background-repeat:space round'],
  ['negative-repeat', base + 'background-size:27px 21px;background-repeat:repeat;background-position:-19px -13px'],
  ['round-position', base + 'background-size:37px 31px;background-repeat:round;background-position:40% 20%'],
  ['space-gradient', 'background:#a2b8c4 linear-gradient(90deg,red,blue);background-size:27px 19px;background-repeat:space'],
  ['round-gradient', 'background:#a2b8c4 linear-gradient(90deg,red,blue);background-size:37px auto;background-repeat:round no-repeat'],
  ['origin-clip', base + 'padding:7px;border:3px solid #345678;background-origin:content-box;background-clip:padding-box;background-size:23px 18px;background-repeat:space round'],
  ['border-space', base + 'padding:11px;border:5px solid transparent;background-origin:padding-box;background-clip:border-box;background-size:23px 18px;background-repeat:space'],
  ['none-list', 'background-color:#90b080;background-image:none,linear-gradient(#c03080,#c03080),linear-gradient(#a09020,#a09020);background-size:27px 21px,41px 33px;background-repeat:space,round;background-blend-mode:screen,multiply'],
  ['transparent-blend', 'background-color:rgba(20,80,180,.4);background-image:linear-gradient(rgba(210,70,40,.6),rgba(210,70,40,.6));background-blend-mode:multiply'],
  ['blend-gap', 'background-color:#345678;background-image:linear-gradient(#c04090,#c04090);background-size:27px 19px;background-repeat:space;background-blend-mode:screen'],
  ['isolated-transparent', 'background-image:linear-gradient(rgba(180,60,120,.5),rgba(180,60,120,.5));background-blend-mode:multiply'],
];
for (const mode of ['normal','multiply','screen','overlay','darken','lighten','color-dodge','color-burn',
                     'hard-light','soft-light','difference','exclusion','hue','saturation','color','luminosity'])
  cases.push([`blend-${mode}`, `background-color:#4080c0;background-image:linear-gradient(#c06090,#c06090);background-blend-mode:${mode}`]);
for (const mode of ['hue','saturation','color','luminosity'])
  cases.push([`alpha-${mode}`, `background-color:rgba(64,128,192,.4);background-image:linear-gradient(rgba(192,96,144,.6),rgba(192,96,144,.6));background-blend-mode:${mode}`]);

const browser = await chromium.launch({headless:true, ...(process.env.CHROME_EXECUTABLE ? {executablePath:process.env.CHROME_EXECUTABLE} : {})});
const references = [];
try {
  for (const dpr of [1,1.25,2]) {
    const page = await browser.newPage({viewport:{width:120,height:108},deviceScaleFactor:dpr});
    for (const [id, css] of cases) {
      await page.setContent(`<!doctype html><style>html,body{margin:0;background:transparent}#box{position:absolute;left:8px;top:8px;width:96px;height:84px;box-sizing:border-box;${css}}</style><div id="box"></div>`);
      const raster = await page.screenshot({omitBackground:true});
      const png = PNG.sync.read(raster);
      if(process.env.MUFFIN_BACKGROUND_PROBE_OUTPUT) {
        fs.mkdirSync(process.env.MUFFIN_BACKGROUND_PROBE_OUTPUT,{recursive:true});
        fs.writeFileSync(path.join(process.env.MUFFIN_BACKGROUND_PROBE_OUTPUT,`${id}-${dpr}.png`),raster);
      }
      const pixels = [];
      const mask = [];
      // Include padding, clip and tile boundaries without relying on fonts or
      // the platform's text antialiasing. Retain physical pixel coordinates.
      for (let y=2;y<png.height;y+=4) for(let x=2;x<png.width;x+=4) {
        const at = (y*png.width+x)*4;
        pixels.push(...png.data.subarray(at,at+4));
        let stable=true;
        // Edge antialiasing of SVG/texture boundaries differs between Skia and
        // Qt. Compare interiors and smooth gradients; geometry errors larger
        // than one physical pixel still reach these samples.
        for(let dy=-1;dy<=1;dy++)for(let dx=-1;dx<=1;dx++) {
          const near=((y+dy)*png.width+x+dx)*4;
          for(let channel=0;channel<4;channel++)
            if(Math.abs(png.data[at+channel]-png.data[near+channel])>12)stable=false;
        }
        mask.push(stable?1:0);
      }
      references.push({id,dpr,css,sampleStart:2,sampleStep:4,pixels:Buffer.from(pixels).toString('base64'),mask:Buffer.from(mask).toString('base64')});
    }
    await page.close();
  }
  fs.writeFileSync(path.join(root,'tests/fixtures/theme/background-browser.json'),
    JSON.stringify({browser:await browser.version(),width:120,height:108,box:[8,8,96,84],cases:references})+'\n');
} finally {await browser.close();}
