// Fixed offline font bytes; no installed-font or network dependency.
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {chromium} from 'playwright';
const directory='tests/fixtures/theme/fonts/';
const face=(family,file,weight=400,style='normal')=>({family,file:directory+file,weight,style});
const latin=['regular','bold','italic','bolditalic'].map((style,i)=>face('Fixture Alias',`latex-${style}.woff2`,i%2?700:400,i>=2?'italic':'normal'));
const han=face('Fixture Han','han.woff2');
const cases=[];
for(const [id,weight,style] of [['regular',400,'normal'],['bold',700,'normal'],['italic',400,'italic'],['bolditalic',700,'italic'],['intermediate',450,'normal']])
  cases.push({id,faces:latin,text:'Hello Muffin office AVATAR',css:`font-family:'Fixture Alias';font-size:20px;font-weight:${weight};font-style:${style}`,width:600});
cases.push({id:'heading',faces:latin,text:'Muffin Markdown Example',css:"font-family:'Fixture Alias';font-size:32px;font-weight:700",width:600,tag:'h1'});
cases.push({id:'inline-styles',faces:latin,text:'Hello Muffin and office',html:'Hello <strong>Muffin</strong> and <em>office</em>',markdown:'Hello **Muffin** and *office*',css:"font-family:'Fixture Alias';font-size:20px",width:600});
cases.push({id:'distinct-weight-faces',faces:[face('Many Weights','latex-regular.woff2',400),face('Many Weights','latex-bold.woff2',450),face('Many Weights','latex-italic.woff2',900)],text:'Hello Muffin office AVATAR',css:"font-family:'Many Weights';font-size:20px;font-weight:450",width:600});
cases.push({id:'missing-first',faces:latin,text:'Hello Muffin office AVATAR',css:"font-family:'Missing Muffin Font','fixture alias';font-size:20px",width:600});
cases.push({id:'quoted-comma',faces:[face('Fixture, Comma','latex-regular.woff2')],text:'Hello Muffin',css:"font-family:'Fixture, Comma';font-size:20px",width:600});
cases.push({id:'mixed-fallback',faces:[...latin,han],text:'Hello 中文 Hello 中文 Hello 中文',css:"font-family:'Fixture Alias','Fixture Han';font-size:20px",width:180});
for(const [id,weight,file] of [['descriptor-normal',400,'latex-bold.woff2'],['descriptor-bold',700,'latex-regular.woff2']])
  cases.push({id,faces:[face('Descriptor Alias',file,weight)],text:'Hello Muffin office AVATAR',css:`font-family:'Descriptor Alias';font-size:20px;font-weight:${weight}`,width:600});
for(const [id,parent,relative] of [['lighter',700,'lighter'],['bolder',700,'bolder'],['light-bolder',300,'bolder']])
  cases.push({id,faces:latin,text:'Hello Muffin',css:`font-family:'Fixture Alias';font-size:20px;font-weight:${relative}`,parent:`font-weight:${parent}`,width:600});
cases.push({id:'noto-synthetic-bold',faces:[{family:'Fixed Noto',file:'third_party/noto/fonts/NotoSans-Regular.ttf',weight:400,style:'normal'}],text:'Alpha node Beta node Outside',css:"font-family:'Fixed Noto';font-size:32px;font-weight:700",width:700});
const hashes={};for(const c of cases)for(const f of c.faces)hashes[f.file]=createHash('sha256').update(fs.readFileSync(f.file)).digest('hex');
const browser=await chromium.launch({headless:true});
try {
 const page=await browser.newPage({viewport:{width:800,height:500},deviceScaleFactor:1});
 for(const c of cases) {
  const rules=c.faces.map(f=>`@font-face{font-family:'${f.family}';src:url(data:font/woff2;base64,${fs.readFileSync(f.file).toString('base64')});font-weight:${f.weight};font-style:${f.style}}`).join('\n');
  await page.setContent(`<style>${rules}body{margin:0;font-weight:400;font-style:normal}#parent{${c.parent||''}}#sample{margin:0;line-height:1.5;${c.css};width:${c.width}px}</style><div id=parent><${c.tag||'p'} id=sample>${c.html||c.text}</${c.tag||'p'}></div>`);
  await page.evaluate(()=>document.fonts.ready);
  c.expected=await page.evaluate(()=>{
   const e=document.querySelector('#sample'),r=e.getBoundingClientRect(),s=getComputedStyle(e),characters=[];
   const walker=document.createTreeWalker(e,NodeFilter.SHOW_TEXT);
   for(let text=walker.nextNode();text;text=walker.nextNode())for(let i=0;i<text.length;i++) {const range=document.createRange();range.setStart(text,i);range.setEnd(text,i+1);const b=range.getBoundingClientRect();characters.push({x:b.x-r.x,y:b.y-r.y,width:b.width});}
   return {height:r.height,fontSize:s.fontSize,fontWeight:s.fontWeight,fontStyle:s.fontStyle,characters};
  });
 }
 fs.writeFileSync('tests/fixtures/theme/font-mapping-browser.json',JSON.stringify({browser:await browser.version(),hashes,cases})+'\n');
} finally {await browser.close();}
