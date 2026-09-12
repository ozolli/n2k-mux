/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * web.c — n2k-mux-web : interface de gestion web (Sources + Arbitrage).
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
#include "inimerge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQ_MAX   65536   /* requête (en-têtes + corps INI) */
#define FILE_MAX  65536   /* fichier servi / INI lu */

static const char *g_cfg_path    = NULL;
static const char *g_sources_path = "/run/n2k-mux/sources.json";
static const char *g_stats_path   = "/run/n2k-mux/stats.json";
static const char *g_busmap_path  = "/run/n2k-mux/busmap.json";
static const char *g_reload_cmd  = NULL;
/* Simulateur : fichier de pilotage relu à chaud par n2k-sim --control, et
 * commandes OPTIONNELLES de démarrage/arrêt de la chaîne simulée. Sans ces
 * commandes, la bascule ne fait que réduire le simulateur au silence (ce qui
 * suffit si la chaîne simulée tourne déjà). */
static const char *g_sim_path    = "/etc/n2k-mux/sim.ctl";
/* État DÉDUIT publié par n2k-sim --state : éphémère, donc dans /run. */
static const char *g_sim_state   = "/run/n2k-mux/sim.state";
static const char *g_sim_start   = NULL;
static const char *g_sim_stop    = NULL;
#define AUTH_RAW_MAX 256                   /* longueur max acceptée de "user:pass" */
static const char *g_auth        = NULL;   /* "user:pass" attendu (NULL = pas d'auth) */
static char        g_auth_b64[352];        /* base64 du credential (>= 4*ceil(256/3)+1) */

