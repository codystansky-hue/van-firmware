// pages.h -- the hub's web pages: the dashboard (/) and the setup page (/setup, with %PLACEHOLDERS% filled in hub.ino).
#pragma once

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Van</title>
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-title" content="Van">
<style>
:root{--bg:#eef0f3;--card:#fff;--fg:#15181c;--mut:#5f6670;--line:#dde1e6;--btn:#f3f4f6;--ok:#1a7f37;--warn:#a85400;--bad:#c62828;--bar:#2e9e5b}
@media(prefers-color-scheme:dark){:root{--bg:#101215;--card:#1b1e23;--fg:#e9eaec;--mut:#9aa1aa;--line:#2b3037;--btn:#262a31;--ok:#5cc178;--warn:#f0a050;--bad:#ef6b6b;--bar:#3fb06d}}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--fg);max-width:520px;margin:0 auto;padding:10px 12px}
.c{background:var(--card);border-radius:12px;padding:12px 14px;margin:10px 0}
h2{font-size:13px;font-weight:600;color:var(--mut);margin:0 0 4px;text-transform:uppercase;letter-spacing:.05em}
.big{font-size:38px;font-weight:600;line-height:1.15}.big small{font-size:17px;color:var(--mut);font-weight:400}
.r{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-top:1px solid var(--line)}.r span:last-child{text-align:right}
.m{color:var(--mut);font-size:14px;margin:3px 0}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.bar{height:8px;border-radius:4px;background:var(--line);overflow:hidden;margin:6px 0 4px}.bar i{display:block;height:100%;background:var(--bar)}
.b{display:flex;gap:6px;margin-top:8px}.b button{flex:1;font-size:17px;padding:12px 2px;border-radius:10px;border:1px solid var(--line);background:var(--btn);color:var(--fg)}
.b button.on{border-color:var(--bar);font-weight:600}summary{font-weight:600;padding:2px 0}
input,select{font-size:16px;width:100%;box-sizing:border-box;margin:4px 0;padding:7px;border-radius:8px;border:1px solid var(--line);background:var(--btn);color:var(--fg)}
a{color:inherit}
</style></head><body>
<div class="c"><h2>Battery</h2><div id="bat">...</div></div>
<div class="c"><h2>Solar</h2><div id="sol">...</div></div>
<div class="c"><h2>Water</h2><div id="wat">...</div></div>
<div class="c"><h2>MaxxFan</h2><div id="fan">...</div>
<div class="b"><button onclick="mf('lid=open')">Open</button><button onclick="mf('lid=close')">Close</button><button onclick="mf('lid=stop')">Stop</button></div>
<div id="fanb" hidden><div class="b" id="sp"></div><div class="b" id="dr"></div></div></div>
<details class="c"><summary>Wi-Fi</summary><div class="m" id="net">...</div>
<div class="m">Save the van router (Starlink) here and keep the phone on it: <b>http://van.local</b>. The VanHub hotspot then
turns off, and comes back by itself when the router is off for 20 s.</div>
<select id="nets" onchange="ss.value=this.value" hidden></select>
<input id="ss" placeholder="network name" autocapitalize="off" autocorrect="off" spellcheck="false">
<input id="pw" type="password" placeholder="password" autocapitalize="off" autocorrect="off">
<div class="b"><button onclick="scan()">scan</button><button onclick="join()">save + join</button><button onclick="forget()">forget</button></div>
<div class="m" id="wm"></div></details>
<p class="m" id="ft"></p>
<script>
const $=i=>document.getElementById(i);let U='us',first=true,fanSet=null;
const n=(v,d=1)=>v==null||isNaN(v)?'--':Number(v).toFixed(d);
const esc=s=>String(s).replace(/[<>&"]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]));
const tmp=c=>c==null?'--':U=='us'?Math.round(c*1.8+32)+'°F':Math.round(c)+'°C';
const vol=l=>l==null?'--':U=='us'?n(l/3.785,1)+' gal':n(l,0)+' L';
const fresh=a=>a!=null&&a<120;
const old=a=>a==null?'':a<120?'':' <span class="warn">('+(a<5400?Math.round(a/60)+' min':Math.round(a/3600)+' h')+' old)</span>';
const bar=p=>'<div class="bar"><i style="width:'+Math.max(0,Math.min(100,p||0))+'%"></i></div>';
function dur(m){let h=Math.floor(m/60);return h>=48?Math.round(h/24)+' days':h?h+' h '+m%60+' min':m+' min'}
function battery(r){
 let sh=r.victron.find(d=>d.saved&&d.type==2&&d.age!=null),rb=r.renogy,h='';
 let soc=sh&&fresh(sh.age)?sh.soc:rb.age!=null&&fresh(rb.age)?rb.soc:null;
 h+='<div class="big">'+(soc==null?'--':n(soc,0)+'<small> %</small>')+'</div>'+(soc==null?'':bar(soc));
 if(sh){let w=sh.bat_v*sh.bat_a;
  h+='<div class="m">'+esc(sh.name)+': '+n(sh.bat_v,2)+' V &middot; '+(sh.bat_a>0?'+':'')+n(sh.bat_a,1)+' A &middot; '+n(w,0)+' W'
  +(sh.ttg!=null&&sh.bat_a<-0.05?' &middot; '+dur(sh.ttg)+' left':'')+old(sh.age)+'</div>'+(sh.alarm?'<div class="bad">'+esc(sh.alarm)+'</div>':'')}
 if(rb.on){if(rb.age!=null){let c=rb.cells||[],t=rb.temps||[];
  h+='<div class="m">Renogy BMS: '+n(rb.soc,0)+' % &middot; '+n(rb.v,1)+' V &middot; '+n(rb.a,1)+' A &middot; '+n(rb.rem_ah,0)+' of '+n(rb.cap_ah,0)+' Ah'
  +(c.length?' &middot; cells '+n(Math.min(...c),1)+'-'+n(Math.max(...c),1)+' V':'')+(t.length?' &middot; '+tmp(Math.max(...t)):'')+old(rb.age)+'</div>'}
  if(rb.err)h+='<div class="m warn">Renogy: '+esc(rb.err)+'</div>'}
 $('bat').innerHTML=h}
function solar(r){
 let ch=r.victron.filter(d=>d.saved&&d.type==1),pv=0,kwh=0,amps=0,rows='';
 ch.forEach(d=>{if(d.age==null){rows+='<div class="r"><span>'+esc(d.name)+'</span><span class="warn">'+(d.key_wrong?'wrong key':d.key?'waiting':'no key yet')+'</span></div>';return}
  if(fresh(d.age)){pv+=d.pv_w||0;kwh+=d.yield_kwh||0;amps+=d.bat_a||0}
  rows+='<div class="r"><span>'+esc(d.name)+'<br><span class="m">'+d.state+(d.err?' <span class="bad">'+esc(d.err)+'</span>':'')+old(d.age)+'</span></span><span><b>'+n(d.pv_w,0)+' W</b><br><span class="m">'
  +n(d.bat_a,1)+' A &middot; '+n(d.yield_kwh,2)+' kWh today</span></span></div>'});
 let other=r.victron.filter(d=>!d.saved).length;
 $('sol').innerHTML=(ch.length?'<div class="big">'+pv+'<small> W &middot; '+n(amps,1)+' A in &middot; '+n(kwh,2)+' kWh today</small></div>':'<div class="m">No chargers set up yet.</div>')
  +rows+(other?'<div class="m">'+other+' more Victron device'+(other>1?'s':'')+' heard nearby: <a href="/setup">setup</a></div>':'')}
function water(r){let w=r.water;
 if(!w.on){$('wat').innerHTML='<div class="m">Flow meter not set up: <a href="/setup">setup</a></div>';return}
 let left=w.tank>0?Math.max(0,w.tank-w.used):null,p=left==null?null:100*left/w.tank;
 $('wat').innerHTML=(left==null?'<div class="big">'+vol(w.used)+'<small> used</small></div>'
  :'<div class="big">'+vol(left)+'<small> left &middot; '+n(p,0)+' %</small></div>'+bar(p))
  +'<div class="m">'+vol(w.used)+' used since the tank was filled'+(w.lpm>0.05?' &middot; running '+(U=='us'?n(w.lpm/3.785,1)+' gal':n(w.lpm,1)+' L')+'/min':'')+'</div>'
  +'<div class="b"><button onclick="filled()">Tank filled</button></div>'}
function fan(r){let m=r.maxxfan;
 if(!m.ok){$('fan').innerHTML='<div class="m warn">'+esc(m.err)+'</div>';$('fanb').hidden=true;return}
 let s=m.s,sp=s.fan_out||s.fan;
 let t='lid '+s.state+(s.pos<0?' (position unknown)':' &middot; ~'+Math.round(s.pos)+'% open');
 let f=s.fan_mode==0?'fan: knob only':'fan '+(sp?sp+'% '+(s.dir=='fwd'?'exhaust':'intake'):'off')
  +(s.fan_wait?' ('+(s.fan_wait=='lid'?'waiting for the lid':'reversing')+')':'')+(s.fan_mode==2?' &middot; set by '+s.fan_src:'');
 $('fan').innerHTML='<div class="big" style="font-size:24px">'+t+'</div><div class="m">'+f+old(m.age>10?m.age:null)+'</div>';
 $('fanb').hidden=s.fan_mode==0;fanSet={speed:s.fan,dir:s.dir};
 $('sp').innerHTML=[0,30,60,100].map(v=>'<button class="'+(s.fan==v?'on':'')+'" onclick="mf(\'speed='+v+'&dir='+s.dir+'\')">'+(v?v+'%':'off')+'</button>').join('');
 $('dr').innerHTML=['fwd','rev'].map(d=>'<button class="'+(s.dir==d?'on':'')+'" onclick="mf(\'speed='+(s.fan||50)+'&dir='+d+'\')">'+(d=='fwd'?'exhaust':'intake')+'</button>').join('')}
async function mf(q){let r=await fetch('/api/maxxfan?'+q);if(!r.ok)$('fan').innerHTML='<div class="m bad">'+esc(await r.text())+'</div>';u()}
async function filled(){if(confirm('Tank filled: start counting from full?')){await fetch('/api/water',{method:'POST',body:new URLSearchParams({filled:1})});u()}}
async function wifi(p){let r=await fetch('/api/wifi',{method:'POST',body:new URLSearchParams(p)});wm.textContent=r.ok?'':await r.text();u()}
function join(){if(ss.value)wifi({ssid:ss.value,pass:pw.value})}
function forget(){if(confirm('Forget the router? The hub will only be on its own VanHub hotspot.'))wifi({forget:1})}
async function scan(){wm.textContent='scanning ...';for(let i=0;i<15;i++){let r=await (await fetch('/api/scan')).json();
 if(r.nets){nets.innerHTML='<option value="">pick a network ('+r.nets.length+' found)</option>';
  r.nets.forEach(x=>{let o=document.createElement('option');o.value=o.textContent=x;nets.appendChild(o)});
  nets.hidden=false;wm.textContent='';return}
 await new Promise(z=>setTimeout(z,1000))}wm.textContent='scan failed, type the name'}
let busy=false;
async function u(){if(busy)return;busy=true;try{let r=await (await fetch('/api/status')).json();U=r.units;
 for(let f of [battery,solar,water,fan])try{f(r)}catch(e){console.log(e)}
 let w=!r.wifi_ssid?'no router saved':r.wifi_up?'on '+r.wifi_ssid+' ('+r.wifi_ip+', '+r.wifi_rssi+' dBm)':'trying '+r.wifi_ssid+' ...';
 w+=r.ap?' | hotspot on':' | hotspot off';
 if(r.wifi_up&&r.ap&&r.ap_clients)w+=' -- switch your phone to '+r.wifi_ssid+' and open http://van.local';
 net.textContent=w;if(first){ss.value=r.wifi_ssid;first=false}
 ft.innerHTML='hub '+r.fw+' &middot; <a href="/setup">setup</a> &middot; <a href="http://maxxfan.local">MaxxFan page</a>'}
 catch(e){ft.textContent='hub not answering'}busy=false}
u();setInterval(u,2000);
</script></body></html>)HTML";

const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Van hub setup</title>
<style>
:root{--bg:#fff;--fg:#15181c;--mut:#5f6670;--line:#c9ced5;--btn:#dff3e3;--ok:#1a7f37;--warn:#a85400;--bad:#c62828}
@media(prefers-color-scheme:dark){:root{--bg:#101215;--fg:#e9eaec;--mut:#9aa1aa;--line:#3a4048;--btn:#203a28;--ok:#5cc178;--warn:#f0a050;--bad:#ef6b6b}}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--fg);max-width:520px;margin:16px auto;padding:0 12px}
h1{font-size:22px}h2{font-size:18px;margin-top:26px;border-bottom:1px solid var(--line)}
button{font-size:19px;padding:12px;margin:8px 0;width:100%;border-radius:10px;border:1px solid var(--line);background:var(--btn);color:var(--fg)}
button.sec{background:transparent}.two{display:flex;gap:8px}
input,select{font-size:16px;padding:6px;margin:4px 0;border-radius:6px;border:1px solid var(--line);background:transparent;color:var(--fg)}
input{width:90px}.w{width:100%;box-sizing:border-box}label{display:block;margin:10px 0}
small,.m{color:var(--mut)}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.dev{border:1px solid var(--line);border-radius:10px;padding:10px;margin:10px 0}a{color:inherit}
</style></head><body>
<h1>Van hub setup</h1><p>Running <b>%FW%</b> &middot; <a href="/">back to the dashboard</a></p>
%MSG%
<h2>Victron (Instant Readout)</h2>
<small>Each device sends its readings encrypted, with its own key. In VictronConnect open the device, tap the gear, then
<b>&#8942; &gt; Product info</b>: <b>Instant readout via Bluetooth</b> on, then <b>Show</b> under Instant readout details. Copy
the encryption key (use the copy button: 32 characters) and paste it on the same device below; give it a name too.
VictronConnect connected to a device stops its readings until it disconnects.</small>
%VICTRON%
<form method="POST" action="/api/hub">
<h2>Renogy battery</h2>
%RENOGY%
<small>%RSTATUS%</small>
<h2>Water</h2>
<label>Flow meter pulses per litre <input name="ppl" inputmode="decimal" value="%PPL%"></label>
<small>450 = the old Arduino's setting (YF-S201 style 1/2" sensor). 0 = no flow meter. If the count drifts, fill a known
amount (e.g. a 1 gal jug), compare, and scale this number.</small>
<label>Tank size <input name="tank" inputmode="decimal" value="%TANK%"> %TUNIT%</label>
<label>Units <select name="units"><option value="us" %US%>US: gal, &deg;F</option><option value="metric" %METRIC%>metric: L, &deg;C</option></select></label>
<h2>MaxxFan</h2>
<label>Address <input class="w" name="mf" value="%MF%" autocapitalize="off" autocorrect="off"></label>
<small>maxxfan (found by name on the van router) or its IP address.</small>
<button>Save</button></form>
<h2>Firmware update</h2>
<form method="POST" action="/update" enctype="multipart/form-data"><input class="w" type="file" name="fw" required>
<button>Upload and restart</button></form>
<small>Pick the hub <b>.bin</b> (hub.ino.bin or hub-vN.bin, not a .merged.bin); about 20 s. The new firmware is kept once
the dashboard loads; if it never does, the hub goes back to the old firmware by itself after 10 min.</small>
<h2>Passwords</h2>
<form method="POST" action="/api/setup">
VanHub hotspot <input class="w" type="password" name="ap_pass" placeholder="leave empty to keep" autocomplete="new-password">
setup + updates (user van) <input class="w" type="password" name="ota_pass" placeholder="leave empty to keep" autocomplete="new-password">
<small>8 to 63 characters. Kept on the hub, through firmware updates.</small>
<button>Save and restart</button></form></body></html>)HTML";
