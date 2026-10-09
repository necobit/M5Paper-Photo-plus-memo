#pragma once
// iPhone の Safari で開くアップロード画面
static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5Paper Memo</title>
<style>
:root{--bg:#f4f4f2;--fg:#1d1d1b;--card:#fff;--line:#d8d8d4;--accent:#1d1d1b;--muted:#6b6b66}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ececea;--card:#222220;--line:#3a3a37;--accent:#ececea;--muted:#9a9a94}}
*{box-sizing:border-box}
body{margin:0;font-family:-apple-system,system-ui,sans-serif;background:var(--bg);color:var(--fg);padding:16px;max-width:720px;margin:auto}
h1{font-size:20px;margin:4px 0 16px}
h2{font-size:15px;margin:0 0 10px;color:var(--muted);font-weight:600}
section{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
.btn{display:inline-block;border:1px solid var(--accent);background:var(--accent);color:var(--bg);border-radius:10px;padding:12px 16px;font-size:16px;font-weight:600;text-align:center;cursor:pointer}
.btn.sub{background:transparent;color:var(--fg);border-color:var(--line);padding:8px 10px;font-size:14px;font-weight:500}
.btn.wide{display:block;width:100%}
input[type=file]{display:none}
select,input[type=text],input[type=password]{width:100%;font-size:16px;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg);margin:6px 0}
#status{margin-top:10px;font-size:14px;color:var(--muted);min-height:1.2em}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.ph{border:1px solid var(--line);border-radius:10px;overflow:hidden;background:var(--bg)}
.ph img{display:block;width:100%;aspect-ratio:880/540;object-fit:cover;background:#fff}
.ph.cur{outline:3px solid var(--accent)}
.ph .row{display:flex;gap:6px;padding:6px}
.ph .row .btn{flex:1;padding:6px 0}
.muted{color:var(--muted);font-size:13px}
</style></head><body>
<h1>M5Paper 写真メモ</h1>

<section>
<h2>写真を送る</h2>
<label class="btn wide" for="file">写真を選ぶ</label>
<input id="file" type="file" accept="image/*" multiple>
<select id="rot">
<option value="0">縦長写真もそのまま表示</option>
<option value="1" selected>縦長写真は右に90°回して大きく表示</option>
<option value="-1">縦長写真は左に90°回して大きく表示</option>
</select>
<div id="status"></div>
</section>

<section>
<h2>写真一覧</h2>
<div id="list" class="grid"></div>
<p class="muted" id="space"></p>
<a class="btn sub" href="/api/snapshot.bmp" target="_blank">表示中の画面（メモ込み）を保存</a>
</section>

<section>
<h2>省電力</h2>
<p class="muted">最後の操作から数えます。スリープ中はタッチですぐ戻り、電源オフ後は本体のボタンで起動します（画面は表示されたまま）。</p>
<label class="muted">スリープまで</label>
<select id="sleep">
<option value="15">15 秒</option><option value="30">30 秒</option><option value="60">1 分</option>
<option value="120">2 分</option><option value="300">5 分</option>
</select>
<label class="muted">電源オフまで</label>
<select id="poweroff">
<option value="300">5 分</option><option value="600">10 分</option><option value="1800">30 分</option>
<option value="3600">1 時間</option><option value="0">切らない</option>
</select>
</section>

<section>
<h2>Wi-Fi 設定</h2>
<p class="muted">家の Wi-Fi に接続すると、iPhone を Wi-Fi に繋いだまま送れます。空欄で保存するとアクセスポイントモードに戻ります。</p>
<form method="post" action="/api/wifi">
<input type="text" name="ssid" placeholder="SSID" autocapitalize="off">
<input type="password" name="pass" placeholder="パスワード">
<button class="btn sub" type="submit">保存して再起動</button>
</form>
</section>

<script>
const W=880,H=540;
const $=s=>document.querySelector(s);
const status=t=>$('#status').textContent=t;

async function loadImage(file){
  const url=URL.createObjectURL(file);
  const img=new Image();
  img.src=url;
  await img.decode();
  URL.revokeObjectURL(url);
  return img;
}

async function toJpeg(file,rot){
  const img=await loadImage(file);
  const w=img.naturalWidth,h=img.naturalHeight;
  const turn=rot!==0&&h>w;
  const sw=turn?h:w, sh=turn?w:h;
  const s=Math.min(W/sw,H/sh);
  const cw=Math.round(sw*s), ch=Math.round(sh*s);
  const c=document.createElement('canvas');
  c.width=cw;c.height=ch;
  const g=c.getContext('2d');
  g.fillStyle='#fff';g.fillRect(0,0,cw,ch);
  g.imageSmoothingQuality='high';
  if(turn&&rot>0){g.translate(cw,0);g.rotate(Math.PI/2);g.drawImage(img,0,0,ch,cw);}
  else if(turn){g.translate(0,ch);g.rotate(-Math.PI/2);g.drawImage(img,0,0,ch,cw);}
  else g.drawImage(img,0,0,cw,ch);
  return new Promise(r=>c.toBlob(r,'image/jpeg',0.92));
}

function post(url,body,onProgress){
  return new Promise((res,rej)=>{
    const x=new XMLHttpRequest();
    x.open('POST',url);
    if(onProgress)x.upload.onprogress=e=>onProgress(e.loaded/e.total);
    x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}
      x.status==200?res(j):rej(new Error(j.error||('HTTP '+x.status)))};
    x.onerror=()=>rej(new Error('通信エラー'));
    x.send(body);
  });
}