/* --- Page unique embarquée (single quotes en JS pour éviter d'échapper les "). --- */
static const char PAGE[] =
"<!DOCTYPE html><html lang='fr'><head><meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>n2k-mux</title>\n"
"<style>\n"
":root{--bg:#0f1216;--fg:#d8dee5;--panel:#161b22;--border:#2a313a;--row:#222a33;--muted:#8b949e;--accent:#58a6ff;--btn:#21262d;--active:#1f6feb;--inbg:#0b0e12;--save:#238636;--code:#7ee787;--dot:#3fb950;--warn:#d29922;--okbg:#12351f;--okfg:#7ee787;--errbg:#3a1212;--errfg:#ff7b72;--chip:#0d1117}\n"
"body.light{--bg:#ffffff;--fg:#1b2430;--panel:#f3f5f8;--border:#cfd8e3;--row:#e3e9f0;--muted:#5b6675;--accent:#0b62d6;--btn:#e6ebf2;--active:#1f6feb;--inbg:#ffffff;--save:#1a7f37;--code:#0a7a33;--dot:#1a7f37;--warn:#9a6a00;--okbg:#e6f4ea;--okfg:#136a2e;--errbg:#fdecec;--errfg:#b42318;--chip:#eef2f7}\n"
"*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg)}\n"
"header{background:var(--panel);padding:.6em 1em;border-bottom:1px solid var(--border);display:flex;gap:1em;align-items:center}\n"
"header b{color:var(--accent)}nav{display:flex;gap:.3em}\n"
"nav button{background:var(--btn);color:var(--fg);border:1px solid var(--border);padding:.4em .9em;border-radius:6px;cursor:pointer}\n"
"nav button.on{background:var(--active);border-color:var(--active);color:#fff}\n"
".hbtn{background:var(--btn);color:var(--fg);border:1px solid var(--border);padding:.25em .55em;border-radius:6px;cursor:pointer;font-size:.85em}\n"
"main{padding:1em}#sources{max-width:820px;margin:0 auto}.tab{display:none}.tab.on{display:block}\n"
"table{border-collapse:collapse;width:100%;margin:.5em 0}\n"
".card{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:.2em .9em .5em;flex:0 0 auto}\n"
".card h3{margin:.5em 0 0;font-size:.95em;color:var(--accent)}\n"
"th,td{padding:.35em .7em;border-bottom:1px solid var(--row);text-align:left}\n"
"th{color:var(--muted);font-weight:600;font-size:.85em}th.n,td.n{text-align:right}td.n{font-variant-numeric:tabular-nums}\n"
"code{color:var(--code)}.pill{display:inline-block;background:var(--btn);border-radius:10px;padding:.05em .5em;margin:.1em;font-size:.82em}\n"
".bar{background:var(--btn);border-radius:5px;height:16px;overflow:hidden;min-width:120px;display:inline-block;vertical-align:middle}\n"
".bar>i{display:block;height:100%;background:var(--active)}\n"
".btn{background:var(--save);color:#fff;border:0;padding:.5em 1.1em;border-radius:6px;cursor:pointer;margin-right:.5em}\n"
".btn.sec{background:var(--btn);color:var(--fg);border:1px solid var(--border)}\n"
"#arb_msg,#src_msg{margin:.6em 0;padding:.5em .8em;border-radius:6px;display:none}#arb_msg.ok,#src_msg.ok{display:block;background:var(--okbg);color:var(--okfg)}\n"
"#arb_msg.err,#src_msg.err{display:block;background:var(--errbg);color:var(--errfg);white-space:pre-wrap}\n"
".ni{background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:.15em .4em;width:7em}\n"
".ri{background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:.15em .3em;width:5em;text-align:right}\n"
"small{color:var(--muted)}\n"
".bok{color:var(--dot)}.blo{color:var(--warn)}.bnu{color:var(--muted)}\n"   /* statut : émis / supplanté / neutre */
".dot{color:var(--dot)}.dotoff{color:var(--border)}\n"                      /* pastille vivant */
".ob{background:var(--btn);color:var(--fg);border:1px solid var(--border);border-radius:4px;cursor:pointer;padding:0 .35em;margin:0 1px}\n"
"select{background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:.1em .3em}\n"
".tg{font-size:.85em;margin-left:.7em;white-space:nowrap;color:var(--fg)}\n"
".arb th,.arb td{white-space:nowrap;vertical-align:middle}\n"
"td.c,th.c{text-align:center}\n"
".arb td.srccol,.arb th.srccol{white-space:normal;width:100%}\n"
".chip{display:inline-block;border:1px solid var(--border);border-radius:6px;padding:.1em .45em;margin:.12em .2em;background:var(--chip);white-space:nowrap;font-size:.9em}\n"
".sl{display:inline-block;font-size:.82em;white-space:nowrap;margin:0 .35em .1em 0}\n"
".arb tr.ign td{opacity:.45}.arb tr.ign td:first-child{opacity:1}\n"
".dead{color:var(--errfg);font-weight:600}\n"
"</style></head><body>\n"
"<header><b>n2k-mux</b>\n"
"<nav><button data-t='sources' class='on' data-i18n='tab_sources'>Sources</button>"
"<button data-t='arbitrage' data-i18n='tab_arb'>Arbitrage</button>"
"<button data-t='sim' data-i18n='tab_sim'>Simulateur</button></nav>\n"
"<span style='margin-left:auto;display:flex;gap:.5em;align-items:center'>"
"<button id='lang' class='hbtn' title='Langue / Language'></button>"
"<button id='theme' class='hbtn' title='Thème / Theme'></button>"
"<small id='clock'></small></span></header>\n"
"<main>\n"
"<section id='sources' class='tab on'>\n"
"<div id='src_msg'></div>\n"
"<div style='margin:.2em 0 .6em'><button class='btn' id='src_save' data-i18n='save_names'>Enregistrer les noms</button>"
"<button class='btn sec' id='src_reload' data-i18n='refresh'>Rafraîchir</button>"
"<small> &nbsp;<span data-i18n='src_help'></span></small></div>\n"
"<div id='src_body'>…</div></section>\n"
"<section id='arbitrage' class='tab'>\n"
"<div id='arb_msg'></div>\n"
"<div style='margin:.2em 0 .6em'><button class='btn' id='arb_save' data-i18n='save_arb'>Enregistrer l'arbitrage</button>"
"<button class='btn sec' id='arb_reload' data-i18n='refresh'>Rafraîchir</button>"
"<label class=sl style='margin-left:.6em'><input type=checkbox id='arb_unseen'> <span data-i18n='show_unseen'></span></label>"
"<small> &nbsp;<span data-i18n='arb_help'></span></small></div>\n"
"<div id='arb_load' class='card' style='margin-bottom:.6em'></div>\n"
"<div id='arb_body'>…</div></section>\n"
"<section id='sim' class='tab'>\n"
"<div id='sim_msg'></div>\n"
"<div style='margin:.2em 0 .6em'>"
"<label class=sl><input type=checkbox id='sim_on'> <b><span data-i18n='sim_on'></span></b></label>"
"<button class='btn sec' id='sim_reload' data-i18n='refresh'>Rafraîchir</button>"
"<small> &nbsp;<span data-i18n='sim_help'></span></small></div>\n"
"<div id='sim_body' style='max-width:760px'>…</div>\n"
"<div class='card' style='max-width:760px;margin-top:.6em'>"
"<h3 data-i18n='sim_derived_t'></h3><div id='sim_state'></div>"
"<small><span data-i18n='sim_derived'></span></small></div>\n"
"</section>\n"
"</main>\n"
"<script>\n"
"let lang=localStorage.getItem('lang')||((navigator.language||'fr').toLowerCase().startsWith('fr')?'fr':'en');\n"
"let theme=localStorage.getItem('theme')||((window.matchMedia&&matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark');\n"
"const L={fr:{\n"
"tab_sources:'Sources',tab_arb:'Arbitrage',save_names:'Enregistrer les noms',save_arb:'Enregistrer l’arbitrage',refresh:'Rafraîchir',\n"
"src_help:'nommer une source la rend utilisable dans l’onglet Arbitrage',arb_help:'case = source retenue · ◀▶ = ordre de priorité',\n"
"h_src:'src',h_name:'Nom',h_ignore:'Ignorer',h_ident:'Identité',h_model:'Modèle',h_pgns:'PGNs publiés',\n"
"a_mode:'Mode',a_talker:'Talker',a_sent:'Phrases 0183',a_ms:'ms',a_sources:'Sources',a_sub:'(gauche = prioritaire)',a_total:'Total reçu',a_hz:'Hz',\n"
"t_src:'Adresse source NMEA 2000 (0-251) ; peut changer aux réallocations d’adresse du bus',\n"
"t_name:'Nom logique éditable. Le saisir ici rend la source utilisable dans l’onglet Arbitrage',\n"
"t_ignore:'Ignorer toutes les trames de cette source ([ignore] src)',\n"
"t_ident:'Identité stable de l’appareil (Model Serial Code ou Unique Number) — survit aux changements d’adresse',\n"
"t_model:'Modèle / fabricant déclaré par l’appareil (PGN 126996 Product Information)',\n"
"t_pgns:'PGN réellement émis par cet appareil et leur nombre',\n"
"t_pgn:'Numéro de PGN NMEA 2000 (+ discriminant éventuel) et son rôle',\n"
"t_mode:'Mode de sélection : priority (1re source vivante), min (profondeur, sécurité), max (loch), fusion (AIS, dédup MMSI)',\n"
"t_n2k:'Réémettre ce PGN en NMEA 2000 (vcan0 + YDRAW). Décoché = ce PGN est coupé du flux N2K arbitré',\n"
"t_talker:'Talker 0183 pour ce PGN (2 lettres, ex. GP, II, HE). Vide = talker global de la sortie',\n"
"t_sent:'Phrases 0183 émises pour ce PGN. Tout coché = défaut ; un sous-ensemble = liste blanche [sentence] ; rien coché = pas de 0183 pour ce PGN',\n"
"t_ms:'Intervalle minimum entre deux phrases de ce PGN, en millisecondes (throttle). Vide ou 0 = pas de limite',\n"
"t_sources:'Sources vues sur le bus pour ce PGN. Cochée = retenue ; ordre de gauche à droite = priorité décroissante ; ◀▶ réordonne',\n"
"t_total:'Nombre total de messages de ce PGN reçus depuis le démarrage du daemon',\n"
"t_hz:'Fréquence d’arrivée mesurée du PGN (messages par seconde, toutes sources confondues)',\n"
"t_ign:'Ignorer complètement ce PGN : ni arbitrage, ni sortie 0183/N2K ([ignore] pgn)',\n"
"measured:'mesuré',estimated:'estimé',frames_s:'trames/s',sent_s:'phrases/s',\n"
"names_saved:'Noms enregistrés et rechargés.',arb_saved:'Arbitrage enregistré et rechargé.',rej_line:'Refusé — ligne ',err_pfx:'Erreur : ',\n"
"no_src:'Aucune source vue (le daemon tourne ?).',na_src:'sources.json indisponible.',na_bm:'busmap/rules indisponible (daemon avec --busmap ?).',\n"
"to_name:'à nommer',ignore_lbl:'ignorer',show_unseen:'PGN non vus',arb_none:'Aucun PGN vu pour l’instant (le bus émet ? sinon, cocher « PGN non vus »).',\n"
"stale:'FLUX MORT : aucun message depuis',stale_never:'FLUX MORT : aucun message reçu',\n"
"tab_sim:'Simulateur',sim_on:'Simulateur actif',\n"
"sim_help:'bateau simulé, sans matériel : ces six grandeurs pilotent tout le flux',\n"
"sim_hdg:'Cap vrai (HDG)',sim_stw:'Vitesse surface (STW)',\n"
"sim_cog:'Route fond (COG)',sim_sog:'Vitesse fond (SOG)',sim_set:'Direction du courant (set)',\n"
"sim_drift:'Vitesse du courant (drift)',sim_twd:'Direction du vent vrai (TWD)',sim_tws:'Vitesse du vent vrai (TWS)',\n"
"sim_twa:'Angle du vent vrai (TWA)',sim_awa:'Angle du vent apparent (AWA)',sim_aws:'Vitesse du vent apparent (AWS)',\n"
"sim_lat:'Latitude',sim_lon:'Longitude',\n"
"sim_auto:'auto',sim_saved:'Simulateur mis à jour.',sim_na:'Pilotage indisponible (chemin --sim-control accessible ?).',\n"
"sim_boat:'Bateau',sim_cur:'Courant',sim_wind:'Vent',sim_windby:'Vent défini par',\n"
"sim_by_twd:'direction vraie (TWD + TWS)',sim_by_twa:'angle vrai (TWA + TWS)',sim_by_awa:'apparent (AWA + AWS)',\n"
"sim_set_lbl:'réglé',sim_calc:'déduit',sim_nostate:'État déduit indisponible : le simulateur ne tourne pas, ou --state n’est pas publié.',\n"
"sim_derived_t:'Valeurs déduites',\n"
"sim_derived:'Le triangle des vitesses lie tout : route et vitesse fond = vecteur surface plus vecteur courant, et le vent vrai, son angle et l’apparent sont trois façons de dire la même chose. On impose donc UNE paire de vent et le reste est calculé. Giration = dérivée du cap, nulle si le cap est imposé.',\n"
"sim_nostart:'La bascule ne fait que rendre le simulateur muet : aucune commande de démarrage n’est configurée (--sim-start).',\n"
"sim_dir:'°',sim_kn:'nds'\n"
"},en:{\n"
"tab_sources:'Sources',tab_arb:'Arbitration',save_names:'Save names',save_arb:'Save arbitration',refresh:'Refresh',\n"
"src_help:'naming a source makes it usable in the Arbitration tab',arb_help:'checkbox = selected source · ◀▶ = priority order',\n"
"h_src:'src',h_name:'Name',h_ignore:'Ignore',h_ident:'Identity',h_model:'Model',h_pgns:'Published PGNs',\n"
"a_mode:'Mode',a_talker:'Talker',a_sent:'0183 sentences',a_ms:'ms',a_sources:'Sources',a_sub:'(left = highest priority)',a_total:'Total recv',a_hz:'Hz',\n"
"t_src:'NMEA 2000 source address (0-251); may change on bus address reallocation',\n"
"t_name:'Editable logical name. Setting it here makes the source usable in the Arbitration tab',\n"
"t_ignore:'Ignore all frames from this source ([ignore] src)',\n"
"t_ident:'Stable device identity (Model Serial Code or Unique Number) — survives address changes',\n"
"t_model:'Model / manufacturer declared by the device (PGN 126996 Product Information)',\n"
"t_pgns:'PGNs actually transmitted by this device and their count',\n"
"t_pgn:'NMEA 2000 PGN number (+ optional discriminant) and its role',\n"
"t_mode:'Selection mode: priority (first live source), min (depth, safety), max (log), fusion (AIS, MMSI dedup)',\n"
"t_n2k:'Re-emit this PGN in NMEA 2000 (vcan0 + YDRAW). Unchecked = this PGN is dropped from the arbitrated N2K stream',\n"
"t_talker:'0183 talker for this PGN (2 letters, e.g. GP, II, HE). Empty = global output talker',\n"
"t_sent:'0183 sentences emitted for this PGN. All checked = default; a subset = [sentence] whitelist; none checked = no 0183 for this PGN',\n"
"t_ms:'Minimum interval between two sentences of this PGN, in milliseconds (throttle). Empty or 0 = no limit',\n"
"t_sources:'Sources seen on the bus for this PGN. Checked = selected; left-to-right = decreasing priority; ◀▶ reorders',\n"
"t_total:'Total number of messages of this PGN received since daemon start',\n"
"t_hz:'Measured arrival rate of the PGN (messages per second, all sources combined)',\n"
"t_ign:'Completely ignore this PGN: no arbitration, no 0183/N2K output ([ignore] pgn)',\n"
"measured:'measured',estimated:'estimated',frames_s:'frames/s',sent_s:'sentences/s',\n"
"names_saved:'Names saved and reloaded.',arb_saved:'Arbitration saved and reloaded.',rej_line:'Rejected — line ',err_pfx:'Error: ',\n"
"no_src:'No source seen (is the daemon running?).',na_src:'sources.json unavailable.',na_bm:'busmap/rules unavailable (daemon with --busmap?).',\n"
"to_name:'to name',ignore_lbl:'ignore',show_unseen:'unseen PGNs',arb_none:'No PGN seen yet (is the bus active? otherwise tick “unseen PGNs”).',\n"
"stale:'STREAM DEAD: no message for',stale_never:'STREAM DEAD: no message received',\n"
"tab_sim:'Simulator',sim_on:'Simulator running',\n"
"sim_help:'simulated boat, no hardware: these six inputs drive the whole stream',\n"
"sim_hdg:'True heading (HDG)',sim_stw:'Speed through water (STW)',\n"
"sim_cog:'Course over ground (COG)',sim_sog:'Speed over ground (SOG)',sim_set:'Current direction (set)',\n"
"sim_drift:'Current speed (drift)',sim_twd:'True wind direction (TWD)',sim_tws:'True wind speed (TWS)',\n"
"sim_twa:'True wind angle (TWA)',sim_awa:'Apparent wind angle (AWA)',sim_aws:'Apparent wind speed (AWS)',\n"
"sim_lat:'Latitude',sim_lon:'Longitude',\n"
"sim_auto:'auto',sim_saved:'Simulator updated.',sim_na:'Control unavailable (is --sim-control writable?).',\n"
"sim_boat:'Boat',sim_cur:'Current',sim_wind:'Wind',sim_windby:'Wind defined by',\n"
"sim_by_twd:'true direction (TWD + TWS)',sim_by_twa:'true angle (TWA + TWS)',sim_by_awa:'apparent (AWA + AWS)',\n"
"sim_set_lbl:'set',sim_calc:'derived',sim_nostate:'Derived state unavailable: the simulator is not running, or --state is not published.',\n"
"sim_derived_t:'Derived values',\n"
"sim_derived:'The velocity triangle ties everything together: course and speed over ground = water vector plus current vector, and true wind, its angle and the apparent wind are three ways of saying the same thing. So ONE wind pair is imposed and the rest is computed. Rate of turn = heading derivative, zero when the heading is forced.',\n"
"sim_nostart:'The toggle only silences the simulator: no start command is configured (--sim-start).',\n"
"sim_dir:'°',sim_kn:'kn'\n"
"}};\n"
"const T=k=>{const o=L[lang];return (o&&o[k]!=null)?o[k]:k;};\n"
"function applyI18n(){document.querySelectorAll('[data-i18n]').forEach(e=>{e.textContent=T(e.dataset.i18n);});document.documentElement.lang=lang;}\n"
"const $=s=>document.querySelector(s),H=(t,n)=>{n=n??0;return Number(n).toFixed(t)};\n"
"const ms=h=>(h>0.0001)?Math.round(1000/h)+' ms':'— ms';\n"
"function esc(s){return (''+s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}\n"
"document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{\n"
" document.querySelectorAll('nav button').forEach(x=>x.classList.remove('on'));b.classList.add('on');\n"
" document.querySelectorAll('.tab').forEach(x=>x.classList.remove('on'));$('#'+b.dataset.t).classList.add('on');\n"
" if(b.dataset.t==='arbitrage')loadArb();\n"
" if(b.dataset.t==='sources')renderSources();\n"
" if(b.dataset.t==='sim')loadSim();\n"
"});\n"
"async function jget(u){const r=await fetch(u);if(!r.ok)throw new Error(r.status);return r.json()}\n"
"function smsg(t,cls){const m=$('#src_msg');m.textContent=t;m.className=t?(cls||'ok'):'';}\n"
"async function renderSources(){try{const r=await Promise.all([jget('/api/sources'),jget('/api/rules')]);const a=r[0],ru=r[1];\n"
" a.sort((x,y)=>x.src-y.src);\n"
" const nm={};for(const s of (ru.sources||[]))nm[s.ident]=s.name;\n"
" const ig=(ru.ignore&&ru.ignore.src)||[];\n"
" let h='<table><tr>'\n"
"  +'<th class=n title=\"'+esc(T('t_src'))+'\">'+T('h_src')+'</th>'\n"
"  +'<th title=\"'+esc(T('t_name'))+'\">'+T('h_name')+'</th>'\n"
"  +'<th class=c title=\"'+esc(T('t_ignore'))+'\">'+T('h_ignore')+'</th>'\n"
"  +'<th title=\"'+esc(T('t_ident'))+'\">'+T('h_ident')+'</th>'\n"
"  +'<th title=\"'+esc(T('t_model'))+'\">'+T('h_model')+'</th>'\n"
"  +'<th title=\"'+esc(T('t_pgns'))+'\">'+T('h_pgns')+'</th>'\n"
"  +'</tr>';\n"
" for(const s of a){if(!s.ident)continue;h+='<tr><td class=n>'+s.src+'</td>'\n"
"  +'<td><input class=ni data-ident=\"'+esc(s.ident)+'\" value=\"'+esc(nm[s.ident]||'')+'\" placeholder=\"—\"></td>'\n"
"  +'<td class=c><input type=checkbox class=ig data-src='+s.src+(ig.indexOf(s.src)>=0?' checked':'')+'></td>'\n"
"  +'<td><code>'+esc(s.ident)+'</code></td><td>'+esc(s.model||s.mfg||'')+'</td>'\n"
"  +'<td>'+(s.pgns||[]).slice().sort((p,q)=>p.pgn-q.pgn).map(p=>'<span class=pill>'+p.pgn+'</span>').join('')+'</td></tr>';}\n"
" h+='</table>';$('#src_body').innerHTML='<div class=card>'+(a.length?h:'<p><small>'+T('no_src')+'</small></p>')+'</div>';\n"
"}catch(e){$('#src_body').innerHTML='<p><small>'+T('na_src')+'</small></p>';}}\n"
"async function saveSrc(){try{const ru=await jget('/api/rules');\n"
" const identByName={};for(const s of (ru.sources||[]))identByName[s.name]=s.ident;\n"
" const newName={};document.querySelectorAll('#src_body input[data-ident]').forEach(i=>{const v=i.value.trim();if(v)newName[i.dataset.ident]=v;});\n"
" const sources=Object.keys(newName).map(id=>({ident:id,name:newName[id]}));\n"
" const rules=(ru.rules||[]).map(r=>({pgn:r.pgn,disc:r.disc,mode:r.mode,sources:r.sources.map(n=>{const id=identByName[n];return id===undefined?n:newName[id];}).filter(Boolean)}));\n"
" const igs=[];document.querySelectorAll('#src_body input.ig:checked').forEach(i=>igs.push(+i.dataset.src));\n"
" const ignore={src:igs,pgn:(ru.ignore&&ru.ignore.pgn)||[]};\n"
" const res=await fetch('/api/config',{method:'POST',body:genIni(ru.talker,sources,rules,ignore,ru.rates,ru.no_n2k,ru.no_0183,ru.talkers,ru.sentences)});const d=await res.json();\n"
" d.ok?smsg(T('names_saved'),'ok'):smsg(T('rej_line')+(d.line||'?')+' : '+(d.err||''),'err');\n"
" if(d.ok)setTimeout(renderSources,2500);\n"
"}catch(e){smsg(T('err_pfx')+e,'err');}}\n"
"function bar(p){p=Math.max(0,Math.min(100,p));return '<span class=bar><i style=\\'width:'+p+'%\\'></i></span> '+H(1,p)+'%';}\n"
"async function updateLoad(){if(!ARB)return;try{const d=await jget('/api/stats');\n"
" const m={},mt={};for(const p of (d.pgns||[])){m[p.pgn]=p.hz;mt[p.pgn]=p.total;}\n"
" // last_msg_age_s : publié par le daemon (horodatage du dernier message reçu).\n"
" // Un débit nul ne dit PAS si la chaîne est morte ou simplement calme ; l'âge, si.\n"
" const age=(d.last_msg_age_s===undefined)?-1:d.last_msg_age_s;\n"
" const dead=(age<0)?T('stale_never'):((age>10)?(T('stale')+' '+H(0,age)+' s'):'');\n"
" $('#arb_load').innerHTML=(dead?'<span class=dead>'+dead+'</span> &nbsp; ':'')+'Bus N2K '+bar(d.bus_load_pct)+' ('+(d.measured?T('measured'):T('estimated'))+') &nbsp; 0183 '+bar(d.out_load_pct)+' &nbsp;<small>'+H(1,d.frames_per_s)+' '+T('frames_s')+' · '+H(1,d.out_sent_per_s)+' '+T('sent_s')+'</small>';\n"
" for(let i=0;i<ARB.units.length;i++){const pg=ARB.units[i].pgn;\n"
"  const c=$('#ld'+i);if(c){const hz=m[pg];c.innerHTML=hz?(H(1,hz)+'<small> Hz</small>'):'';}\n"
"  const tc=$('#tot'+i);if(tc){const tt=mt[pg];tc.textContent=tt?tt:'';}}\n"
"}catch(e){}}\n"
"const PGNNAME={129025:'Position',129026:'COG/SOG',129029:'Position GNSS',129539:'DOP',129540:'Satellites',126992:'Heure',127250:'Cap',127251:'Giration',127257:'Attitude',130306:'Vent',127245:'Barre',129291:'Courant',128259:'Vitesse surface',128267:'Profondeur',128275:'Loch',130316:'Température',130314:'Pression',129038:'AIS pos A',129039:'AIS pos B',129040:'AIS pos B',129041:'AIS AtoN',129794:'AIS statique A',129809:'AIS statique 24A',129810:'AIS statique 24B',60928:'ISO Address Claim',126996:'Product Info',126993:'Heartbeat',59904:'ISO Request'};\n"
"const PGNNAME_EN={129029:'GNSS position',126992:'Time',127250:'Heading',127251:'Rate of turn',130306:'Wind',127245:'Rudder',129291:'Current',128259:'Water speed',128267:'Depth',128275:'Log',130316:'Temperature',130314:'Pressure',129794:'AIS static A',129809:'AIS static 24A',129810:'AIS static 24B'};\n"
"function pgnName(p){return (lang==='en'?(PGNNAME_EN[p]||PGNNAME[p]):PGNNAME[p])||'';}\n"
"const ST={accept:['émis','sent','bok'],reject_priority:['supplanté','superseded','blo'],not_in_rule:['hors-règle','not in rule','bnu'],no_rule:['non réglé','no rule','bnu'],unconfigured:['non configuré','unconfigured','bnu'],unknown_src:['identité ?','identity?','bnu'],ignored:['ignoré','ignored','bnu']};\n"
"const PGN2SENT={129025:['GLL'],129026:['VTG'],129029:['GGA','RMC'],129539:['GSA'],129540:['GSV'],126992:['ZDA'],127250:['HDG','HDM','HDT'],127251:['ROT'],127257:['XDR'],130306:['MWV','MWD'],127245:['RSA'],129291:['VDR'],128259:['VHW'],128267:['DPT'],128275:['VLW'],130316:['MTW','MDA'],130314:['MDA']};\n"
"const AISPGN=[129038,129039,129040,129041,129793,129794,129795,129796,129797,129798,129801,129802,129809,129810];\n"
"function has0183(p){return PGN2SENT[p]!==undefined||AISPGN.indexOf(p)>=0}\n"
"function typesOf(p){return PGN2SENT[p]||(AISPGN.indexOf(p)>=0?['VDM']:[])}\n"
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
" const ig=(ru.ignore&&ru.ignore.pgn)||[];\n"
" for(const p of ig){const k=akey(p,'');if(!seen[k]){seen[k]=1;ARB.units.push({pgn:p,disc:'',mode:'priority',sel:[],obs:[]});}}\n"
" ARB.units.sort((a,b)=>a.pgn-b.pgn||(a.disc||'').localeCompare(b.disc||''));\n"
" const nn=ru.no_n2k||[],no=ru.no_0183||[],tkm={},sm={};\n"
" for(const t of (ru.talkers||[]))tkm[t.pgn]=t.tk;for(const s of (ru.sentences||[]))sm[s.pgn]=s.types;\n"
" for(const u of ARB.units){u.seen=u.obs.length>0;u.n2k=nn.indexOf(u.pgn)<0;u.types=typesOf(u.pgn);u.mappable=u.types.length>0;\n"
"  const off=no.indexOf(u.pgn)>=0;u.on={};u.types.forEach(t=>{u.on[t]=!off&&(sm[u.pgn]===undefined||sm[u.pgn].indexOf(t)>=0);});\n"
"  u.hasRate=PGN2SENT[u.pgn]!==undefined;u.o183ms=u.hasRate?rateOf(PGN2SENT[u.pgn][0]):'';u.tk=tkm[u.pgn]||'';u.ign=ig.indexOf(u.pgn)>=0;}\n"
" amsg('');drawArb();updateLoad();\n"
"}catch(e){$('#arb_body').innerHTML='<p><small>'+T('na_bm')+'</small></p>';}}\n"
"function drawArb(){const dtk=ARB.talker||'II';let h='<table class=arb><tr>'\n"
"  +'<th title=\"'+esc(T('t_pgn'))+'\">PGN</th>'\n"
"  +'<th title=\"'+esc(T('t_mode'))+'\">'+T('a_mode')+'</th>'\n"
"  +'<th class=c title=\"'+esc(T('t_n2k'))+'\">N2K</th>'\n"
"  +'<th class=c title=\"'+esc(T('t_talker'))+'\">'+T('a_talker')+'</th>'\n"
"  +'<th class=c title=\"'+esc(T('t_sent'))+'\">'+T('a_sent')+'</th>'\n"
"  +'<th class=c title=\"'+esc(T('t_ms'))+'\">'+T('a_ms')+'</th>'\n"
"  +'<th class=srccol title=\"'+esc(T('t_sources'))+'\">'+T('a_sources')+' <small>'+T('a_sub')+'</small></th>'\n"
"  +'<th class=c title=\"'+esc(T('t_total'))+'\">'+T('a_total')+'</th>'\n"
"  +'<th class=c title=\"'+esc(T('t_hz'))+'\">'+T('a_hz')+'</th>'\n"
"  +'</tr>';\n"
" const showU=$('#arb_unseen')&&$('#arb_unseen').checked;let shown=0;\n"
" for(let ui=0;ui<ARB.units.length;ui++){const u=ARB.units[ui];if(!showU&&!u.seen)continue;shown++;\n"
"  const cand=[];const add=n=>{if(n&&cand.indexOf(n)<0)cand.push(n)};\n"
"  u.sel.forEach(add);u.obs.forEach(o=>{const nm=o.name||nameOf(o.ident);if(nm)add(nm)});u.cand=cand;\n"
"  h+='<tr'+(u.ign?' class=ign':'')+'><td>'+u.pgn+(u.disc?' /'+esc(u.disc):'')+'<br><small>'+pgnName(u.pgn)+'</small>'\n"
"    +'<br><label class=sl title=\"'+esc(T('t_ign'))+'\"><input type=checkbox data-act=ign data-u='+ui+(u.ign?' checked':'')+'>'+T('ignore_lbl')+'</label></td>';\n"
"  h+='<td><select data-act=mode data-u='+ui+'>'+['priority','min','max','fusion'].map(m=>'<option value='+m+(u.mode===m?' selected':'')+'>'+m+'</option>').join('')+'</select></td>';\n"
"  h+='<td class=c><input type=checkbox data-act=n2k data-u='+ui+(u.n2k?' checked':'')+'></td>';\n"
"  h+='<td class=c>'+(u.hasRate?('<input class=ri style=\"width:3.2em;text-align:center\" maxlength=2 data-act=tk data-u='+ui+' value=\"'+esc(u.tk||'')+'\" placeholder=\"'+esc(dtk)+'\">'):'<small>—</small>')+'</td>';\n"
"  h+='<td class=c>'+(u.types.length?u.types.map(t=>'<label class=sl><input type=checkbox data-act=o183 data-u='+ui+' data-ty='+t+(u.on[t]?' checked':'')+'>'+t+'</label>').join(''):'<small>—</small>')+'</td>';\n"
"  h+='<td class=c>'+(u.hasRate?('<input class=ri type=number min=0 data-act=int data-u='+ui+' value='+(u.o183ms||'')+'>'):'<small>—</small>')+'</td>';\n"
"  h+='<td class=srccol>';\n"
"  cand.forEach((nm,ci)=>{const si=u.sel.indexOf(nm);const o=u.obs.find(x=>(x.name||nameOf(x.ident))===nm)||{};const st=ST[o.status];const slab=st?(lang==='en'?st[1]:st[0]):'';const scl=st?st[2]:'bnu';\n"
"   h+='<span class=chip><input type=checkbox data-act=tog data-u='+ui+' data-ci='+ci+(si>=0?' checked':'')+'>'\n"
"     +'<span class='+(o.alive?'dot':'dotoff')+'>●</span>'+esc(nm)\n"
"     +(si>=0?(' <button class=ob data-act=mv data-u='+ui+' data-i='+si+' data-d=-1>◀</button><button class=ob data-act=mv data-u='+ui+' data-i='+si+' data-d=1>▶</button>'):'')\n"
"     +((slab&&o.status&&o.status!=='accept')?(' <span class='+scl+'>'+slab+'</span>'):'')+'</span>';});\n"
"  u.obs.filter(o=>!(o.name||nameOf(o.ident))).forEach(o=>{h+='<span class=chip><small>src '+o.src+' '+esc(o.ident||'')+' — '+T('to_name')+'</small></span>';});\n"
"  if(!cand.length&&!u.obs.length)h+='<small>—</small>';\n"
"  h+='</td><td class=c id=tot'+ui+'></td><td class=c id=ld'+ui+'></td></tr>';}\n"
" h+='</table>';if(!shown)h+='<p style=\"margin:.4em\"><small>'+T('arb_none')+'</small></p>';$('#arb_body').innerHTML='<div class=card>'+h+'</div>';}\n"
"$('#arb_body').addEventListener('change',e=>{const t=e.target,u=+t.dataset.u;\n"
" if(t.dataset.act==='tog'){const nm=ARB.units[u].cand[+t.dataset.ci],s=ARB.units[u].sel,i=s.indexOf(nm);if(i>=0)s.splice(i,1);else s.push(nm);drawArb();}\n"
" else if(t.dataset.act==='mode'){ARB.units[u].mode=t.value;}\n"
" else if(t.dataset.act==='n2k'){ARB.units[u].n2k=t.checked;}\n"
" else if(t.dataset.act==='o183'){ARB.units[u].on[t.dataset.ty]=t.checked;}\n"
" else if(t.dataset.act==='int'){setInt(u,t.value);}\n"
" else if(t.dataset.act==='tk'){ARB.units[u].tk=(t.value||'').trim().toUpperCase();}\n"
" else if(t.dataset.act==='ign'){ARB.units[u].ign=t.checked;drawArb();}});\n"
"$('#arb_body').addEventListener('click',e=>{const t=e.target;if(t.dataset.act!=='mv')return;const u=+t.dataset.u,i=+t.dataset.i,d=+t.dataset.d,s=ARB.units[u].sel,j=i+d;if(j<0||j>=s.length)return;const x=s[i];s[i]=s[j];s[j]=x;drawArb();});\n"
"function genIni(talker,sources,rules,ignore,rates,no_n2k,no_0183,talkers,sentences){let o='[output]\\ntalker = '+(talker||'II')+'\\n';\n"
" if(no_n2k&&no_n2k.length)o+='no_n2k = '+no_n2k.join(', ')+'\\n';\n"
" if(no_0183&&no_0183.length)o+='no_0183 = '+no_0183.join(', ')+'\\n';\n"
" o+='\\n[sources]\\n';\n"
" for(const s of (sources||[]))if(s.name)o+=s.name+' = '+s.ident+'\\n';\n"
" o+='\\n[priority]\\n';\n"
" for(const r of (rules||[])){if(!r.sources||!r.sources.length)continue;const p=(r.mode&&r.mode!=='priority')?r.mode+': ':'';o+=r.pgn+(r.disc?'/'+r.disc:'')+' = '+p+r.sources.join(', ')+'\\n';}\n"
" o+='\\n[ignore]\\n';if(ignore&&ignore.src&&ignore.src.length)o+='src = '+ignore.src.join(', ')+'\\n';if(ignore&&ignore.pgn&&ignore.pgn.length)o+='pgn = '+ignore.pgn.join(', ')+'\\n';\n"
" o+='\\n[rate]\\n';for(const x of (rates||[]))o+=x.type+' = '+x.ms+'\\n';\n"
" if(talkers&&talkers.length){o+='\\n[talker]\\n';for(const t of talkers)o+=t.pgn+' = '+t.tk+'\\n';}\n"
" if(sentences&&sentences.length){o+='\\n[sentence]\\n';for(const s of sentences)o+=s.pgn+' = '+s.types.join(', ')+'\\n';}\n"
" return o;}\n"
"$('#arb_save').onclick=async()=>{const rules=ARB.units.filter(u=>u.sel.length).map(u=>({pgn:u.pgn,disc:u.disc,mode:u.mode,sources:u.sel}));\n"
" const noN=ARB.units.filter(u=>!u.n2k).map(u=>u.pgn),noO=[],sents=[];\n"
" for(const u of ARB.units){if(!u.types.length)continue;const on=u.types.filter(t=>u.on[t]);\n"
"  if(!on.length)noO.push(u.pgn);else if(on.length<u.types.length)sents.push({pgn:u.pgn,types:on});}\n"
" const tks=ARB.units.filter(u=>u.tk&&u.tk!==(ARB.talker||'II')).map(u=>({pgn:u.pgn,tk:u.tk}));\n"
" const ip=[];for(const u of ARB.units)if(u.ign&&ip.indexOf(u.pgn)<0)ip.push(u.pgn);\n"
" const ign={src:(ARB.ignore&&ARB.ignore.src)||[],pgn:ip};\n"
" const r=await fetch('/api/config',{method:'POST',body:genIni(ARB.talker,ARB.sources,rules,ign,ARB.rates,noN,noO,tks,sents)});const d=await r.json();\n"
" d.ok?amsg(T('arb_saved'),'ok'):amsg(T('rej_line')+(d.line||'?')+' : '+(d.err||''),'err');if(d.ok)setTimeout(loadArb,2500);};\n"
"$('#arb_reload').onclick=loadArb;\n"
"$('#arb_unseen').checked=localStorage.getItem('arb_unseen')==='1';\n"
"$('#arb_unseen').onchange=e=>{localStorage.setItem('arb_unseen',e.target.checked?'1':'');if(ARB)drawArb();};\n"
"$('#src_save').onclick=saveSrc;$('#src_reload').onclick=renderSources;\n"
"// --- Simulateur ------------------------------------------------------------\n"
"// Entrées : cap et vitesse SURFACE, courant, et UNE paire de vent. Le triangle\n"
"// des vitesses interdit d'imposer davantage : route/vitesse fond et les deux\n"
"// autres expressions du vent sont CALCULÉES par n2k-sim, qui les republie dans\n"
"// son fichier d'état — c'est ce qu'affiche le tableau « valeurs déduites ».\n"
"const SIMF={hdg:[0,359,1,'sim_dir'],stw:[0,20,0.1,'sim_kn'],\n"
"            set:[0,359,1,'sim_dir'],drift:[0,6,0.1,'sim_kn'],\n"
"            twd:[0,359,1,'sim_dir'],tws:[0,60,0.5,'sim_kn'],\n"
"            twa:[0,359,1,'sim_dir'],awa:[0,359,1,'sim_dir'],aws:[0,60,0.5,'sim_kn']};\n"
"const SIMDEF={hdg:90,stw:6,set:120,drift:1,twd:225,tws:17,twa:60,awa:45,aws:20};\n"
"const WMODES={twd:['twd','tws'],twa:['twa','tws'],awa:['awa','aws']};\n"
"const SIMST=['hdg','stw','cog','sog','set','drift','twd','tws','twa','awa','aws'];\n"
"let SIM=null,simTimer=null,wmode='twd';\n"
"// smsg() appartient à l'onglet Sources : ne pas réutiliser ce nom ici.\n"
"function simsg(t,cls){const m=$('#sim_msg');m.textContent=t;m.className=t?(cls||'ok'):'';}\n"
"function simActive(){return ['hdg','stw','set','drift'].concat(WMODES[wmode]);}\n"
"async function loadSim(){try{SIM=await jget('/api/sim');}catch(e){$('#sim_body').innerHTML='<small>'+T('sim_na')+'</small>';return;}\n"
" // Le mode de saisie du vent se déduit des clés réellement fixées.\n"
" wmode=(SIM.awa!==null&&SIM.aws!==null)?'awa':((SIM.twa!==null)?'twa':'twd');\n"
" $('#sim_on').checked=!!SIM.enabled;drawSim();drawState();\n"
" simsg(SIM.can_start?'':T('sim_nostart'),'ok');}\n"
"function simRow(k){const f=SIMF[k],v=SIM[k],auto=(v===null||v===undefined),cur=auto?SIMDEF[k]:v;\n"
" return '<tr><td>'+T('sim_'+k)+'</td>'\n"
"  +'<td style=\"width:50%\"><input type=range id=sr_'+k+' min='+f[0]+' max='+f[1]+' step='+f[2]+' value='+cur+(auto?' disabled':'')+' style=\"width:100%\"></td>'\n"
"  +'<td class=n><input class=ri id=sn_'+k+' type=number min='+f[0]+' max='+f[1]+' step='+f[2]+' value='+cur+(auto?' disabled':'')+'> <small>'+T(f[3])+'</small></td>'\n"
"  +'<td class=c><label class=sl><input type=checkbox id=sa_'+k+(auto?' checked':'')+'> '+T('sim_auto')+'</label></td></tr>';}\n"
"function drawSim(){let h='<table><tr><th colspan=4>'+T('sim_boat')+'</th></tr>';\n"
" h+=simRow('hdg')+simRow('stw');\n"
" h+='<tr><th colspan=4>'+T('sim_cur')+'</th></tr>'+simRow('set')+simRow('drift');\n"
" h+='<tr><th colspan=4>'+T('sim_wind')+' &nbsp;<small>'+T('sim_windby')+' </small>'\n"
"  +'<select id=sim_wmode>'\n"
"  +'<option value=twd'+(wmode==='twd'?' selected':'')+'>'+T('sim_by_twd')+'</option>'\n"
"  +'<option value=twa'+(wmode==='twa'?' selected':'')+'>'+T('sim_by_twa')+'</option>'\n"
"  +'<option value=awa'+(wmode==='awa'?' selected':'')+'>'+T('sim_by_awa')+'</option>'\n"
"  +'</select></th></tr>';\n"
" for(const k of WMODES[wmode])h+=simRow(k);\n"
" h+='</table>';$('#sim_body').innerHTML=h;\n"
" for(const k of simActive()){const r=$('#sr_'+k),nb=$('#sn_'+k),au=$('#sa_'+k);\n"
"  r.oninput=()=>{nb.value=r.value;pushSim();};\n"
"  nb.oninput=()=>{r.value=nb.value;pushSim();};\n"
"  au.onchange=()=>{r.disabled=nb.disabled=au.checked;pushSim();};}\n"
" $('#sim_wmode').onchange=e=>{wmode=e.target.value;\n"
"  // On repart des valeurs courantes : les clés de l'ancien mode passent à auto.\n"
"  for(const k of WMODES[wmode])if(SIM[k]===null||SIM[k]===undefined)SIM[k]=SIMDEF[k];\n"
"  drawSim();pushSim();};}\n"
"// Tableau des valeurs déduites, relu dans l'état publié par le simulateur.\n"
"function drawState(){const st=(SIM&&SIM.state)||{};const act=simActive();\n"
" if(!Object.keys(st).length){$('#sim_state').innerHTML='<small>'+T('sim_nostate')+'</small>';return;}\n"
" let h='<table>';\n"
" for(const k of SIMST){if(st[k]===undefined)continue;\n"
"  const forced=act.indexOf(k)>=0&&SIM[k]!==null&&SIM[k]!==undefined;\n"
"  h+='<tr><td>'+T('sim_'+k)+'</td><td class=n>'+H(SIMF[k]&&SIMF[k][2]<1?2:1,st[k])\n"
"   +' <small>'+T((k==='stw'||k==='sog'||k==='drift'||k==='tws'||k==='aws')?'sim_kn':'sim_dir')+'</small></td>'\n"
"   +'<td><small class='+(forced?'bnu':'bok')+'>'+T(forced?'sim_set_lbl':'sim_calc')+'</small></td></tr>';}\n"
" h+='</table>';$('#sim_state').innerHTML=h;}\n"
"function simBody(){let t='enabled = '+($('#sim_on').checked?1:0)+'\\n';\n"
" const act=simActive();\n"
" for(const k in SIMF){\n"
"  const on=act.indexOf(k)>=0&&!$('#sa_'+k).checked;\n"
"  t+=k+' = '+(on?$('#sn_'+k).value:'auto')+'\\n';}\n"
" return t;}\n"
"// Un geste de curseur produit beaucoup d'événements : on n'écrit qu'une fois\n"
"// la main relâchée (250 ms sans changement).\n"
"function pushSim(){clearTimeout(simTimer);simTimer=setTimeout(sendSim,250);}\n"
"async function sendSim(){try{const r=await fetch('/api/sim',{method:'POST',body:simBody()});\n"
"  const d=await r.json();\n"
"  if(!d.ok){simsg(T('err_pfx')+(d.err||''),'err');return;}\n"
"  simsg((SIM&&SIM.can_start)?T('sim_saved'):T('sim_nostart'),'ok');\n"
" }catch(e){simsg(T('err_pfx')+e,'err');}}\n"
"// Rafraîchit l'état déduit sans toucher aux champs en cours d'édition.\n"
"async function refreshState(){try{const d=await jget('/api/sim');if(SIM){SIM.state=d.state;drawState();}}catch(e){}}\n"
"$('#sim_on').onchange=pushSim;$('#sim_reload').onclick=loadSim;\n"
"function tick(){$('#clock').textContent=new Date().toLocaleTimeString();\n"
" if($('#arbitrage').classList.contains('on'))updateLoad();\n"
" if($('#sim').classList.contains('on'))refreshState();}\n"
"function applyTheme(){document.body.classList.toggle('light',theme==='light');$('#theme').textContent=theme==='dark'?'☀':'🌙';}\n"
"function applyLang(){$('#lang').textContent=lang==='fr'?'EN':'FR';applyI18n();\n"
" if($('#arbitrage').classList.contains('on'))loadArb();\n"
" else if($('#sim').classList.contains('on'))loadSim();\n"
" else renderSources();}\n"
"$('#lang').onclick=()=>{lang=(lang==='fr')?'en':'fr';localStorage.setItem('lang',lang);applyLang();};\n"
"$('#theme').onclick=()=>{theme=(theme==='dark')?'light':'dark';localStorage.setItem('theme',theme);applyTheme();};\n"
"if(window.matchMedia)matchMedia('(prefers-color-scheme: light)').addEventListener('change',e=>{\n"
" if(!localStorage.getItem('theme')){theme=e.matches?'light':'dark';applyTheme();}});\n"
"applyTheme();$('#lang').textContent=lang==='fr'?'EN':'FR';applyI18n();\n"
"renderSources();tick();setInterval(tick,3000);\n"
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

