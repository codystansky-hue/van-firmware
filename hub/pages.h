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
[hidden]{display:none!important}
input[type=range]{accent-color:var(--bar);padding:0;border:0;background:none;height:34px;margin:8px 0 0}
.tk{display:flex;justify-content:space-between;font-size:12px;color:var(--mut);padding:0 5px;margin-top:-4px;font-variant-numeric:tabular-nums}
.flt{display:flex;gap:8px;align-items:center;justify-content:space-between;color:var(--bad);font-size:14px;margin:6px 0}
.flt button{flex:none;font-size:14px;padding:8px 10px;border-radius:8px;border:1px solid var(--bad);background:var(--btn);color:var(--fg)}
.gt{display:flex;justify-content:space-between;align-items:center;margin-top:12px;padding-top:8px;border-top:1px solid var(--line)}
.sg{display:flex;border:1px solid var(--line);border-radius:8px;overflow:hidden}
.sg button{font-size:13px;padding:5px 10px;border:0;background:var(--btn);color:var(--fg);white-space:nowrap}
.sg button+button{border-left:1px solid var(--line)}.sg button.on{font-weight:600;box-shadow:inset 0 -2px var(--bar)}
.gh{display:flex;justify-content:space-between;align-items:baseline;gap:8px;font-size:13px;color:var(--mut);margin-top:6px}
.gh b{color:var(--fg);font-size:15px;font-weight:600;font-variant-numeric:tabular-nums;text-align:right}
canvas{width:100%;height:56px;display:block;touch-action:pan-y}
.gx{display:flex;justify-content:space-between;gap:8px;font-size:11px;color:var(--mut);padding-left:38px;margin-top:2px}
#mgt{color:var(--fg);font-weight:600}
table{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}
th,td{text-align:right;padding:4px 6px;border-top:1px solid var(--line)}th:first-child,td:first-child{text-align:left}th{color:var(--mut);font-weight:500}
input,select{font-size:16px;width:100%;box-sizing:border-box;margin:4px 0;padding:7px;border-radius:8px;border:1px solid var(--line);background:var(--btn);color:var(--fg)}
a{color:inherit}
</style></head><body>
<div class="c"><h2>Battery</h2><div id="bat">...</div></div>
<div class="c"><h2>Solar</h2><div id="sol">...</div></div>
<div class="c"><h2>Water</h2><div id="wat">...</div></div>
<div class="c"><h2>MaxxFan</h2><div id="fan">...</div>
<div id="mflt" class="flt" hidden><span id="mfltx"></span><button onclick="mf('clear=1')">Clear the fault</button></div>
<div class="b"><button onclick="mf('lid=open')">Open</button><button onclick="mf('lid=close')">Close</button><button onclick="mf('lid=stop')">Stop</button></div>
<div id="fanb" hidden>
<input type="range" id="mspd" min="0" max="10" step="1" value="0" aria-label="Fan speed, 0 is off">
<div class="tk"><span>off</span><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span><span>6</span><span>7</span><span>8</span><span>9</span><span>10</span></div>
<div class="b" id="dr"></div></div>
<div id="mg" hidden>
<div class="gt"><span class="m">Motor</span><span class="sg"><button id="mw2" onclick="mwin(120)">2 min</button><button id="mw10" class="on" onclick="mwin(600)">10 min</button></span></div>
<div class="gh"><span>Speed</span><b id="mv0">-</b></div><canvas id="mg0" aria-label="MaxxFan motor speed over time"></canvas>
<div class="gh"><span>Current</span><b id="mv1">-</b></div><canvas id="mg1" aria-label="MaxxFan supply current over time"></canvas>
<div class="gh"><span>Supply</span><b id="mv2">-</b></div><canvas id="mg2" aria-label="MaxxFan supply voltage over time"></canvas>
<div class="gx"><span id="mgx0">10 min ago</span><span id="mgt"></span><span>now</span></div>
<details id="mtab" ontoggle="mtable()"><summary class="m">Readings</summary><table><thead><tr><th>when</th><th>rpm</th><th>A</th><th>V</th></tr></thead><tbody id="mtb"></tbody></table></details>
</div></div>
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
const ZN={off:'at its click',vent:'at vent',fan:'up'};
const WHY=[[/NO_MTR/,'a motor wire is loose'],[/IPD_/,'it could not find the rotor at start-up'],[/MTR_LCK|ABN_SPEED|ABN_BEMF/,'it lost track of the rotor'],
 [/LOCK_ILIMIT/,'too much current'],[/UNDER_VOLTAGE/,'the 12 V dropped under 8 V'],[/OVER_VOLTAGE|OVP/,'the supply went over 18 V'],[/OTW|OTS/,'the motor chip is hot']];
