// Offline browser oracle; CI uses the fixed pixels/rectangles/text witnesses.
import fs from 'node:fs';
import {chromium} from 'playwright';
import {PNG} from 'pngjs';
const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="12" height="8"><path fill="white" d="M0 0h6v8H0z"/></svg>';
const image = `url("data:image/svg+xml;base64,${Buffer.from(svg).toString('base64')}")`;
const effects = [
 ['ordered-a','filter:brightness(.6) contrast(1.7)'],
 ['ordered-b','filter:contrast(1.7) brightness(.6)'],
 ['repeated','filter:invert(.3) invert(.4) saturate(.7)'],
 ['hue','filter:hue-rotate(75deg) saturate(1.3)'],
 ['gray-sepia','filter:grayscale(.6) sepia(.4) opacity(.7)'],
 ['blur','filter:blur(2px)'],
 ['shadow','filter:drop-shadow(5px 3px 1px #386eaa)'],
 ['shadow-order','filter:drop-shadow(-4px 3px 0 red) invert(1)'],
 ['far-shadow','transform:translateX(-12345px);filter:drop-shadow(12345px 0 #386eaa)'],
 ['far-shadow-negative','transform:translateX(100000px);filter:drop-shadow(-100000px 0 #386eaa)'],
 ['far-shadow-blur','transform:translateX(-12345px);filter:drop-shadow(12345px 0 2px #386eaa)'],
 ['far-shadow-chain','transform:translateX(-12345px);filter:drop-shadow(12345px 0 #386eaa) drop-shadow(4px 3px 1px red) invert(.3)'],
 ['far-shadow-mask','transform:translateX(-12345px);filter:drop-shadow(12345px 0 #386eaa);mask-image:linear-gradient(to right,transparent,black);opacity:.6'],
 ['backdrop-ordered','backdrop-filter:invert(.4) contrast(1.3);background:rgba(160,60,120,.3)'],
 ['backdrop-blur','backdrop-filter:blur(2px);background:rgba(160,60,120,.3)'],
 ['mask-alpha','mask-image:linear-gradient(to right,transparent,black)'],
 ['mask-luminance','mask-image:linear-gradient(#808080,#808080);mask-mode:luminance'],
 ['mask-svg',`mask:${image} center / 24px 16px no-repeat`],
 ['mask-vendor',`-webkit-mask:${image} center / 24px 16px no-repeat`],
 ['mask-content',`mask-image:${image};mask-origin:content-box;mask-clip:content-box;mask-size:12px 8px;mask-repeat:round space`],
 ['mask-none','mask-image:none'],
 ['mask-missing',"mask-image:url('file:///missing-muffin-mask.png')"],
 ['mask-filter-opacity','filter:invert(.3);opacity:.6;mask-image:linear-gradient(to right,transparent,black)'],
 ...['add','subtract','intersect','exclude'].map(mode=>['mask-'+mode,
   'mask-image:linear-gradient(to right,black 50%,transparent 50%),linear-gradient(to bottom,black 50%,transparent 50%);mask-composite:'+mode]),
 ['mask-none-intersect','mask-image:none,linear-gradient(black,black);mask-composite:intersect'],
];
const positions = [
 ['stretch','left:10%;right:15%;top:5px;bottom:7px'],
 ['stretch-content','left:12px;right:20px;top:10%;bottom:20%;padding:3px;border:2px solid;box-sizing:content-box'],
 ['max-min','left:10px;right:10px;top:3px;bottom:3px;max-width:90px;min-height:35px'],
 ['explicit-content','right:10%;bottom:10%;width:50%;height:25%;padding:3px;border:2px solid;box-sizing:content-box'],
 ['explicit-border','right:10%;bottom:10%;width:50%;height:25%;padding:3px;border:2px solid;box-sizing:border-box'],
 ['minimum-wins','left:0;right:0;top:0;bottom:0;max-width:40px;min-width:60px;max-height:20px;min-height:30px'],
 ['negative-insets','left:-8px;right:-4px;top:-3px;bottom:-2px'],
 ['margins','left:5%;right:5%;top:5%;bottom:5%;margin:2px 4px'],
];
const counters = [
 {id:'host-before',markdown:'Alpha\n\nBeta',html:'<p id="a">Alpha</p><p id="b">Beta</p>',css:'body{counter-reset:n 2}body::before{counter-increment:n 2;content:""}p{counter-increment:n}p::before{content:counter(n)}',before:['5','6']},
 {id:'sibling-without-parent',markdown:'Alpha\n\nBeta\n\nGamma',html:'<p id="a">Alpha</p><p id="b">Beta</p><p id="c">Gamma</p>',css:'p:first-child{counter-reset:n 5}p{counter-increment:n}p::before{content:counter(n)}',before:['6','7','8']},
 {id:'pseudo-local',markdown:'Alpha\n\nBeta',html:'<p id="a">Alpha</p><p id="b">Beta</p>',css:'p::before{counter-increment:n;content:counter(n)}',before:['1','1']},
 {id:'nested',markdown:'Alpha\n\n> Beta\n>\n> > Gamma\n>\n> Delta\n\nEpsilon',html:'<p id="a">Alpha</p><blockquote><p id="b">Beta</p><blockquote><p id="c">Gamma</p></blockquote><p id="d">Delta</p></blockquote><p id="e">Epsilon</p>',css:'#write,blockquote{counter-reset:n}p{counter-increment:n}p::before{content:counters(n,".") ":"}',before:['1:','1.1:','1.1.1:','1.2:','2:']},
 {id:'set-and-case',markdown:'Alpha\n\nBeta\n\nGamma',html:'<p id="a">Alpha</p><p id="b">Beta</p><p id="c">Gamma</p>',css:'#write{counter-reset:N 3 n 8}p{counter-increment:N 2 n -1}p:nth-child(2){counter-set:N 10}p::before{content:counter(N) "/" counter(n)}',before:['5/7','10/6','12/5']},
 {id:'pseudo-order',markdown:'Alpha\n\n> Beta\n\nGamma',html:'<p id="a">Alpha</p><blockquote id="b"><p id="c">Beta</p></blockquote><p id="d">Gamma</p>',css:'#write{counter-reset:n}p{counter-increment:n}blockquote::before{counter-increment:n;content:counter(n)}blockquote::after{counter-increment:n;content:counter(n)}p::before{content:counter(n)}',before:['1','2','3','5'],after:{b:'4'}},
 {id:'sibling-reset',markdown:'Alpha\n\nBeta\n\nGamma',html:'<p id="a">Alpha</p><p id="b">Beta</p><p id="c">Gamma</p>',css:'#write{counter-reset:n 2}p:first-child{counter-reset:n 5}p{counter-increment:n}p::before{content:counters(n,".")}',before:['2.6','3','4']},
 {id:'hidden',markdown:'Alpha\n\nBeta\n\nGamma',html:'<p id="a">Alpha</p><p id="b">Beta</p><p id="c">Gamma</p>',css:'#write{counter-reset:n}p{counter-increment:n}p:nth-child(2){display:none}p::before{content:counter(n)}',before:['1',null,'2']},
 {id:'inline',markdown:'**Alpha** and *Beta*\n\n**Gamma**',html:'<p><span md-inline="strong"><strong id="a">Alpha</strong></span> and <span md-inline="em"><em id="b">Beta</em></span></p><p><span md-inline="strong"><strong id="c">Gamma</strong></span></p>',css:'#write{counter-reset:n}strong,em{counter-increment:n}strong::before,em::before{content:counter(n) ":"}',before:['1:','2:','3:'],inline:true},
];
const browser = await chromium.launch({headless:true});
const reference = {browser:await browser.version(),width:96,height:80,effects:[],positions:[],counters};
try {
 for (const dpr of [1,1.25,2]) {
  const page = await browser.newPage({viewport:{width:96,height:80},deviceScaleFactor:dpr});
  for (const [id,css] of effects) {
   await page.setContent(`<style>html,body{margin:0;background:transparent}#backdrop{position:absolute;inset:0;background:linear-gradient(to right,#3678a0 50%,#ac5268 50%)}#box{position:absolute;left:16px;top:16px;width:64px;height:48px;box-sizing:border-box;padding:4px;border:2px solid #789abc;background:#a03c78;${css}}#child{position:absolute;left:12px;top:8px;width:24px;height:20px;background:#2c74a8}</style>${id.startsWith('backdrop')?'<div id="backdrop"></div>':''}<div id="box"><div id="child"></div></div>`);
   const png=PNG.sync.read(await page.screenshot({omitBackground:true}));
   const pixels=[],stable=[];
   for(let y=3;y<png.height-2;y+=4)for(let x=3;x<png.width-2;x+=4) {
    const at=(y*png.width+x)*4;pixels.push(...png.data.subarray(at,at+4));
    let valid=true;
    for(let dy=-1;dy<=1;dy++)for(let dx=-1;dx<=1;dx++)for(let c=0;c<4;c++)
     if(Math.abs(png.data[at+c]-png.data[((y+dy)*png.width+x+dx)*4+c])>12)valid=false;
    stable.push(valid?1:0);
   }
   reference.effects.push({id,css,dpr,pixels:Buffer.from(pixels).toString('base64'),stable:Buffer.from(stable).toString('base64')});
  }
  await page.close();
 }
 const page = await browser.newPage({viewport:{width:400,height:300}});
 for(const [id,css] of positions) {
  await page.setContent(`<style>body{margin:0}#host{position:relative;box-sizing:border-box;width:224px;height:124px;padding:10px;border:2px solid}#host::before{content:"";position:absolute;${css}}</style><div id="host"></div>`);
  const geometry=await page.evaluate(()=>{
   const host=document.querySelector('#host'),cs=getComputedStyle(host,'::before');
   const probe=document.createElement('div');probe.style.cssText=cs.cssText;
   for(const name of cs)probe.style.setProperty(name,cs.getPropertyValue(name));
   host.append(probe);const r=probe.getBoundingClientRect();return [r.x-2,r.y-2,r.width,r.height];
  });
  reference.positions.push({id,css,rect:geometry});
 }
 // A browser screenshot of counters must equal literal witness strings. This
 // verifies resolved text, which getComputedStyle(content) does not expose.
 for(const c of counters) {
  await page.setContent(`<style>body{margin:0}#write{font:16px monospace}p,blockquote{margin:0} ${c.css}</style><div id="write">${c.html}</div>`);
  const first=PNG.sync.read(await page.screenshot());
  await page.addStyleTag({content:c.before.map((text,i)=>text===null?'':`#${String.fromCharCode(97+i)}::before{content:${JSON.stringify(text)}!important}`).join('')+Object.entries(c.after||{}).map(([id,text])=>`#${id}::after{content:${JSON.stringify(text)}!important}`).join('')});
  const second=PNG.sync.read(await page.screenshot());
  if(!first.data.equals(second.data))throw new Error('counter witness disagrees with browser: '+c.id);
 }
 await page.close();
 fs.writeFileSync('tests/fixtures/theme/effects-browser.json',JSON.stringify(reference)+'\n');
} finally {await browser.close();}
