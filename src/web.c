/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * web.c — n2k-mux-web : interface de gestion web (remplace à terme la GUI GTK).
 *
 * Mini serveur HTTP zéro-dépendance (C11) servant une SPA + des points d'API :
 *   GET  /              → page unique (HTML/CSS/JS embarqués)
 *   GET  /api/sources   → relaie sources.json (produit par le daemon)
 *   GET  /api/stats     → relaie stats.json
 *   GET  /api/config    → contenu brut de l'INI
 *   POST /api/validate  → valide un INI (config_parse_string) → {ok,err,line}
 *   POST /api/config    → valide PUIS écrit l'INI, puis lance le reload
 *
 * Le daemon relit sa config sur SIGHUP : « Enregistrer » écrit le fichier et
 * exécute la commande de reload (--reload-cmd, ex. "pkill -HUP -x n2k-mux") —
 * pas de root ni de systemctl restart nécessaires.
 *
 * Mono-client séquentiel (outil d'admin, un utilisateur) : accept → lit la
 * requête → répond → ferme. Sockets avec timeout. Zéro allocation dynamique
 * (tampons fixes), cohérent avec le reste du projet. Réutilise le module config.
 *
 * Usage : n2k-mux-web [config.ini] [--sources P] [--stats P]
 *                     [--port N] [--bind ADDR] [--reload-cmd CMD]
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQ_MAX   65536   /* requête (en-têtes + corps INI) */
#define FILE_MAX  65536   /* fichier servi / INI lu */

static const char *g_cfg_path    = NULL;
static const char *g_sources_path = "/run/n2k-mux/sources.json";
static const char *g_stats_path   = "/run/n2k-mux/stats.json";
static const char *g_busmap_path  = "/run/n2k-mux/busmap.json";
static const char *g_reload_cmd  = NULL;

/* --- Page unique embarquée (single quotes en JS pour éviter d'échapper les "). --- */
static const char PAGE[] =
"<!DOCTYPE html><html lang='fr'><head><meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>n2k-mux</title>\n"
"<style>\n"
"*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,sans-serif;background:#0f1216;color:#d8dee5}\n"
"header{background:#161b22;padding:.6em 1em;border-bottom:1px solid #2a313a;display:flex;gap:1em;align-items:center}\n"
"header b{color:#58a6ff}nav{display:flex;gap:.3em}\n"
"nav button{background:#21262d;color:#d8dee5;border:1px solid #2a313a;padding:.4em .9em;border-radius:6px;cursor:pointer}\n"
"nav button.on{background:#1f6feb;border-color:#1f6feb;color:#fff}\n"
"main{padding:1em}#sources,#charge{max-width:820px;margin:0 auto}.tab{display:none}.tab.on{display:block}\n"
"table{border-collapse:collapse;width:100%;margin:.5em 0}\n"
"table.fix{width:330px;table-layout:fixed;margin:.2em 0}table.fix .n{width:6.5em;white-space:nowrap}\n"
".row{display:flex;gap:2em;flex-wrap:wrap;align-items:flex-start}\n"
".card{background:#161b22;border:1px solid #2a313a;border-radius:8px;padding:.2em .9em .5em;flex:0 0 auto}\n"
".card h3{margin:.5em 0 0;font-size:.95em;color:#58a6ff}\n"
"th,td{padding:.35em .7em;border-bottom:1px solid #222a33;text-align:left}\n"
"th{color:#8b949e;font-weight:600;font-size:.85em}th.n,td.n{text-align:right}td.n{font-variant-numeric:tabular-nums}\n"
"code{color:#7ee787}.pill{display:inline-block;background:#21262d;border-radius:10px;padding:.05em .5em;margin:.1em;font-size:.82em}\n"
".bar{background:#21262d;border-radius:5px;height:16px;overflow:hidden;min-width:120px;display:inline-block;vertical-align:middle}\n"
".bar>i{display:block;height:100%;background:#1f6feb}\n"
"textarea{width:100%;height:calc(100vh - 230px);background:#0b0e12;color:#d8dee5;border:1px solid #2a313a;border-radius:6px;padding:.7em;font:15px/1.5 ui-monospace,monospace;resize:none;scrollbar-width:thin;scrollbar-color:#30363d #0b0e12}\n"
"textarea::-webkit-scrollbar{width:12px;height:12px}textarea::-webkit-scrollbar-track{background:#0b0e12}textarea::-webkit-scrollbar-thumb{background:#30363d;border-radius:6px;border:2px solid #0b0e12}\n"
".btn{background:#238636;color:#fff;border:0;padding:.5em 1.1em;border-radius:6px;cursor:pointer;margin-right:.5em}\n"
".btn.sec{background:#21262d;border:1px solid #2a313a}\n"
"#msg,#arb_msg,#src_msg{margin:.6em 0;padding:.5em .8em;border-radius:6px;display:none}#msg.ok,#arb_msg.ok,#src_msg.ok{display:block;background:#12351f;color:#7ee787}\n"
"#msg.err,#arb_msg.err,#src_msg.err{display:block;background:#3a1212;color:#ff7b72;white-space:pre-wrap}\n"
".ni{background:#0b0e12;color:#d8dee5;border:1px solid #2a313a;border-radius:4px;padding:.15em .4em;width:7em}\n"
".ri{background:#0b0e12;color:#d8dee5;border:1px solid #2a313a;border-radius:4px;padding:.15em .3em;width:5em;text-align:right}\n"
"small{color:#8b949e}\n"
".bok{color:#3fb950}.blo{color:#d29922}.bnu{color:#8b949e}\n"   /* statut : émis / supplanté / neutre */
".dot{color:#3fb950}.dotoff{color:#444}\n"                     /* pastille vivant */
".ob{background:#21262d;color:#d8dee5;border:1px solid #2a313a;border-radius:4px;cursor:pointer;padding:0 .35em;margin:0 1px}\n"
"select{background:#0b0e12;color:#d8dee5;border:1px solid #2a313a;border-radius:4px;padding:.1em .3em}\n"
".tg{font-size:.85em;margin-left:.7em;white-space:nowrap;color:#d8dee5}\n"
".arb th,.arb td{white-space:nowrap;vertical-align:middle}\n"
".arb td.c,.arb th.c{text-align:center}\n"
".arb td:last-child,.arb th:last-child{white-space:normal;width:100%}\n"
".chip{display:inline-block;border:1px solid #2a313a;border-radius:6px;padding:.1em .45em;margin:.12em .2em;background:#0d1117;white-space:nowrap;font-size:.9em}\n"
"</style></head><body>\n"
"<header><b>n2k-mux</b>\n"
"<nav><button data-t='sources' class='on'>Sources</button>"
"<button data-t='arbitrage'>Arbitrage</button>"
"<button data-t='charge'>Charge</button><button data-t='config'>Configuration</button></nav>\n"
"<small id='clock' style='margin-left:auto'></small></header>\n"
"<main>\n"
"<section id='sources' class='tab on'>\n"
"<div id='src_msg'></div>\n"
"<div style='margin:.2em 0 .6em'><button class='btn' id='src_save'>Enregistrer les noms</button>"
"<button class='btn sec' id='src_reload'>Rafraîchir</button>"
"<small> &nbsp;nommer une source la rend utilisable dans l'onglet Arbitrage</small></div>\n"
"<div id='src_body'>…</div></section>\n"
"<section id='arbitrage' class='tab'>\n"
"<div id='arb_msg'></div>\n"
"<div style='margin:.2em 0 .6em'><button class='btn' id='arb_save'>Enregistrer l'arbitrage</button>"
"<button class='btn sec' id='arb_reload'>Rafraîchir</button>"
"<small> &nbsp;case = source retenue · ▲▼ = ordre de priorité · mode par groupe</small></div>\n"
"<div id='arb_body'>…</div></section>\n"
"<section id='charge' class='tab'><div id='chg_body'>…</div></section>\n"
"<section id='config' class='tab'>\n"
"<div id='msg'></div>\n"
"<div class=card>\n"
"<textarea id='ini' spellcheck='false'></textarea>\n"
"<div style='margin-top:.6em'><button class='btn' id='save'>Enregistrer</button>"
"<button class='btn sec' id='valid'>Valider</button>"
"<button class='btn sec' id='reload'>Recharger depuis le disque</button></div>\n"
"<small>« Enregistrer » valide, écrit le fichier puis recharge le daemon (SIGHUP).</small>\n"
"</div>\n"
"</section>\n"
"</main>\n"
"<script>\n"
"const $=s=>document.querySelector(s),H=(t,n)=>{n=n??0;return Number(n).toFixed(t)};\n"
"const ms=h=>(h>0.0001)?Math.round(1000/h)+' ms':'— ms';\n"
"function esc(s){return (''+s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}\n"
"document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{\n"
" document.querySelectorAll('nav button').forEach(x=>x.classList.remove('on'));b.classList.add('on');\n"
" document.querySelectorAll('.tab').forEach(x=>x.classList.remove('on'));$('#'+b.dataset.t).classList.add('on');\n"
" if(b.dataset.t==='arbitrage')loadArb();\n"
" if(b.dataset.t==='sources')renderSources();\n"
"});\n"
"async function jget(u){const r=await fetch(u);if(!r.ok)throw new Error(r.status);return r.json()}\n"
"function smsg(t,cls){const m=$('#src_msg');m.textContent=t;m.className=t?(cls||'ok'):'';}\n"
"async function renderSources(){try{const r=await Promise.all([jget('/api/sources'),jget('/api/rules')]);const a=r[0],ru=r[1];\n"
" a.sort((x,y)=>x.src-y.src);\n"
" const nm={};for(const s of (ru.sources||[]))nm[s.ident]=s.name;\n"
" let h='<table><tr><th class=n>src</th><th>Nom</th><th>Identité</th><th>Modèle</th><th>PGNs publiés</th></tr>';\n"
" for(const s of a){if(!s.ident)continue;h+='<tr><td class=n>'+s.src+'</td>'\n"
"  +'<td><input class=ni data-ident=\"'+esc(s.ident)+'\" value=\"'+esc(nm[s.ident]||'')+'\" placeholder=\"—\"></td>'\n"
"  +'<td><code>'+esc(s.ident)+'</code></td><td>'+esc(s.model||s.mfg||'')+'</td>'\n"
"  +'<td>'+(s.pgns||[]).slice().sort((p,q)=>p.pgn-q.pgn).map(p=>'<span class=pill>'+p.pgn+'</span>').join('')+'</td></tr>';}\n"
" h+='</table>';$('#src_body').innerHTML='<div class=card>'+(a.length?h:'<p><small>Aucune source vue (le daemon tourne ?).</small></p>')+'</div>';\n"
"}catch(e){$('#src_body').innerHTML='<p><small>sources.json indisponible.</small></p>';}}\n"
"async function saveSrc(){try{const ru=await jget('/api/rules');\n"
" const identByName={};for(const s of (ru.sources||[]))identByName[s.name]=s.ident;\n"
" const newName={};document.querySelectorAll('#src_body input[data-ident]').forEach(i=>{const v=i.value.trim();if(v)newName[i.dataset.ident]=v;});\n"
" const sources=Object.keys(newName).map(id=>({ident:id,name:newName[id]}));\n"
" const rules=(ru.rules||[]).map(r=>({pgn:r.pgn,disc:r.disc,mode:r.mode,sources:r.sources.map(n=>{const id=identByName[n];return id===undefined?n:newName[id];}).filter(Boolean)}));\n"
" const res=await fetch('/api/config',{method:'POST',body:genIni(ru.talker,sources,rules,ru.ignore,ru.rates,ru.no_n2k,ru.no_0183)});const d=await res.json();\n"
" d.ok?smsg('Noms enregistrés et rechargés.','ok'):smsg('Refusé — ligne '+(d.line||'?')+' : '+(d.err||''),'err');\n"
" if(d.ok)setTimeout(renderSources,2500);\n"
"}catch(e){smsg('Erreur : '+e,'err');}}\n"
"function bar(p){p=Math.max(0,Math.min(100,p));return '<span class=bar><i style=\\'width:'+p+'%\\'></i></span> '+H(1,p)+'%';}\n"
"async function renderCharge(){try{const d=await jget('/api/stats');\n"
" let h='<table>'+\n"
"  '<tr><th>Bus N2K ('+(d.measured?'mesuré':'estimé')+')</th><td>'+bar(d.bus_load_pct)+' &nbsp;<small>'+H(1,d.msg_per_s)+' msg/s, '+H(1,d.frames_per_s)+' trames/s</small></td></tr>'+\n"
"  '<tr><th>Sortie 0183</th><td>'+bar(d.out_load_pct)+' &nbsp;<small>'+H(1,d.out_sent_per_s)+' phrases/s, '+H(0,d.out_bytes_per_s)+' o/s</small></td></tr></table>';\n"
" h+='<div class=row><div class=card><h3>PGN reçus</h3><table class=fix><tr><th>PGN</th><th class=n>intervalle</th><th class=n>total</th></tr>';\n"
" for(const p of (d.pgns||[]).slice().sort((a,b)=>a.pgn-b.pgn))h+='<tr><td>'+p.pgn+'</td><td class=n>'+ms(p.hz)+'</td><td class=n>'+p.total+'</td></tr>';\n"
" h+='</table></div><div class=card><h3>Phrases 0183 émises</h3><table class=fix><tr><th>type</th><th class=n>intervalle</th><th class=n>total</th></tr>';\n"
" for(const t of (d.out_types||[]).slice().sort((a,b)=>(a.type||'').localeCompare(b.type||'')))h+='<tr><td>'+esc(t.type)+'</td><td class=n>'+ms(t.hz)+'</td><td class=n>'+t.total+'</td></tr>';\n"
" h+='</table></div></div>';$('#chg_body').innerHTML=h;\n"
"}catch(e){$('#chg_body').innerHTML='<p><small>stats.json indisponible.</small></p>';}}\n"
"async function loadIni(){const r=await fetch('/api/config');$('#ini').value=await r.text();msg('');}\n"
"let msgTimer;\n"
"function msg(t,cls){const m=$('#msg');m.textContent=t;m.className=t?(cls||'ok'):'';\n"
" clearTimeout(msgTimer);if(t)msgTimer=setTimeout(()=>msg(''),15000);}\n"
"async function post(u){const r=await fetch(u,{method:'POST',body:$('#ini').value});return r.json();}\n"
"$('#valid').onclick=async()=>{const d=await post('/api/validate');\n"
" d.ok?msg('Configuration valide.','ok'):msg('Erreur ligne '+d.line+' : '+d.err,'err');};\n"
"$('#save').onclick=async()=>{const d=await post('/api/config');\n"
" d.ok?msg('Enregistré et rechargé.'+(d.reload?'':' (reload non configuré)'),'ok'):msg('Refusé — ligne '+d.line+' : '+d.err,'err');};\n"
"$('#reload').onclick=loadIni;\n"
"const PGNNAME={129025:'Position',129026:'COG/SOG',129029:'Position GNSS',129539:'DOP',129540:'Satellites',126992:'Heure',127250:'Cap',127251:'Giration',127257:'Attitude',130306:'Vent',127245:'Barre',129291:'Courant',128259:'Vitesse surface',128267:'Profondeur',128275:'Loch',130316:'Température',130314:'Pression',129038:'AIS pos A',129039:'AIS pos B',129040:'AIS pos B',129041:'AIS AtoN',129794:'AIS statique A',129809:'AIS statique 24A',129810:'AIS statique 24B',60928:'ISO Address Claim',126996:'Product Info',126993:'Heartbeat',59904:'ISO Request'};\n"
"const ST={accept:['émis','bok'],reject_priority:['supplanté','blo'],not_in_rule:['hors-règle','bnu'],no_rule:['non réglé','bnu'],unconfigured:['non configuré','bnu'],unknown_src:['identité ?','bnu'],ignored:['ignoré','bnu']};\n"
"const PGN2SENT={129025:['GLL'],129026:['VTG'],129029:['GGA'],129539:['GSA'],129540:['GSV'],126992:['ZDA'],127250:['HDG','HDM','HDT'],127251:['ROT'],127257:['XDR'],130306:['MWV','MWD'],127245:['RSA'],129291:['VDR'],128259:['VHW'],128267:['DPT'],128275:['VLW'],130316:['MTW','MDA'],130314:['MDA']};\n"
"const AISPGN=[129038,129039,129040,129041,129793,129794,129795,129796,129797,129798,129801,129802,129809,129810];\n"
"function has0183(p){return PGN2SENT[p]!==undefined||AISPGN.indexOf(p)>=0}\n"
"let ARB=null;\n"
"function akey(p,d){return p+'|'+(d||'')}\n"
"function nameOf(id){const s=(ARB.sources||[]).find(x=>x.ident===id);return s?s.name:''}\n"
"function rateOf(t){const r=(ARB.rates||[]).find(x=>x.type===t);return r?r.ms:''}\n"
"function setRate(t,ms){const a=ARB.rates,i=a.findIndex(x=>x.type===t);\n"
" if(ms>0){if(i>=0)a[i].ms=ms;else a.push({type:t,ms:ms});}else if(i>=0)a.splice(i,1);}\n"
"function setInt(ui,val){const u=ARB.units[ui],v=parseInt(val,10)||0;u.o183ms=v||'';\n"
" (PGN2SENT[u.pgn]||[]).forEach(t=>setRate(t,v));}\n"
"function amsg(t,cls){const m=$('#arb_msg');m.textContent=t;m.className=t?(cls||'ok'):'';}\n"
"async function loadArb(){try{\n"
" const r=await Promise.all([jget('/api/busmap'),jget('/api/rules')]);const bm=r[0],ru=r[1];\n"
" const rmap={};for(const x of (ru.rules||[]))rmap[akey(x.pgn,x.disc)]=x;\n"
" ARB={talker:ru.talker||'II',sources:ru.sources||[],rates:ru.rates||[],ignore:ru.ignore||{src:[],pgn:[]},units:[]};\n"
" const seen={};\n"
" for(const u of (bm.units||[])){const k=akey(u.pgn,u.disc);seen[k]=1;const x=rmap[k];\n"
"  ARB.units.push({pgn:u.pgn,disc:u.disc,mode:x?x.mode:'priority',sel:x?x.sources.slice():[],obs:u.sources.slice()});}\n"
" for(const x of (ru.rules||[])){const k=akey(x.pgn,x.disc);if(!seen[k]){ARB.units.push({pgn:x.pgn,disc:x.disc,mode:x.mode,sel:x.sources.slice(),obs:[]});}}\n"
" ARB.units.sort((a,b)=>a.pgn-b.pgn||(a.disc||'').localeCompare(b.disc||''));\n"
" const nn=ru.no_n2k||[],no=ru.no_0183||[];\n"
" for(const u of ARB.units){u.n2k=nn.indexOf(u.pgn)<0;u.o183=no.indexOf(u.pgn)<0;u.mappable=has0183(u.pgn);\n"
"  u.hasRate=PGN2SENT[u.pgn]!==undefined;u.o183ms=u.hasRate?rateOf(PGN2SENT[u.pgn][0]):'';}\n"
" amsg('');drawArb();\n"
"}catch(e){$('#arb_body').innerHTML='<p><small>busmap/rules indisponible (daemon avec --busmap ?).</small></p>';}}\n"
"function drawArb(){let h='<table class=arb><tr><th>PGN</th><th>Mode</th><th class=c>N2K</th><th class=c>0183</th><th class=c>0183 ms</th><th>Sources <small>(gauche = prioritaire)</small></th></tr>';\n"
" for(let ui=0;ui<ARB.units.length;ui++){const u=ARB.units[ui];\n"
"  const cand=[];const add=n=>{if(n&&cand.indexOf(n)<0)cand.push(n)};\n"
"  u.sel.forEach(add);u.obs.forEach(o=>{const nm=o.name||nameOf(o.ident);if(nm)add(nm)});u.cand=cand;\n"
"  h+='<tr><td>'+u.pgn+(u.disc?' /'+esc(u.disc):'')+'<br><small>'+(PGNNAME[u.pgn]||'')+'</small></td>';\n"
"  h+='<td><select data-act=mode data-u='+ui+'>'+['priority','min','max','fusion'].map(m=>'<option value='+m+(u.mode===m?' selected':'')+'>'+m+'</option>').join('')+'</select></td>';\n"
"  h+='<td class=c><input type=checkbox data-act=n2k data-u='+ui+(u.n2k?' checked':'')+'></td>';\n"
"  h+='<td class=c>'+(u.mappable?('<input type=checkbox data-act=o183 data-u='+ui+(u.o183?' checked':'')+'>'):'<small>—</small>')+'</td>';\n"
"  h+='<td class=c>'+(u.hasRate?('<input class=ri type=number min=0 data-act=int data-u='+ui+' value='+(u.o183ms||'')+'>'):'<small>—</small>')+'</td><td>';\n"
"  cand.forEach((nm,ci)=>{const si=u.sel.indexOf(nm);const o=u.obs.find(x=>(x.name||nameOf(x.ident))===nm)||{};const t=ST[o.status]||['',''];\n"
"   h+='<span class=chip><input type=checkbox data-act=tog data-u='+ui+' data-ci='+ci+(si>=0?' checked':'')+'>'\n"
"     +'<span class='+(o.alive?'dot':'dotoff')+'>●</span>'+esc(nm)\n"
"     +(si>=0?(' <button class=ob data-act=mv data-u='+ui+' data-i='+si+' data-d=-1>◀</button><button class=ob data-act=mv data-u='+ui+' data-i='+si+' data-d=1>▶</button>'):'')\n"
"     +((t[0]&&o.status&&o.status!=='accept')?(' <span class='+(t[1]||'bnu')+'>'+t[0]+'</span>'):'')+'</span>';});\n"
"  u.obs.filter(o=>!(o.name||nameOf(o.ident))).forEach(o=>{h+='<span class=chip><small>src '+o.src+' '+esc(o.ident||'')+' — à nommer</small></span>';});\n"
"  if(!cand.length&&!u.obs.length)h+='<small>—</small>';\n"
"  h+='</td></tr>';}\n"
" h+='</table>';$('#arb_body').innerHTML='<div class=card>'+h+'</div>';}\n"
"$('#arb_body').addEventListener('change',e=>{const t=e.target,u=+t.dataset.u;\n"
" if(t.dataset.act==='tog'){const nm=ARB.units[u].cand[+t.dataset.ci],s=ARB.units[u].sel,i=s.indexOf(nm);if(i>=0)s.splice(i,1);else s.push(nm);drawArb();}\n"
" else if(t.dataset.act==='mode'){ARB.units[u].mode=t.value;}\n"
" else if(t.dataset.act==='n2k'){ARB.units[u].n2k=t.checked;}\n"
" else if(t.dataset.act==='o183'){ARB.units[u].o183=t.checked;}\n"
" else if(t.dataset.act==='int'){setInt(u,t.value);}});\n"
"$('#arb_body').addEventListener('click',e=>{const t=e.target;if(t.dataset.act!=='mv')return;const u=+t.dataset.u,i=+t.dataset.i,d=+t.dataset.d,s=ARB.units[u].sel,j=i+d;if(j<0||j>=s.length)return;const x=s[i];s[i]=s[j];s[j]=x;drawArb();});\n"
"function genIni(talker,sources,rules,ignore,rates,no_n2k,no_0183){let o='[output]\\ntalker = '+(talker||'II')+'\\n';\n"
" if(no_n2k&&no_n2k.length)o+='no_n2k = '+no_n2k.join(', ')+'\\n';\n"
" if(no_0183&&no_0183.length)o+='no_0183 = '+no_0183.join(', ')+'\\n';\n"
" o+='\\n[sources]\\n';\n"
" for(const s of (sources||[]))if(s.name)o+=s.name+' = '+s.ident+'\\n';\n"
" o+='\\n[priority]\\n';\n"
" for(const r of (rules||[])){if(!r.sources||!r.sources.length)continue;const p=(r.mode&&r.mode!=='priority')?r.mode+': ':'';o+=r.pgn+(r.disc?'/'+r.disc:'')+' = '+p+r.sources.join(', ')+'\\n';}\n"
" o+='\\n[ignore]\\n';if(ignore&&ignore.src&&ignore.src.length)o+='src = '+ignore.src.join(', ')+'\\n';if(ignore&&ignore.pgn&&ignore.pgn.length)o+='pgn = '+ignore.pgn.join(', ')+'\\n';\n"
" o+='\\n[rate]\\n';for(const x of (rates||[]))o+=x.type+' = '+x.ms+'\\n';return o;}\n"
"$('#arb_save').onclick=async()=>{const rules=ARB.units.filter(u=>u.sel.length).map(u=>({pgn:u.pgn,disc:u.disc,mode:u.mode,sources:u.sel}));\n"
" const noN=ARB.units.filter(u=>!u.n2k).map(u=>u.pgn),noO=ARB.units.filter(u=>u.mappable&&!u.o183).map(u=>u.pgn);\n"
" const r=await fetch('/api/config',{method:'POST',body:genIni(ARB.talker,ARB.sources,rules,ARB.ignore,ARB.rates,noN,noO)});const d=await r.json();\n"
" d.ok?amsg('Arbitrage enregistré et rechargé.','ok'):amsg('Refusé — ligne '+(d.line||'?')+' : '+(d.err||''),'err');if(d.ok)setTimeout(loadArb,2500);};\n"
"$('#arb_reload').onclick=loadArb;\n"
"$('#src_save').onclick=saveSrc;$('#src_reload').onclick=renderSources;\n"
"function tick(){$('#clock').textContent=new Date().toLocaleTimeString();\n"
" if($('#charge').classList.contains('on'))renderCharge();}\n"
"loadIni();renderSources();tick();setInterval(tick,3000);\n"
"</script></body></html>\n";

/* --- E/S fichier (sans alloc) --- */
static int read_file(const char *path, char *buf, size_t cap, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    ssize_t r;
    while (n < cap - 1 && (r = read(fd, buf + n, cap - 1 - n)) > 0) n += (size_t)r;
    close(fd);
    buf[n] = '\0';
    if (len) *len = n;
    return 0;
}

static int write_file(const char *path, const char *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n < len) {
        ssize_t w = write(fd, buf + n, len - n);
        if (w <= 0) { close(fd); return -1; }
        n += (size_t)w;
    }
    close(fd);
    return 0;
}