/* Copie `path` en `path`.bak. L'éditeur régénère l'INI ENTIER depuis les
 * structures parsées : commentaires et mise en page de la version précédente
 * sont perdus, d'où cette sauvegarde systématique avant écrasement.
 * Best-effort : l'absence de fichier source n'est pas une erreur. */
static void backup_file(const char *path)
{
    static char buf[FILE_MAX];
    size_t len = 0;
    if (read_file(path, buf, sizeof buf, &len) != 0 || len == 0)
        return;
    char bak[576];
    snprintf(bak, sizeof bak, "%s.bak", path);
    (void)write_file(bak, buf, len);
}

/* Écriture ATOMIQUE : temporaire + rename, comme le daemon pour ses JSON. Un
 * O_TRUNC direct laissait une config tronquée si l'écriture échouait en cours
 * — et le daemon refuse alors de (re)démarrer. */
static int write_file_atomic(const char *path, const char *buf, size_t len)
{
    /* Crée le répertoire parent au besoin (best-effort), comme le daemon le
     * fait pour ses JSON : le fichier de pilotage du simulateur peut être le
     * premier à y être écrit. */
    char dircopy[512];
    snprintf(dircopy, sizeof dircopy, "%s", path);
    char *slash = strrchr(dircopy, '/');
    if (slash && slash != dircopy) {
        *slash = '\0';
        mkdir(dircopy, 0755);
    }
    char tmp[576];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (write_file(tmp, buf, len) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
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

/* base64 standard de `in` (terminée NUL) dans `out`. */
static void b64encode(const char *in, char *out, size_t cap)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), i = 0, o = 0;
    while (i < n && o + 4 < cap) {
        unsigned v = (unsigned char)in[i++] << 16; int rem = 1;
        if (i < n) { v |= (unsigned char)in[i++] << 8; rem = 2; }
        if (i < n) { v |= (unsigned char)in[i++];      rem = 3; }
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = rem >= 2 ? T[(v >> 6) & 63] : '=';
        out[o++] = rem >= 3 ? T[v & 63]        : '=';
    }
    out[o] = '\0';
}

