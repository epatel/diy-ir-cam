// The page served at "/" (the captive-portal sign-in page). Self-contained:
// no external files, since the phone has no internet on the camera's network.
// It draws the 32x24 frame with the same ironbow palette as the screen,
// sends option changes to /set, and lists the SD card snapshots
// (/files.json, /file, /save, /delete).

#pragma once

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IR-Cam</title>
<style>
:root{--bg:#0d0f10;--panel:#181b1d;--line:#2a2f33;--text:#e8e6e1;--muted:#8a8f93;--accent:#00e5ff;--hold:#ff4d4d}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,-apple-system,sans-serif}
main{max-width:520px;margin:0 auto;padding:16px}
h1{font-size:17px;margin:0 0 12px;display:flex;justify-content:space-between;align-items:baseline}
h1 small{color:var(--muted);font-weight:400;font-size:13px}
.view{position:relative;background:#000;border-radius:8px;overflow:hidden}
canvas{display:block;width:100%;height:auto}
.badge{position:absolute;right:10px;top:10px;background:var(--hold);color:#fff;font-weight:700;font-size:13px;padding:2px 8px;border-radius:4px;display:none}
.bar{height:10px;border-radius:3px;margin:12px 0 4px}
.range{display:flex;justify-content:space-between;color:var(--muted);font-variant-numeric:tabular-nums;font-size:13px}
.read{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:14px 0}
.read div{background:var(--panel);border-radius:8px;padding:10px 12px}
.read span{display:block;color:var(--muted);font-size:12px}
.read b{font-size:26px;font-variant-numeric:tabular-nums;font-weight:600}
#spotBox b{color:var(--accent)}
.label{color:var(--muted);font-size:12px;margin:14px 0 6px}
.seg{display:grid;grid-template-columns:repeat(3,1fr);border:1px solid var(--line);border-radius:8px;overflow:hidden}
.seg button+button{border-left:1px solid var(--line)}
button{font:inherit;color:var(--text);background:var(--panel);border:0;padding:11px 8px;cursor:pointer}
button[aria-pressed=true]{background:var(--accent);color:#000;font-weight:600}
.row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;margin-top:10px}
.row button{border:1px solid var(--line);border-radius:8px}
#freeze[aria-pressed=true]{background:var(--hold);color:#fff}
.status{color:var(--muted);font-size:12px;margin-top:14px;min-height:1.4em}
#files{list-style:none;margin:0;padding:0;border:1px solid var(--line);border-radius:8px;overflow:hidden}
#files li{display:flex;align-items:center;gap:10px;padding:8px 12px;background:var(--panel)}
#files li+li{border-top:1px solid var(--line)}
#files .n{flex:1;font-variant-numeric:tabular-nums}
#files .s{color:var(--muted);font-size:12px}
#files a{color:var(--accent);text-decoration:none}
#files button{padding:4px 8px;border:1px solid var(--line);border-radius:6px;font-size:13px}
#files button.sure{background:var(--hold);color:#fff}
#files .empty{color:var(--muted)}
</style></head><body><main>
<h1>IR-Cam <small id="fps">connecting</small></h1>
<div class="view"><canvas id="img" width="640" height="480"></canvas><div class="badge" id="hold">HOLD</div></div>
<canvas class="bar" id="bar" width="256" height="1" style="width:100%"></canvas>
<div class="range"><span id="min">-</span><span id="max">-</span></div>
<div class="read">
  <div><span>centre</span><b id="centre">-</b></div>
  <div id="spotBox"><span id="spotLabel">spot</span><b id="spotT">-</b></div>
</div>
<div class="label">Spot marker</div>
<div class="seg" role="group" aria-label="Spot marker">
  <button data-spot="0">Off</button><button data-spot="1">Hot</button><button data-spot="2">Cold</button>
</div>
<div class="row">
  <button id="freeze" aria-pressed="false">Freeze</button>
  <button id="sdsave">Save to SD</button>
  <button id="save">Save PNG</button>
</div>
<div class="status" id="status"></div>
<div class="label">Snapshots on the SD card</div>
<ul id="files"><li class="empty">Loading…</li></ul>
</main>
<script>
(()=>{
const SW=32,SH=24,$=id=>document.getElementById(id);
const stops=[[0,0,0,0],[.15,32,0,140],[.35,145,0,160],[.55,230,50,50],[.75,250,150,0],[.9,255,230,60],[1,255,255,255]];
const pal=[];
for(let i=0;i<256;i++){const t=i/255;let s=0;while(s<stops.length-2&&t>stops[s+1][0])s++;
  const a=stops[s],b=stops[s+1],u=(t-a[0])/(b[0]-a[0]);pal.push([0,1,2].map(k=>a[k+1]+u*(b[k+1]-a[k+1])|0));}
const bar=$('bar').getContext('2d'),bd=bar.createImageData(256,1);
pal.forEach((c,i)=>bd.data.set([...c,255],i*4));bar.putImageData(bd,0,0);

const small=document.createElement('canvas');small.width=SW;small.height=SH;
const sctx=small.getContext('2d'),sd=sctx.createImageData(SW,SH);
const cv=$('img'),ctx=cv.getContext('2d'),W=cv.width,H=cv.height;
let last=null,lastSeq=-1,busy=false,saveSeq=null;

function draw(f){
  const k=255/Math.max(f.max-f.min,1);
  for(let i=0;i<SW*SH;i++){let v=((f.t[i]/10-f.min)*k)|0;v=v<0?0:v>255?255:v;sd.data.set([...pal[v],255],i*4);}
  sctx.putImageData(sd,0,0);
  ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';
  ctx.drawImage(small,0,0,W,H);
  ctx.strokeStyle='#fff';ctx.lineWidth=2;ctx.beginPath();
  ctx.moveTo(W/2-30,H/2);ctx.lineTo(W/2+30,H/2);ctx.moveTo(W/2,H/2-30);ctx.lineTo(W/2,H/2+30);ctx.stroke();
  if(f.spot){
    const x=(f.spotX+.5)*W/SW,y=(f.spotY+.5)*H/SH;
    ctx.lineWidth=7;ctx.strokeStyle='#000';ctx.beginPath();ctx.arc(x,y,18,0,7);ctx.stroke();
    ctx.lineWidth=3;ctx.strokeStyle='#00ffff';ctx.beginPath();ctx.arc(x,y,18,0,7);ctx.stroke();
  }
  $('min').textContent=f.min.toFixed(1)+' °C';$('max').textContent=f.max.toFixed(1)+' °C';
  $('centre').textContent=f.centre.toFixed(1)+' °C';
  $('spotLabel').textContent=['spot off','hot spot','cold spot'][f.spot];
  $('spotT').textContent=f.spot?f.spotT.toFixed(1)+' °C':'-';
}
function sync(f){
  document.querySelectorAll('[data-spot]').forEach(b=>b.setAttribute('aria-pressed',String(+b.dataset.spot===f.spot)));
  $('freeze').setAttribute('aria-pressed',String(f.frozen));
  $('freeze').textContent=f.frozen?'Frozen, tap to resume':'Freeze';
  $('hold').style.display=f.frozen?'block':'none';
  $('fps').textContent=f.frozen?'frozen':f.fps.toFixed(1)+' fps';
}
async function poll(){
  try{
    const r=await fetch('/frame.json',{cache:'no-store'});const f=await r.json();
    if(f.seq!==lastSeq){lastSeq=f.seq;last=f;draw(f);}
    sync(f);
    if(f.saveSeq!==saveSeq){
      if(saveSeq!==null)$('status').textContent=f.saved>=0?'Saved IMG_'+String(f.saved).padStart(4,'0'):'Could not save: no SD card, or it is full';
      saveSeq=f.saveSeq;files();
    }else if($('status').textContent.startsWith('Lost'))$('status').textContent='';
    setTimeout(poll,80);
  }catch(e){$('status').textContent='Lost connection to the camera, retrying…';$('fps').textContent='offline';setTimeout(poll,1000);}
}
async function set(q){if(busy)return;busy=true;try{await fetch('/set?'+q,{cache:'no-store'});}catch(e){}busy=false;}
document.querySelectorAll('[data-spot]').forEach(b=>b.onclick=()=>set('spot='+b.dataset.spot));
$('freeze').onclick=()=>set('freeze='+(last&&last.frozen?0:1));
$('save').onclick=()=>{
  const a=document.createElement('a');a.href=cv.toDataURL('image/png');
  a.download='ir-cam-'+new Date().toISOString().replace(/[:.]/g,'-').slice(0,19)+'.png';
  document.body.appendChild(a);a.click();a.remove();
  $('status').textContent='If nothing downloaded, open http://192.168.4.1 in the browser instead of the sign-in window.';
};
$('sdsave').onclick=()=>fetch('/save',{method:'POST'}).catch(()=>{});

// One row per snapshot (IMG_nnnn.png + .csv), newest first
async function files(){
  let list;
  try{list=await (await fetch('/files.json',{cache:'no-store'})).json();}catch(e){return;}
  const g={};
  list.forEach(f=>{const m=/^(.+)\.(png|bmp|csv)$/i.exec(f.n);if(m)(g[m[1]]=g[m[1]]||{})[m[2].toLowerCase()]=f;});
  const ul=$('files');ul.textContent='';
  const names=Object.keys(g).sort().reverse();
  if(!names.length){const li=document.createElement('li');li.className='empty';li.textContent='None yet';ul.append(li);}
  names.forEach(n=>{
    const li=document.createElement('li'),el=(t,c,txt)=>{const e=document.createElement(t);if(c)e.className=c;if(txt)e.textContent=txt;li.append(e);return e;};
    el('span','n',n);
    const kb=Object.values(g[n]).reduce((a,f)=>a+f.s,0)/1024;
    el('span','s',kb.toFixed(0)+' KB');
    ['png','bmp','csv'].forEach(x=>{if(g[n][x]){const a=el('a','',x.toUpperCase());a.href='/file?name='+encodeURIComponent(g[n][x].n);a.download=g[n][x].n;}});
    const del=el('button','','Delete');
    del.onclick=async()=>{
      if(!del.classList.contains('sure')){del.classList.add('sure');del.textContent='Sure?';setTimeout(()=>{del.classList.remove('sure');del.textContent='Delete';},3000);return;}
      try{await fetch('/delete?name='+encodeURIComponent(n),{method:'POST'});}catch(e){}
      files();
    };
    ul.append(li);
  });
}
poll();
})();
</script></body></html>)HTML";