/* Échappe une chaîne pour l'insérer dans une chaîne JSON. */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20) { continue; }
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* --- HTTP --- */
static void send_resp(int fd, int code, const char *status, const char *ctype,
                      const char *body, size_t len)
{
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, status, ctype, len);
    if (h > 0) { ssize_t w = write(fd, hdr, (size_t)h); (void)w; }
    if (body && len) { ssize_t w = write(fd, body, len); (void)w; }
}

static void send_text(int fd, int code, const char *status, const char *ctype,
                      const char *body)
{
    send_resp(fd, code, status, ctype, body, strlen(body));
}

/* Relaie un fichier JSON ; {} si absent (le daemon ne l'a pas encore écrit). */
static void serve_json_file(int fd, const char *path)
{
    static char buf[FILE_MAX];
    size_t len = 0;
    if (read_file(path, buf, sizeof buf, &len) == 0 && len > 0)
        send_resp(fd, 200, "OK", "application/json", buf, len);
    else
        send_text(fd, 200, "OK", "application/json", "{}");
}

static const char *mode_str(cfg_mode_t m)
{
    switch (m) {
        case CFG_PICK_MIN:    return "min";
        case CFG_PICK_MAX:    return "max";
        case CFG_PICK_FUSION: return "fusion";
        default:              return "priority";
    }
}

