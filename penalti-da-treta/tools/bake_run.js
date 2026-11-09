const {chromium}=require('playwright'); const fs=require('fs');
const jobs=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
(async()=>{const b=await chromium.launch({args:['--use-gl=swiftshader','--enable-webgl','--ignore-gpu-blocklist']});const p=await b.newPage({viewport:{width:1700,height:1200}});
p.on('console',m=>{const t=m.text(); if(!/GPU stall|GroupMarker|fallback/.test(t)) console.log(t.slice(0,300))});
await p.goto('http://localhost:8765/bake.html#'+encodeURIComponent(JSON.stringify(jobs)));await p.waitForFunction('window.DONE',null,{timeout:600000});
const o=await p.evaluate('window.OUT'); if(o) fs.writeFileSync(process.argv[3],o);
const im=await p.evaluate('window.IMG'); if(im) fs.writeFileSync(process.argv[3].replace('.json','.jpg'),Buffer.from(im.split(',')[1],'base64'));
await b.close();})();