function why(t){let w=WHY.filter(x=>x[0].test(t)).map(x=>x[1]);return w.length?w.join(', ')+'.':'a fault.'}
let MS=null,mhold=0,MH=[],MWIN=600,mhv=null,mhistTried=false;
function mstep(s){if(!s.fan)return 0;
 if(s.steps&&s.foc){let b=1,bd=1e9;s.steps.forEach((v,i)=>{let d=Math.abs(v-s.foc.want_rpm);if(d<bd){bd=d;b=i+1}});return b}
 return Math.min(10,Math.max(1,Math.round(s.fan/10)))}
function fan(r){let m=r.maxxfan;
 if(!m.ok){$('fan').innerHTML='<div class="m warn">'+esc(m.err)+'</div>';$('fanb').hidden=true;$('mg').hidden=true;$('mflt').hidden=true;return}
 let s=m.s,f=s.foc,st=mstep(s),sp=s.fan;MS=s;
 let t='lid '+s.state+(s.pos<0?' (position unknown)':' &middot; ~'+Math.round(s.pos)+'% open');
 let fl=s.fan_mode==0?'fan: knob only':'fan '+(sp?(s.steps&&f?'speed '+st+' &middot; '+f.want_rpm+' rpm ':sp+'% ')+(s.dir=='fwd'?'exhaust':'intake'):'off')
  +(s.fan_wait?' ('+(s.fan_wait=='lid'?'waiting for the lid':'reversing')+')':'')
  +(s.fan_mode>=2?' &middot; set by '+s.fan_src+(s.knob_zone?', knob '+ZN[s.knob_zone]:''):'');
 let ml=!f?'':f.up?'<div class="m">motor: '+(f.state=='IDLE'?'stopped':Math.round(f.rpm)+' rpm')+' &middot; '+n(f.bus_a,2)+' A &middot; '
  +n(f.bus_a*f.vm,0)+' W &middot; '+n(f.vm,1)+' V</div>':'<div class="m warn">motor chip not answering (is the 12 V on?)</div>';
 $('fan').innerHTML='<div class="big" style="font-size:24px">'+t+'</div><div class="m">'+fl+old(m.age>10?m.age:null)+'</div>'+ml;
 let bad=!!(f&&f.up&&f.fault);$('mflt').hidden=!bad;
 if(bad)$('mfltx').textContent='Motor stopped: '+why(f.fault)+' ('+f.fault.replace(/\|/g,', ')+')';
 $('fanb').hidden=s.fan_mode==0;
 if(Date.now()>mhold)$('mspd').value=st;
 $('dr').innerHTML=['fwd','rev'].map(d=>'<button class="'+(s.dir==d?'on':'')+'" onclick="mf(\'speed='+(s.fan||0)+'&dir='+d+'\')">'+(d=='fwd'?'exhaust':'intake')+'</button>').join('');
 $('mg').hidden=!f;
 if(f){if(!mhistTried){mhistTried=true;mhist()}
  let now=Date.now(),tt=now-m.age*1000,last=MH.length?MH[MH.length-1].t:0;
  if(tt-last>1500)MH.push({t:tt,rpm:f.up?Math.abs(f.rpm):null,set:f.up?f.sent_rpm:null,vm:f.up?f.vm:null,bus:f.up?f.bus_a:null});
  while(MH.length&&MH[0].t<now-600000)MH.shift();mdraw();mtable()}}
$('mspd').oninput=()=>{mhold=Date.now()+4000};
$('mspd').onchange=()=>{mhold=Date.now()+4000;let k=+$('mspd').value,d=MS&&MS.dir=='rev'?'rev':'fwd';
 mf(!k?'speed=0&dir='+d:MS&&MS.steps&&MS.foc?'rpm='+MS.steps[k-1]+'&dir='+d:'speed='+k*10+'&dir='+d)};
const MP=[{k:'rpm',u:'rpm',z:1,f:v=>Math.round(v)},{k:'bus',u:'A',z:1,f:v=>v.toFixed(2)},{k:'vm',u:'V',z:0,f:v=>v.toFixed(1)}],MGL=38;
function css(v){return getComputedStyle(document.documentElement).getPropertyValue(v).trim()}
function nice(x){let p=Math.pow(10,Math.floor(Math.log10(x))),q=x/p;return([1,1.2,1.5,2,2.5,3,4,5,6,8,10].find(s=>q<=s))*p}
function tick(v){return String(+v.toFixed(2))}
function ago(ms){let s=Math.max(0,Math.round(ms/1000));return s<60?s+' s ago':Math.floor(s/60)+':'+String(s%60).padStart(2,'0')+' ago'}
async function mhist(){try{let q=await fetch('/api/maxxfan_history');if(!q.ok)return;let r=await q.json(),now=Date.now(),k=r.rpm.length,a=[];
 for(let i=0;i<k;i++)a.push({t:now-(r.age+(k-1-i)*r.dt)*1000,rpm:r.rpm[i],set:r.set[i],vm:r.vm[i]==null?null:r.vm[i]/10,bus:r.bus[i]==null?null:r.bus[i]/1000});
 let newest=a.length?a[a.length-1].t:0;MH=a.concat(MH.filter(p=>p.t>newest+1000));mdraw()}catch(e){}}