/* GET /api/rules : la config courante (INI parsée) en JSON structuré, pour
 * l'éditeur d'arbitrage. Une seule source de vérité : le parser C. */
static void serve_rules(int fd)
{
    config_t c;
    if (!config_load(&c, g_cfg_path)) {
        send_text(fd, 200, "OK", "application/json", "{}");
        return;
    }
    static char buf[16384];
    char e[CFG_IDENT_LEN * 2];
    size_t n = 0;
    int w;
#define APP(...) do { w = snprintf(buf + n, sizeof buf - n, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= sizeof buf - n) { \
            send_text(fd, 500, "Error", "application/json", "{}"); return; } \
        n += (size_t)w; } while (0)

    json_escape(c.talker, e, sizeof e);
    APP("{\"talker\":\"%s\",\"sources\":[", e);
    for (int i = 0; i < c.n_sources; i++) {
        json_escape(c.sources[i].name, e, sizeof e);  APP("%s{\"name\":\"%s\",", i ? "," : "", e);
        json_escape(c.sources[i].ident, e, sizeof e); APP("\"ident\":\"%s\"}", e);
    }
    APP("],\"rules\":[");
    for (int i = 0; i < c.n_rules; i++) {
        const cfg_rule_t *r = &c.rules[i];
        json_escape(r->discriminant, e, sizeof e);
        APP("%s{\"pgn\":%d,\"disc\":\"%s\",\"mode\":\"%s\",\"sources\":[",
            i ? "," : "", r->pgn, e, mode_str(r->mode));
        for (int j = 0; j < r->n_sources; j++) {
            json_escape(r->sources[j], e, sizeof e); APP("%s\"%s\"", j ? "," : "", e);
        }
        APP("]}");
    }
    APP("],\"rates\":[");
    for (int i = 0; i < c.n_rates; i++) {
        json_escape(c.rates[i].type, e, sizeof e);
        APP("%s{\"type\":\"%s\",\"ms\":%d}", i ? "," : "", e, c.rates[i].min_interval_ms);
    }
    APP("],\"ignore\":{\"src\":[");
    for (int i = 0; i < c.n_ignore_src; i++) APP("%s%d", i ? "," : "", c.ignore_src[i]);
    APP("],\"pgn\":[");
    for (int i = 0; i < c.n_ignore_pgn; i++) APP("%s%d", i ? "," : "", c.ignore_pgn[i]);
    APP("]},\"no_n2k\":[");
    for (int i = 0; i < c.n_no_n2k; i++) APP("%s%d", i ? "," : "", c.no_n2k_pgn[i]);
    APP("],\"no_0183\":[");
    for (int i = 0; i < c.n_no_0183; i++) APP("%s%d", i ? "," : "", c.no_0183_pgn[i]);
    APP("]}");