/* Comparaison à temps constant (évite le timing-oracle sur le mot de passe). */
static int ct_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), n = la < lb ? la : lb;
    unsigned d = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; i++) d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}

/* 401 avec en-tête WWW-Authenticate (déclenche le prompt du navigateur). */
static void send_401(int fd)
{
    static const char *r =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"n2k-mux\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\nContent-Length: 16\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
        "401 Unauthorized";
    ssize_t w = write(fd, r, strlen(r)); (void)w;
}

/* Requête authentifiée ? (Authorization: Basic <base64> == g_auth_b64). */
static int authed(const char *req)
{
    if (!g_auth) return 1;                 /* auth désactivée */
    const char *h = strcasestr(req, "Authorization:");
    if (!h) return 0;
    const char *b = strcasestr(h, "Basic ");
    if (!b) return 0;
    b += 6;
    char tok[sizeof g_auth_b64]; size_t i = 0;
    while (b[i] && b[i] != '\r' && b[i] != '\n' && b[i] != ' ' && i < sizeof tok - 1) {
        tok[i] = b[i]; i++;
    }
    tok[i] = '\0';
    return ct_eq(tok, g_auth_b64);
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

/* --- Simulateur ------------------------------------------------------------
 * Le fichier de pilotage est un simple « clé = valeur » (cf. n2k-sim
 * --control) : l'UI l'envoie tel quel, on le valide puis on l'écrit. Pas de
 * parser JSON côté serveur, et le simulateur lit le même format. */

/* Réglages acceptés. Le vent est défini par UNE paire : awa+aws, twa+tws ou
 * twd+tws (le simulateur applique cette priorité) ; les autres clés sont
 * écrites à « auto ». Le triangle des vitesses interdit de tout imposer. */
static const char *SIM_KEYS[] = {
    "hdg", "stw", "set", "drift", "twd", "tws", "twa", "awa", "aws"
};
#define SIM_NKEYS ((int)(sizeof SIM_KEYS / sizeof *SIM_KEYS))

/* Grandeurs publiées par --state (état déduit, lecture seule). */
static const char *SIM_STATE_KEYS[] = {
    "hdg", "stw", "cog", "sog", "set", "drift",
    "twd", "tws", "twa", "awa", "aws", "lat", "lon"
};
#define SIM_NSTATE ((int)(sizeof SIM_STATE_KEYS / sizeof *SIM_STATE_KEYS))

/* Lit une clé du fichier de pilotage. Retourne 1 si présente et numérique,
 * 0 si absente ou « auto ». */
static int sim_get(const char *text, const char *key, double *out)
{
    char pat[32];
    snprintf(pat, sizeof pat, "%s", key);
    for (const char *p = text; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        size_t kl = strlen(pat);
        if (strncasecmp(p, pat, kl) == 0) {
            const char *q = p + kl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                if (strncasecmp(q, "auto", 4) == 0) return 0;
                char *end = NULL;
                double v = strtod(q, &end);
                if (end && end != q) { *out = v; return 1; }
                return 0;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    return 0;
}

/* GET /api/sim : état courant du pilotage, en JSON (null = automatique). */
static void serve_sim(int fd)
{
    static char buf[4096];
    size_t len = 0;
    if (read_file(g_sim_path, buf, sizeof buf, &len) != 0)
        buf[0] = '\0';

    double en = 1;
    int    enabled = sim_get(buf, "enabled", &en) ? (en != 0) : 1;

    static char sbuf[4096];
    size_t slen = 0;
    if (read_file(g_sim_state, sbuf, sizeof sbuf, &slen) != 0)
        sbuf[0] = '\0';

    char out[1024];
    size_t n = 0;
    int w = snprintf(out, sizeof out, "{\"enabled\":%s,\"can_start\":%s",
                     enabled ? "true" : "false",
                     (g_sim_start && g_sim_start[0]) ? "true" : "false");
    if (w < 0 || (size_t)w >= sizeof out) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
    n += (size_t)w;
    for (int i = 0; i < SIM_NKEYS; i++) {
        double v = 0;
        int has = sim_get(buf, SIM_KEYS[i], &v);
        w = has ? snprintf(out + n, sizeof out - n, ",\"%s\":%.2f", SIM_KEYS[i], v)
                : snprintf(out + n, sizeof out - n, ",\"%s\":null", SIM_KEYS[i]);
        if (w < 0 || (size_t)w >= sizeof out - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
        n += (size_t)w;
    }
    /* état déduit, tel que le simulateur le calcule (vide s'il ne tourne pas) */
    w = snprintf(out + n, sizeof out - n, ",\"state\":{");
    if (w < 0 || (size_t)w >= sizeof out - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
    n += (size_t)w;
    int first = 1;
    for (int i = 0; i < SIM_NSTATE; i++) {
        double v = 0;
        if (!sim_get(sbuf, SIM_STATE_KEYS[i], &v))
            continue;
        w = snprintf(out + n, sizeof out - n, "%s\"%s\":%.2f",
                     first ? "" : ",", SIM_STATE_KEYS[i], v);
        if (w < 0 || (size_t)w >= sizeof out - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
        n += (size_t)w;
        first = 0;
    }
    snprintf(out + n, sizeof out - n, "}}");
    send_text(fd, 200, "OK", "application/json", out);
}

/* POST /api/sim : le corps est le fichier de pilotage. On n'accepte que les
 * clés connues et des valeurs numériques ou « auto », puis on réécrit le
 * fichier proprement (temporaire + rename) — le simulateur le relit seul. */
static void handle_sim_post(int fd, const char *body)
{
    double en = 1;
    int enabled = sim_get(body, "enabled", &en) ? (en != 0) : 1;

    char text[1024];
    size_t n = 0;
    int w = snprintf(text, sizeof text,
                     "# n2k-sim : pilotage écrit par l'interface web\n"
                     "# vitesses en nœuds, caps en degrés, « auto » = sinusoïde\n"
                     "enabled = %d\n", enabled ? 1 : 0);
    if (w < 0) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
    n += (size_t)w;

    for (int i = 0; i < SIM_NKEYS; i++) {
        double v = 0;
        if (!sim_get(body, SIM_KEYS[i], &v)) {
            w = snprintf(text + n, sizeof text - n, "%s = auto\n", SIM_KEYS[i]);
        } else {
            /* bornes : un cap tourne, une vitesse ne descend pas sous zéro */
            int is_dir = (strcmp(SIM_KEYS[i], "cog") == 0 ||
                          strcmp(SIM_KEYS[i], "set") == 0 ||
                          strcmp(SIM_KEYS[i], "twd") == 0);
            if (is_dir) {
                while (v < 0)    v += 360;
                while (v >= 360) v -= 360;
            } else if (v < 0) {
                v = 0;
            }
            w = snprintf(text + n, sizeof text - n, "%s = %.2f\n", SIM_KEYS[i], v);
        }
        if (w < 0 || (size_t)w >= sizeof text - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
        n += (size_t)w;
    }

    if (write_file_atomic(g_sim_path, text, n) != 0) {
        char out[256];
        snprintf(out, sizeof out, "{\"ok\":false,\"err\":\"écriture impossible (%s)\"}",
                 strerror(errno));
        send_text(fd, 200, "OK", "application/json", out);
        return;
    }

    /* Commandes de chaîne, si l'exploitant les a configurées. */
    const char *cmd = enabled ? g_sim_start : g_sim_stop;
    int ran = 0;
    if (cmd && cmd[0])
        ran = (system(cmd) == 0) ? 1 : -1;

    char out[256];
    snprintf(out, sizeof out, "{\"ok\":true,\"enabled\":%s,\"cmd\":%d}",
             enabled ? "true" : "false", ran);
    send_text(fd, 200, "OK", "application/json", out);
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
    APP("],\"talkers\":[");
    for (int i = 0; i < c.n_talkers; i++) {
        json_escape(c.talkers[i].talker, e, sizeof e);
        APP("%s{\"pgn\":%d,\"tk\":\"%s\"}", i ? "," : "", c.talkers[i].pgn, e);
    }
    /* Listes blanches de phrases [sentence], regroupées par PGN. */
    APP("],\"sentences\":[");
    {
        int first = 1;
        for (int i = 0; i < c.n_sentences; i++) {
            int pgn = c.sentences[i].pgn, seen = 0;
            for (int j = 0; j < i; j++)
                if (c.sentences[j].pgn == pgn) { seen = 1; break; }
            if (seen) continue;
            APP("%s{\"pgn\":%d,\"types\":[", first ? "" : ",", pgn);
            first = 0;
            int nt = 0;
            for (int j = 0; j < c.n_sentences; j++) {
                if (c.sentences[j].pgn != pgn) continue;
                json_escape(c.sentences[j].type, e, sizeof e);
                APP("%s\"%s\"", nt ? "," : "", e);
                nt++;
            }
            APP("]}");
        }
    }
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
        /* L'éditeur envoie un INI régénéré, sans commentaires. On le FUSIONNE
         * dans le fichier existant pour n'y changer que les valeurs et garder
         * commentaires, ordre et alignement. Si la fusion déborde ou si son
         * résultat ne se relit pas, on écrit le contenu régénéré tel quel :
         * l'édition de l'utilisateur ne doit jamais être perdue. */
        const char *to_write = body;
        size_t      to_len   = strlen(body);
        static char oldbuf[FILE_MAX], merged[FILE_MAX];
        size_t      oldlen = 0;
        if (g_cfg_path &&
            read_file(g_cfg_path, oldbuf, sizeof oldbuf, &oldlen) == 0 && oldlen > 0) {
            int mn = ini_merge(oldbuf, body, merged, sizeof merged);
            if (mn > 0) {
                config_t chk;
                config_init(&chk);
                if (config_parse_string(&chk, merged)) {
                    to_write = merged;
                    to_len   = (size_t)mn;
                }
            }
        }
        if (g_cfg_path)
            backup_file(g_cfg_path);   /* .bak : filet en cas de fusion imparfaite */
        if (!g_cfg_path || write_file_atomic(g_cfg_path, to_write, to_len) != 0) {
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

    if (!authed(req)) { send_401(fd); return; }

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
        else if (strcmp(path, "/api/sim") == 0)
            serve_sim(fd);
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
        else if (strcmp(path, "/api/sim") == 0)
            handle_sim_post(fd, body);
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
    int allow_anon = 0;   /* --allow-anonymous : écoute réseau sans auth, assumée */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sources") == 0 && i + 1 < argc) g_sources_path = argv[++i];
        else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) g_stats_path = argv[++i];
        else if (strcmp(argv[i], "--busmap") == 0 && i + 1 < argc) g_busmap_path = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) bind_addr = argv[++i];
        else if (strcmp(argv[i], "--reload-cmd") == 0 && i + 1 < argc) g_reload_cmd = argv[++i];
        else if (strcmp(argv[i], "--auth") == 0 && i + 1 < argc) g_auth = argv[++i];
        else if (strcmp(argv[i], "--allow-anonymous") == 0) allow_anon = 1;
        else if (strcmp(argv[i], "--sim-control") == 0 && i + 1 < argc) g_sim_path = argv[++i];
        else if (strcmp(argv[i], "--sim-state") == 0 && i + 1 < argc) g_sim_state = argv[++i];
        else if (strcmp(argv[i], "--sim-start") == 0 && i + 1 < argc) g_sim_start = argv[++i];
        else if (strcmp(argv[i], "--sim-stop") == 0 && i + 1 < argc) g_sim_stop = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "Usage : %s [config.ini] [--sources P] [--stats P] [--busmap P]\n"
                "          [--port N] [--bind ADDR] [--reload-cmd CMD] [--auth user:pass]\n"
                "  config.ini    fichier INI édité par l'interface\n"
                "  --busmap P    carte (pgn/disc)->sources pour l'onglet Arbitrage\n"
                "  --port N      port d'écoute (défaut 8080)\n"
                "  --bind ADDR   adresse d'écoute (défaut 127.0.0.1 ; 0.0.0.0 = LAN)\n"
                "  --reload-cmd  commande lancée après sauvegarde (ex. \"pkill -HUP -x n2k-mux\")\n"
                "  --auth        exige une authentification HTTP Basic (user:pass)\n"
                "                (ou variable d'environnement N2K_MUX_WEB_AUTH)\n"
                "  --allow-anonymous  autorise l'écoute réseau SANS authentification\n"
                "  --sim-control P    fichier de pilotage du simulateur\n"
                "                (défaut /etc/n2k-mux/sim.ctl ; cf. n2k-sim --control)\n"
                "  --sim-state P      état déduit publié par n2k-sim --state\n"
                "                (défaut /run/n2k-mux/sim.state ; affiché par l'UI)\n"
                "  --sim-start CMD    commande lançant la chaîne simulée (optionnel)\n"
                "  --sim-stop CMD     commande arrêtant la chaîne simulée (optionnel)\n",
                argv[0]);
            return 0;
        } else if (argv[i][0] != '-') g_cfg_path = argv[i];
        else { fprintf(stderr, "option inconnue : %s\n", argv[i]); return 2; }
    }

    /* Le credential peut venir de l'environnement plutôt que de la ligne de
     * commande : argv est lisible par tout utilisateur local via /proc. */
    if (!g_auth) {
        const char *env = getenv("N2K_MUX_WEB_AUTH");
        if (env && env[0]) g_auth = env;
    }
    if (g_auth) {
        if (!strchr(g_auth, ':')) {
            fprintf(stderr, "--auth attend le format user:pass\n"); return 2;
        }
        if (strlen(g_auth) > AUTH_RAW_MAX) {
            fprintf(stderr, "--auth : credential trop long (max %d caractères)\n",
                    AUTH_RAW_MAX); return 2;
        }
        b64encode(g_auth, g_auth_b64, sizeof g_auth_b64);
    }

    /* L'interface ÉCRIT la config et lance la commande de reload : l'exposer
     * hors de la boucle locale sans authentification donne ce pouvoir à tout le
     * réseau. On refuse, sauf demande explicite (--allow-anonymous). */
    if (!g_auth && !allow_anon && strncmp(bind_addr, "127.", 4) != 0 &&
        strcmp(bind_addr, "::1") != 0 && strcmp(bind_addr, "localhost") != 0) {
        fprintf(stderr,
            "n2k-mux-web : refus d'écouter sur %s sans authentification.\n"
            "  Poser WEB_AUTH=user:pass dans /etc/default/n2k-mux (ou --auth user:pass),\n"
            "  ou --bind 127.0.0.1, ou --allow-anonymous pour assumer le risque.\n",
            bind_addr);
        return 2;
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

    fprintf(stderr, "n2k-mux-web : http://%s:%d/  (config: %s, auth: %s)\n",
            bind_addr, port, g_cfg_path ? g_cfg_path : "(aucune)",
            g_auth ? "oui" : "non");

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        /* 2 s : le serveur est séquentiel, un client qui se connecte sans rien
         * envoyer bloque tous les autres pendant ce délai. */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        handle_client(fd);
        close(fd);
    }
    close(ls);
    return 0;
}