function mwin(v){MWIN=v;$('mw2').className=v==120?'on':'';$('mw10').className=v==600?'on':'';$('mgx0').textContent=v==120?'2 min ago':'10 min ago';mdraw()}
function mdraw(){if($('mg').hidden)return;
 let now=Date.now(),t0=now-MWIN*1000,pts=MH.filter(p=>p.t>=t0),hp=null;
 if(mhv!=null){let b=1e18;pts.forEach(p=>{let d=Math.abs(p.t-mhv);if(d<b){b=d;hp=p}})}
 $('mgt').textContent=hp?ago(now-hp.t):'';
 let acc=css('--bar'),rule=css('--line'),mut=css('--mut'),card=css('--card');
 MP.forEach((P,i)=>{let c=$('mg'+i),w=c.clientWidth,h=c.clientHeight,dpr=window.devicePixelRatio||1;if(!w)return;
  if(c.width!=Math.round(w*dpr)||c.height!=Math.round(h*dpr)){c.width=Math.round(w*dpr);c.height=Math.round(h*dpr)}
  let x=c.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);x.clearRect(0,0,w,h);
  let vals=pts.map(p=>p[P.k]).filter(v=>v!=null),lo,hi;
  if(P.z){lo=0;hi=nice(Math.max(P.k=='rpm'?400:0.5,...vals)*1.05)}
  else{let mn=vals.length?Math.min(...vals):11.5,mx=vals.length?Math.max(...vals):13.5,mid=(mn+mx)/2,sp=Math.max(2,(mx-mn)*1.2);lo=Math.floor((mid-sp/2)*2)/2;hi=Math.ceil((mid+sp/2)*2)/2}
  let L=MGL,Rr=w-6,T=6,B=h-6,X=t=>L+(t-t0)/(MWIN*1000)*(Rr-L),Y=v=>B-(v-lo)/(hi-lo)*(B-T);
  x.strokeStyle=rule;x.lineWidth=1;x.beginPath();[T,B].forEach(y=>{let yy=Math.round(y)+.5;x.moveTo(L,yy);x.lineTo(Rr,yy)});x.stroke();
  x.fillStyle=mut;x.font='11px system-ui,sans-serif';x.textAlign='right';x.textBaseline='middle';x.fillText(tick(hi),L-6,T);x.fillText(tick(lo),L-6,B);
  x.strokeStyle=acc;x.lineWidth=2;x.lineJoin='round';x.lineCap='round';x.beginPath();let pen=false;
  pts.forEach(p=>{let v=p[P.k];if(v==null){pen=false;return}let px=X(p.t),py=Y(v);if(pen)x.lineTo(px,py);else x.moveTo(px,py);pen=true});x.stroke();
  let last=null;for(let j=pts.length-1;j>=0;j--)if(pts[j][P.k]!=null){last=pts[j];break}
  let d=hp||last;
  if(hp){let hx=Math.round(X(hp.t))+.5;x.strokeStyle=mut;x.lineWidth=1;x.beginPath();x.moveTo(hx,T);x.lineTo(hx,B);x.stroke()}
  if(d&&d[P.k]!=null){let px=X(d.t),py=Y(d[P.k]);x.fillStyle=card;x.beginPath();x.arc(px,py,6,0,7);x.fill();x.fillStyle=acc;x.beginPath();x.arc(px,py,4,0,7);x.fill()}
  let v=d?d[P.k]:null,t='-';
  if(v!=null){t=P.f(v)+' '+P.u;if(P.k=='rpm'&&d.set!=null)t+=' (asked '+Math.round(d.set)+')';if(P.k=='bus'&&d.vm!=null)t+=' · '+Math.round(v*d.vm)+' W'}
  $('mv'+i).textContent=t})}
function mhov(e){let r=e.currentTarget.getBoundingClientRect(),q=(e.clientX-r.left-MGL)/(r.width-6-MGL);mhv=q<0||q>1?null:Date.now()-MWIN*1000*(1-q);mdraw()}
['mg0','mg1','mg2'].forEach(id=>{let c=$(id);c.onpointermove=mhov;c.onpointerdown=mhov;c.onpointerleave=()=>{mhv=null;mdraw()};c.onpointercancel=c.onpointerleave});
function mtable(){if(!$('mtab').open)return;let tb=$('mtb'),now=Date.now(),last=null,k=0;tb.textContent='';
 for(let j=MH.length-1;j>=0&&k<20;j--){let p=MH[j];if(last!=null&&last-p.t<15000)continue;last=p.t;k++;
  let row=tb.insertRow();[ago(now-p.t),p.rpm==null?'-':Math.round(p.rpm),p.bus==null?'-':p.bus.toFixed(2),p.vm==null?'-':p.vm.toFixed(1)].forEach(c=>{row.insertCell().textContent=c})}}
window.addEventListener('resize',mdraw);
document.addEventListener('visibilitychange',()=>{if(document.visibilityState=='visible'&&MS&&MS.foc)mhist()});
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