#undef APP
    send_text(fd, 200, "OK", "application/json", buf);
}

/* POST /api/validate et /api/config : `body` = texte INI. */
static void handle_config_post(int fd, char *body, int write_it)
{
    config_t cfg;
    config_init(&cfg);
    bool ok = config_parse_string(&cfg, body);

    char esc[256];
    json_escape(ok ? "" : cfg.err, esc, sizeof esc);

    int reloaded = 0;
    if (ok && write_it) {
        if (!g_cfg_path || write_file(g_cfg_path, body, strlen(body)) != 0) {
            char out[256];
            snprintf(out, sizeof out,
                "{\"ok\":false,\"line\":0,\"err\":\"écriture impossible (%s)\"}",
                g_cfg_path ? strerror(errno) : "aucun fichier");
            send_text(fd, 200, "OK", "application/json", out);
            return;
        }
        if (g_reload_cmd && g_reload_cmd[0]) {
            int rc = system(g_reload_cmd);
            reloaded = (rc == 0);
        }
    }

    char out[512];
    snprintf(out, sizeof out, "{\"ok\":%s,\"line\":%d,\"err\":\"%s\",\"reload\":%s}",
             ok ? "true" : "false", ok ? 0 : cfg.err_line, esc,
             reloaded ? "true" : "false");
    send_text(fd, ok ? 200 : 400, ok ? "OK" : "Bad Request",
              "application/json", out);
}