$('#file').addEventListener('change',async e=>{
  const files=[...e.target.files];
  const rot=+$('#rot').value;
  for(let i=0;i<files.length;i++){
    const tag=files.length>1?`(${i+1}/${files.length}) `:'';
    try{
      status(tag+'変換中…');
      const blob=await toJpeg(files[i],rot);
      const fd=new FormData();
      fd.append('file',blob,'photo.jpg');
      await post('/api/upload',fd,p=>status(tag+'送信中… '+Math.round(p*100)+'%'));
      status(tag+'表示中…');
    }catch(err){status(tag+'失敗: '+err.message);return;}
  }
  status('送信しました');
  e.target.value='';
  refresh();
});

function pickOption(sel,v){
  if(![...sel.options].some(o=>o.value==v)){const o=new Option(v+' 秒',v);sel.add(o);}
  sel.value=v;
}
for(const id of ['sleep','poweroff']){
  $('#'+id).addEventListener('change',async e=>{
    try{await post('/api/settings?'+id+'='+e.target.value);status('設定を保存しました');}
    catch(err){status('設定の保存に失敗: '+err.message);}
  });
}

async function refresh(){
  const j=await (await fetch('/api/list')).json();
  pickOption($('#sleep'),j.sleep);
  pickOption($('#poweroff'),j.poweroff);
  const list=$('#list');
  list.innerHTML='';
  if(!j.photos.length)list.innerHTML='<p class="muted">まだ写真がありません</p>';
  for(const id of j.photos){
    const d=document.createElement('div');
    d.className='ph'+(id==j.current?' cur':'');
    d.innerHTML=`<img loading="lazy" src="/api/thumb?id=${id}" alt="写真 ${id}"><div class="row">
      <span class="btn sub" data-a="show">表示</span><span class="btn sub" data-a="delete">削除</span></div>`;
    d.querySelector('[data-a=show]').onclick=async()=>{await post('/api/show?id='+id);refresh();};
    d.querySelector('[data-a=delete]').onclick=async()=>{
      if(!confirm('この写真とメモを削除しますか？'))return;
      await post('/api/delete?id='+id);refresh();};
    list.appendChild(d);
  }
  const mb=b=>(b/1048576).toFixed(1)+'MB';
  $('#space').textContent=`空き ${mb(j.total-j.used)} / ${mb(j.total)}（1枚あたり約0.25MB）`;
}
refresh();
</script>
</body></html>
)HTML";