static void handle_client(int fd)
{
    static char req[REQ_MAX];
    size_t n = 0;
    ssize_t r;
    /* Lit les en-têtes (jusqu'à \r\n\r\n). */
    while (n < sizeof req - 1) {
        r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (n == 0) return;
    req[n] = '\0';

    char method[8] = "", path[256] = "";
    sscanf(req, "%7s %255s", method, path);

    char *hdr_end = strstr(req, "\r\n\r\n");
    char *body = hdr_end ? hdr_end + 4 : NULL;
    /* Corps complet ? (Content-Length) */
    if (body) {
        const char *cl = strcasestr(req, "Content-Length:");
        if (cl) {
            size_t want = (size_t)strtoul(cl + 15, NULL, 10);
            size_t have = n - (size_t)(body - req);
            while (have < want && n < sizeof req - 1) {
                r = recv(fd, req + n, sizeof req - 1 - n, 0);
                if (r <= 0) break;
                n += (size_t)r; have += (size_t)r;
            }
            req[n] = '\0';
            body = strstr(req, "\r\n\r\n") + 4;
            body[want < have ? want : have] = '\0';
        }
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0)
            send_resp(fd, 200, "OK", "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
        else if (strcmp(path, "/api/sources") == 0)
            serve_json_file(fd, g_sources_path);
        else if (strcmp(path, "/api/stats") == 0)
            serve_json_file(fd, g_stats_path);
        else if (strcmp(path, "/api/busmap") == 0)
            serve_json_file(fd, g_busmap_path);
        else if (strcmp(path, "/api/rules") == 0)
            serve_rules(fd);
        else if (strcmp(path, "/api/config") == 0) {
            static char buf[FILE_MAX]; size_t len = 0;
            if (g_cfg_path && read_file(g_cfg_path, buf, sizeof buf, &len) == 0)
                send_resp(fd, 200, "OK", "text/plain; charset=utf-8", buf, len);
            else
                send_text(fd, 200, "OK", "text/plain; charset=utf-8", "");
        } else
            send_text(fd, 404, "Not Found", "text/plain", "404\n");
    } else if (strcmp(method, "POST") == 0 && body) {
        if (strcmp(path, "/api/validate") == 0)
            handle_config_post(fd, body, 0);
        else if (strcmp(path, "/api/config") == 0)
            handle_config_post(fd, body, 1);
        else
            send_text(fd, 404, "Not Found", "text/plain", "404\n");
    } else {
        send_text(fd, 400, "Bad Request", "text/plain", "400\n");
    }
}

int main(int argc, char **argv)
{
    int port = 8080;
    const char *bind_addr = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sources") == 0 && i + 1 < argc) g_sources_path = argv[++i];
        else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) g_stats_path = argv[++i];
        else if (strcmp(argv[i], "--busmap") == 0 && i + 1 < argc) g_busmap_path = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) bind_addr = argv[++i];
        else if (strcmp(argv[i], "--reload-cmd") == 0 && i + 1 < argc) g_reload_cmd = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "Usage : %s [config.ini] [--sources P] [--stats P] [--busmap P]\n"
                "          [--port N] [--bind ADDR] [--reload-cmd CMD]\n"
                "  config.ini    fichier INI édité par l'interface\n"
                "  --busmap P    carte (pgn/disc)->sources pour l'onglet Arbitrage\n"
                "  --port N      port d'écoute (défaut 8080)\n"
                "  --bind ADDR   adresse d'écoute (défaut 127.0.0.1 ; 0.0.0.0 = LAN)\n"
                "  --reload-cmd  commande lancée après sauvegarde (ex. \"pkill -HUP -x n2k-mux\")\n",
                argv[0]);
            return 0;
        } else if (argv[i][0] != '-') g_cfg_path = argv[i];
        else { fprintf(stderr, "option inconnue : %s\n", argv[i]); return 2; }
    }

    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) {
        fprintf(stderr, "adresse invalide : %s\n", bind_addr); return 2;
    }
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
    if (listen(ls, 8) != 0) { perror("listen"); return 1; }

    fprintf(stderr, "n2k-mux-web : http://%s:%d/  (config: %s)\n",
            bind_addr, port, g_cfg_path ? g_cfg_path : "(aucune)");

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        handle_client(fd);
        close(fd);
    }
    close(ls);
    return 0;
}
