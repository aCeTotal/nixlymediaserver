/*
 * Nixly Media Server - HTML Pages
 */

#include "pages.h"

/* Shared CSS — modern dark theme with accent gradient, soft cards, subtle motion. */
#define NIXLY_STYLE \
"<style>" \
":root{" \
"--bg:#0b0d12;--bg2:#0f131a;--surface:#161b25;--surface2:#1c2230;" \
"--border:#262e3d;--border2:#323b50;" \
"--text:#e7ecf3;--muted:#8d97ad;--dim:#5d6679;" \
"--accent:#7c5cff;--accent2:#22d3ee;--ok:#4ade80;--warn:#fbbf24;--err:#f87171;" \
"--shadow:0 10px 30px rgba(0,0,0,.45);--shadow-sm:0 4px 12px rgba(0,0,0,.35);" \
"}" \
"*{box-sizing:border-box}" \
"html,body{margin:0;padding:0}" \
"body{" \
"background:radial-gradient(1200px 600px at 80% -10%,rgba(124,92,255,.15),transparent 60%)," \
"radial-gradient(900px 500px at -10% 10%,rgba(34,211,238,.10),transparent 55%)," \
"linear-gradient(180deg,var(--bg),var(--bg2));" \
"color:var(--text);font:14px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Inter,Roboto,sans-serif;" \
"min-height:100vh;-webkit-font-smoothing:antialiased;" \
"}" \
"a{color:var(--accent2);text-decoration:none;transition:color .15s}" \
"a:hover{color:#fff}" \
"header{" \
"position:sticky;top:0;z-index:50;backdrop-filter:blur(12px);" \
"background:rgba(11,13,18,.7);border-bottom:1px solid var(--border);" \
"}" \
".hwrap{max-width:1280px;margin:0 auto;padding:14px 28px;display:flex;align-items:center;gap:24px;}" \
".logo{font-weight:700;font-size:17px;letter-spacing:.2px;display:flex;align-items:center;gap:10px}" \
".logo .dot{width:10px;height:10px;border-radius:50%;background:linear-gradient(135deg,var(--accent),var(--accent2));box-shadow:0 0 12px rgba(124,92,255,.7)}" \
"nav{display:flex;gap:4px;margin-left:8px}" \
"nav a{padding:8px 14px;border-radius:8px;color:var(--muted);font-weight:500;font-size:13px;transition:all .15s}" \
"nav a:hover{background:var(--surface);color:var(--text)}" \
"nav a.active{background:var(--surface2);color:var(--text)}" \
"main{max-width:1280px;margin:0 auto;padding:32px 28px 80px;}" \
"h1{font-size:28px;font-weight:700;margin:0 0 24px;letter-spacing:-.3px;}" \
"h2{font-size:15px;font-weight:600;text-transform:uppercase;letter-spacing:1.2px;color:var(--muted);margin:36px 0 14px;}" \
"h2:first-child{margin-top:0}" \
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:14px;}" \
".card{" \
"background:linear-gradient(160deg,var(--surface),var(--surface2));" \
"border:1px solid var(--border);border-radius:14px;overflow:hidden;" \
"transition:transform .15s,border-color .15s,box-shadow .15s;" \
"display:flex;flex-direction:column;" \
"}" \
".card.padded{padding:16px}" \
".card:hover{transform:translateY(-3px);border-color:var(--border2);box-shadow:var(--shadow);}" \
".card .poster-wrap{aspect-ratio:2/3;background:linear-gradient(135deg,#1c2230,#0f131a);background-size:cover;background-position:center;position:relative;overflow:hidden;}" \
".card .poster-wrap::after{content:'';position:absolute;inset:0;background:linear-gradient(180deg,transparent 60%,rgba(0,0,0,.65));}" \
".card .poster-empty{display:flex;align-items:center;justify-content:center;height:100%;color:var(--dim);font-size:32px;}" \
".card .body{padding:12px 14px;flex:1;display:flex;flex-direction:column;gap:4px;}" \
".card .title{font-weight:600;font-size:14px;color:var(--text);line-height:1.3;overflow:hidden;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;text-overflow:ellipsis;}" \
".card .title a{color:inherit;cursor:pointer}" \
".card .title a:hover{color:var(--accent2)}" \
".card .meta{color:var(--muted);font-size:12px;display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-top:auto;}" \
".card .meta .sep{opacity:.4}" \
".grid.posters{grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:16px;}" \
".bar{background:var(--surface);border-radius:999px;height:8px;overflow:hidden;position:relative}" \
".bar>span{display:block;height:100%;background:linear-gradient(90deg,var(--accent),var(--accent2));border-radius:999px;transition:width .4s ease;position:relative;overflow:hidden}" \
".bar>span::after{content:'';position:absolute;inset:0;background:linear-gradient(90deg,transparent,rgba(255,255,255,.3),transparent);animation:shine 1.8s linear infinite}" \
"@keyframes shine{0%{transform:translateX(-100%)}100%{transform:translateX(100%)}}" \
".dl{" \
"background:linear-gradient(160deg,var(--surface),var(--surface2));" \
"border:1px solid var(--border);border-radius:14px;padding:16px 18px;margin-bottom:10px;" \
"}" \
".dl .row1{display:flex;align-items:center;gap:10px;margin-bottom:10px}" \
".dl .name{font-weight:600;font-size:14px;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}" \
".dl .meta{color:var(--muted);font-size:12.5px;display:flex;gap:10px;margin-top:8px;flex-wrap:wrap;}" \
".pill{" \
"display:inline-flex;align-items:center;padding:3px 10px;border-radius:999px;" \
"font-size:11px;font-weight:600;letter-spacing:.4px;text-transform:uppercase;" \
"border:1px solid transparent;" \
"}" \
".pill.tv{background:rgba(34,211,238,.12);color:#67e8f9;border-color:rgba(34,211,238,.3)}" \
".pill.movie{background:rgba(251,191,36,.12);color:#fcd34d;border-color:rgba(251,191,36,.3)}" \
".pill.active{background:rgba(124,92,255,.14);color:#a78bfa;border-color:rgba(124,92,255,.35)}" \
".pill.done{background:rgba(74,222,128,.12);color:#86efac;border-color:rgba(74,222,128,.3)}" \
".pill.failed{background:rgba(248,113,113,.12);color:#fca5a5;border-color:rgba(248,113,113,.35)}" \
".pill.queued{background:rgba(141,151,173,.12);color:#cbd5e1;border-color:rgba(141,151,173,.3)}" \
".empty{color:var(--muted);text-align:center;padding:48px 16px;border:1px dashed var(--border);border-radius:14px;}" \
".back{display:inline-flex;align-items:center;gap:6px;color:var(--muted);font-size:13px;margin-bottom:14px;cursor:pointer;padding:6px 10px;border-radius:8px;transition:all .15s}" \
".back:hover{background:var(--surface);color:var(--text)}" \
".eplist{background:linear-gradient(160deg,var(--surface),var(--surface2));border:1px solid var(--border);border-radius:14px;overflow:hidden}" \
".eplist .ep{padding:12px 18px;border-bottom:1px solid var(--border);display:flex;gap:12px;align-items:center;transition:background .15s}" \
".eplist .ep:last-child{border-bottom:0}" \
".eplist .ep:hover{background:var(--surface2)}" \
".eplist .ep .num{font-family:'SF Mono','Consolas',monospace;background:var(--surface);padding:3px 9px;border-radius:6px;font-size:12px;color:var(--accent2);font-weight:600;min-width:42px;text-align:center;}" \
".stream{display:flex;gap:14px;background:linear-gradient(160deg,var(--surface),var(--surface2));border:1px solid var(--border);border-radius:14px;padding:14px;margin-bottom:10px;align-items:stretch;}" \
".stream .poster{width:60px;min-width:60px;height:90px;border-radius:8px;background:var(--surface);background-size:cover;background-position:center;flex-shrink:0;box-shadow:0 4px 10px rgba(0,0,0,.4);}" \
".stream .info{flex:1;display:flex;flex-direction:column;min-width:0;}" \
".stream .title{font-weight:600;font-size:15px;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}" \
".stream .subtitle{color:var(--muted);font-size:13px;margin-top:2px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}" \
".stream .footer{margin-top:auto;display:flex;gap:8px;align-items:center;color:var(--muted);font-size:12px;}" \
".stream .ip{font-family:'SF Mono','Consolas',monospace;color:var(--accent2);background:rgba(34,211,238,.08);padding:2px 8px;border-radius:6px;}" \
".stream .live{display:inline-flex;align-items:center;gap:6px;color:#86efac;font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:1px;}" \
".stream .live::before{content:'';width:8px;height:8px;border-radius:50%;background:#4ade80;box-shadow:0 0 8px #4ade80;animation:pulse 1.4s ease-in-out infinite;}" \
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}" \
".stream .actions{display:flex;align-items:flex-start;}" \
".btn-block{margin:0;padding:7px 14px;background:linear-gradient(135deg,#f87171,#fbbf24);box-shadow:0 4px 10px rgba(248,113,113,.3);font-size:12px;}" \
".btn-block:hover{box-shadow:0 6px 14px rgba(248,113,113,.45);}" \
".btn-unblock{margin:0;padding:7px 14px;background:linear-gradient(135deg,#4ade80,#22d3ee);color:#0b0d12;box-shadow:0 4px 10px rgba(74,222,128,.3);font-size:12px;}" \
"form .row{display:flex;gap:10px;margin:8px 0;align-items:center}" \
"form .row label{width:24px;color:var(--dim);text-align:right;font-size:13px;font-weight:600;font-family:'SF Mono','Consolas',monospace}" \
"form .row input,form .row select{" \
"padding:11px 14px;background:var(--surface);color:var(--text);border:1px solid var(--border);" \
"border-radius:10px;font:inherit;transition:border-color .15s,background .15s;outline:none;" \
"}" \
"form .row input{flex:1}" \
"form .row input:focus,form .row select:focus{border-color:var(--accent);background:var(--surface2)}" \
"form .row input::placeholder{color:var(--dim)}" \
"form .row select{cursor:pointer;min-width:110px}" \
"button{" \
"margin-top:20px;padding:12px 24px;" \
"background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff;border:0;border-radius:10px;" \
"font:600 14px/1 inherit;cursor:pointer;letter-spacing:.3px;" \
"box-shadow:0 6px 18px rgba(124,92,255,.4);transition:transform .15s,box-shadow .15s;" \
"}" \
"button:hover{transform:translateY(-1px);box-shadow:0 10px 24px rgba(124,92,255,.55)}" \
"button:active{transform:translateY(0)}" \
"#result{margin-left:14px;color:var(--ok);font-size:13px}" \
".lead{color:var(--muted);margin:0 0 24px;max-width:60ch}" \
".stat-row{display:flex;gap:14px;margin-bottom:24px;flex-wrap:wrap}" \
".stat{flex:1;min-width:160px;background:linear-gradient(160deg,var(--surface),var(--surface2));border:1px solid var(--border);border-radius:14px;padding:16px 18px;}" \
".stat .label{color:var(--muted);font-size:11.5px;text-transform:uppercase;letter-spacing:1.2px;font-weight:600}" \
".stat .value{font-size:24px;font-weight:700;margin-top:4px;background:linear-gradient(135deg,var(--text),var(--accent2));-webkit-background-clip:text;background-clip:text;color:transparent}" \
"@media(max-width:640px){.hwrap{padding:12px 16px}main{padding:24px 16px}h1{font-size:22px}}" \
"</style>"

#define NIXLY_HEADER(active_) \
"<header><div class=\"hwrap\">" \
"<div class=\"logo\"><span class=\"dot\"></span>Nixly</div>" \
"<nav>" \
"<a href=\"/\"" active_ "_home>Home</a>" \
"<a href=\"/status\"" active_ "_status>Library</a>" \
"<a href=\"/wget\"" active_ "_wget>Downloader</a>" \
"</nav>" \
"</div></header>"

/* Helper: add class="active" to the nav link matching the current page.
 * We emit literal strings — the chosen page gets ` class="active"`, others get "". */

const char *page_index_html =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Nixly Media Server</title>"
NIXLY_STYLE
"</head><body>"
"<header><div class=\"hwrap\">"
"<div class=\"logo\"><span class=\"dot\"></span>Nixly</div>"
"<nav>"
"<a href=\"/\" class=\"active\">Home</a>"
"<a href=\"/status\">Library</a>"
"<a href=\"/wget\">Downloader</a>"
"</nav>"
"</div></header>"
"<main>"
"<h1>Dashboard</h1>"
"<p class=\"lead\">Live overview of streams, downloads, and TMDB scraper.</p>"
"<div class=\"stat-row\">"
"<div class=\"stat\"><div class=\"label\">Stream Gate</div><div class=\"value\" id=\"s-gate\">—</div><div class=\"meta\" id=\"s-gate-sub\" style=\"margin-top:6px\"></div></div>"
"<div class=\"stat\"><div class=\"label\">Library</div><div class=\"value\" id=\"s-lib\">—</div></div>"
"<div class=\"stat\"><div class=\"label\">TMDB Pending</div><div class=\"value\" id=\"s-pen\">—</div></div>"
"<div class=\"stat\"><div class=\"label\">Active Downloads</div><div class=\"value\" id=\"s-dl\">—</div></div>"
"</div>"
"<h2>Active Streams</h2>"
"<div id=\"streams\"><div class=\"empty\">No active streams</div></div>"
"<h2>Blocked IPs</h2>"
"<div id=\"blocked\"><div class=\"empty\">None blocked</div></div>"
"<h2>Downloads</h2>"
"<div id=\"downloads\"><div class=\"empty\">No active downloads</div></div>"
"<h2>Scraper</h2>"
"<div id=\"scrape\" class=\"dl\"><div class=\"meta\">Loading…</div></div>"
"</main>"
"<script>"
"function fmtBytes(b){if(!b)return'0 B';var u=['B','KB','MB','GB','TB'];var i=0;"
"while(b>=1024&&i<4){b/=1024;i++;}return b.toFixed(1)+' '+u[i];}"
"function fmtSpeed(b){return b?fmtBytes(b)+'/s':'—';}"
"function fmtTime(s){if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+(s%60)+'s';return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';}"
"function esc(s){return (s||'').replace(/[&<>\"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c];});}"
"async function blockIp(ip){"
"  if(!confirm('Block '+ip+'? They will be disconnected and refused new streams.'))return;"
"  await fetch('/api/blocked/add',{method:'POST',"
"    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"    body:'ip='+encodeURIComponent(ip)});refresh();"
"}"
"async function unblockIp(ip){"
"  await fetch('/api/blocked/remove',{method:'POST',"
"    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"    body:'ip='+encodeURIComponent(ip)});refresh();"
"}"
"async function refresh(){"
"  try{var r=await fetch('/api/streams/active');var sd=await r.json();"
"    var c=document.getElementById('streams');"
"    if(!sd.streams||sd.streams.length===0){"
"      c.innerHTML='<div class=\"empty\">No active streams</div>';"
"    }else{"
"      c.innerHTML=sd.streams.map(function(s){"
"        var isEp=(s.type===2||s.type===1)&&s.season&&s.episode;"
"        var title=s.title||'(loading…)';"
"        var sub='';"
"        if(isEp){"
"          sub=(s.show||'')+' &middot; S'+String(s.season).padStart(2,'0')+'E'+String(s.episode).padStart(2,'0');"
"        }else if(s.type===1){sub='TV Show';}"
"        else if(s.type===0){sub='Movie';}"
"        var posterStyle=s.poster?'background-image:url(/image/'+encodeURIComponent(s.poster)+');':'';"
"        return '<div class=\"stream\">'+"
"          '<div class=\"poster\" style=\"'+posterStyle+'\"></div>'+"
"          '<div class=\"info\">'+"
"            '<div class=\"title\">'+esc(title)+'</div>'+"
"            '<div class=\"subtitle\">'+sub+'</div>'+"
"            '<div class=\"footer\">'+"
"              '<span class=\"live\">Now Watching</span>'+"
"              '<span class=\"sep\">·</span>'+"
"              '<span class=\"ip\">'+s.ip+'</span>'+"
"              '<span class=\"sep\">·</span>'+"
"              '<span>'+fmtTime(s.elapsed)+'</span>'+"
"              (s.connections>1?'<span class=\"sep\">·</span><span>'+s.connections+' conns</span>':'')+"
"            '</div>'+"
"          '</div>'+"
"          '<div class=\"actions\"><button class=\"btn-block\" onclick=\"blockIp(\\''+s.ip+'\\')\">Block</button></div>'+"
"        '</div>';"
"      }).join('');"
"    }"
"  }catch(e){}"
"  try{var r=await fetch('/api/blocked');var bd=await r.json();"
"    var c=document.getElementById('blocked');"
"    if(!bd.blocked||bd.blocked.length===0){"
"      c.innerHTML='<div class=\"empty\">None blocked</div>';"
"    }else{"
"      c.innerHTML=bd.blocked.map(function(ip){"
"        return '<div class=\"dl\"><div class=\"row1\">'+"
"          '<div class=\"name\" style=\"font-family:SF Mono,Consolas,monospace;color:var(--accent2)\">'+ip+'</div>'+"
"          '<span class=\"pill failed\">blocked</span>'+"
"          '<button class=\"btn-unblock\" onclick=\"unblockIp(\\''+ip+'\\')\">Unblock</button>'+"
"          '</div></div>';"
"      }).join('');"
"    }"
"  }catch(e){}"
"  try{var r=await fetch('/api/downloads/status');var d=await r.json();"
"    var c=document.getElementById('downloads');"
"    var active=(d.slots||[]).filter(function(s){return s.state==='active'||s.state==='queued';}).length;"
"    document.getElementById('s-dl').textContent=active;"
"    if(!d.slots||d.slots.length===0){c.innerHTML='<div class=\"empty\">No active downloads</div>';}"
"    else{c.innerHTML=d.slots.map(function(s){"
"      var pct=Math.max(0,Math.min(100,s.percent)).toFixed(1);"
"      return '<div class=\"dl\"><div class=\"row1\">'+"
"        '<div class=\"name\">'+s.filename+'</div>'+"
"        '<span class=\"pill '+s.type+'\">'+s.type+'</span>'+"
"        '<span class=\"pill '+s.state+'\">'+s.state+'</span></div>'+"
"        '<div class=\"bar\"><span style=\"width:'+pct+'%\"></span></div>'+"
"        '<div class=\"meta\"><span>'+fmtBytes(s.downloaded)+' / '+fmtBytes(s.total)+'</span>'+"
"        '<span>'+pct+'%</span><span>'+fmtSpeed(s.speed_bps)+'</span>'+"
"        (s.error?'<span style=\"color:var(--err)\">'+s.error+'</span>':'')+"
"        '</div></div>';"
"    }).join('');}"
"  }catch(e){}"
"  try{var rs=await fetch('/api/status');var ss=await rs.json();"
"    var gate=document.getElementById('s-gate');"
"    gate.textContent=ss.status==='open'?'Open':'Closed';"
"    gate.style.background=ss.status==='open'?"
"      'linear-gradient(135deg,#4ade80,#22d3ee)':'linear-gradient(135deg,#f87171,#fbbf24)';"
"    gate.style.webkitBackgroundClip='text';gate.style.backgroundClip='text';"
"    document.getElementById('s-gate-sub').innerHTML="
"      '<span class=\"pill '+(ss.can_start?'done':'failed')+'\">'+ss.active_ips+' / '+ss.max_ips+' IPs</span>';"
"  }catch(e){}"
"  try{var r2=await fetch('/api/scrape/status');var sd=await r2.json();"
"    document.getElementById('s-lib').textContent=sd.media_count;"
"    document.getElementById('s-pen').textContent=sd.tmdb_pending;"
"    var t=sd.tmdb;var sc=document.getElementById('scrape');"
"    if(t.active){"
"      sc.innerHTML='<div class=\"row1\"><div class=\"name\">TMDB Fetch</div>'+"
"        '<span class=\"pill active\">active</span></div>'+"
"        '<div class=\"bar\"><span style=\"width:'+t.percent.toFixed(1)+'%\"></span></div>'+"
"        '<div class=\"meta\"><span>'+t.processed+' / '+t.total+'</span>'+"
"        '<span>'+t.percent.toFixed(1)+'%</span>'+"
"        '<span style=\"opacity:.7\">'+t.current_item+'</span></div>';"
"    }else{"
"      sc.innerHTML='<div class=\"meta\"><span class=\"pill queued\">idle</span></div>';"
"    }"
"  }catch(e){}"
"}"
"refresh();setInterval(refresh,1500);"
"</script></body></html>";

const char *page_status_html =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Library — Nixly</title>"
NIXLY_STYLE
"</head><body>"
"<header><div class=\"hwrap\">"
"<div class=\"logo\"><span class=\"dot\"></span>Nixly</div>"
"<nav>"
"<a href=\"/\">Home</a>"
"<a href=\"/status\" class=\"active\">Library</a>"
"<a href=\"/wget\">Downloader</a>"
"</nav>"
"</div></header>"
"<main>"
"<div id=\"view\"><h1>Library</h1><p class=\"lead\">Loading…</p></div>"
"</main>"
"<script>"
"var view=document.getElementById('view');"
"var currentView=null;"  /* () => Promise, re-runs to refresh current page */
"var lastVersion=-1;"
"function esc(s){return (s||'').replace(/[&<>\"']/g,function(c){"
"  return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c];});}"
"async function showTopLevel(){"
"  currentView=showTopLevel;"
"  view.innerHTML='<h1>Library</h1><p class=\"lead\">Loading…</p>';"
"  var [tv,mv]=await Promise.all(["
"    fetch('/api/tvshows').then(r=>r.json()),"
"    fetch('/api/movies').then(r=>r.json())]);"
"  function posterStyle(p){"
"    return p?'background-image:url(/image/'+encodeURIComponent(p)+')':'';"
"  }"
"  var html='<h1>Library</h1>'+"
"    '<p class=\"lead\">Browse your scraped collection. Click a show to drill into seasons and episodes.</p>'+"
"    '<h2>TV Shows · '+(tv.length||0)+'</h2><div class=\"grid posters\">';"
"  tv.forEach(function(s){"
"    var name=s.tmdb_title||s.show_name||s.title||'?';"
"    var seasons=s.tmdb_total_seasons||(s.seasons?s.seasons.split(', ').length:0);"
"    var art=s.poster||s.backdrop||'';"
"    var encName=encodeURIComponent(name).replace(/'/g,\"\\\\'\");"
"    html+='<div class=\"card\" onclick=\"showShow(\\''+encName+'\\')\" style=\"cursor:pointer\">'+"
"      '<div class=\"poster-wrap\" style=\"'+posterStyle(art)+'\">'+"
"        (art?'':'<div class=\"poster-empty\">📺</div>')+'</div>'+"
"      '<div class=\"body\">'+"
"        '<div class=\"title\">'+esc(name)+'</div>'+"
"        '<div class=\"meta\"><span>'+seasons+' seasons</span><span class=\"sep\">·</span>'+"
"        '<span>'+(s.episode_count||0)+' eps</span></div>'+"
"      '</div></div>';"
"  });"
"  if(tv.length===0)html+='<div class=\"empty\">No TV shows scraped yet</div>';"
"  html+='</div><h2>Movies · '+(mv.length||0)+'</h2><div class=\"grid posters\">';"
"  mv.forEach(function(m){"
"    var t=m.tmdb_title||m.title||'?';"
"    var art=m.poster||m.backdrop||'';"
"    html+='<div class=\"card\">'+"
"      '<div class=\"poster-wrap\" style=\"'+posterStyle(art)+'\">'+"
"        (art?'':'<div class=\"poster-empty\">🎬</div>')+'</div>'+"
"      '<div class=\"body\">'+"
"        '<div class=\"title\">'+esc(t)+'</div>'+"
"        '<div class=\"meta\">'+(m.year?'<span>'+m.year+'</span>':'')+"
"        (m.year&&m.rating?'<span class=\"sep\">·</span>':'')+"
"        (m.rating?'<span>★ '+m.rating.toFixed(1)+'</span>':'')+'</div>'+"
"      '</div></div>';"
"  });"
"  if(mv.length===0)html+='<div class=\"empty\">No movies scraped yet</div>';"
"  html+='</div>';"
"  view.innerHTML=html;"
"}"
"async function showShow(encName){"
"  currentView=function(){return showShow(encName);};"
"  view.innerHTML='<p class=\"lead\">Loading…</p>';"
"  var name=decodeURIComponent(encName);"
"  var seasons=await fetch('/api/show/'+encName+'/seasons').then(r=>r.json());"
"  var html='<a class=\"back\" onclick=\"showTopLevel()\">← Back to library</a>'+"
"    '<h1>'+esc(name)+'</h1>'+"
"    '<p class=\"lead\">'+seasons.length+' season'+(seasons.length===1?'':'s')+'</p>'+"
"    '<div class=\"grid\">';"
"  seasons.forEach(function(s){"
"    html+='<div class=\"card padded\" onclick=\"showSeason(\\''+encName+'\\','+s.season+')\" style=\"cursor:pointer\">'+"
"      '<div class=\"title\">Season '+s.season+'</div>'+"
"      '<div class=\"meta\"><span>'+(s.episode_count||0)+' episodes</span></div></div>';"
"  });"
"  html+='</div>';"
"  view.innerHTML=html;"
"}"
"async function showSeason(encName,season){"
"  currentView=function(){return showSeason(encName,season);};"
"  view.innerHTML='<p class=\"lead\">Loading…</p>';"
"  var name=decodeURIComponent(encName);"
"  var eps=await fetch('/api/show/'+encName+'/episodes/'+season).then(r=>r.json());"
"  var html='<a class=\"back\" onclick=\"showShow(\\''+encName+'\\')\">← Back to '+esc(name)+'</a>'+"
"    '<h1>'+esc(name)+' · Season '+season+'</h1>'+"
"    '<p class=\"lead\">'+eps.length+' episode'+(eps.length===1?'':'s')+'</p>'+"
"    '<div class=\"eplist\">';"
"  eps.forEach(function(e){"
"    html+='<div class=\"ep\"><div class=\"num\">E'+(e.episode||'?')+'</div>'+"
"      '<div>'+esc(e.episode_title||e.title||'')+'</div></div>';"
"  });"
"  html+='</div>';"
"  view.innerHTML=html;"
"}"
"async function checkVersion(){"
"  try{var r=await fetch('/api/version');var d=await r.json();"
"    if(lastVersion!==-1&&d.version!==lastVersion&&currentView){currentView();}"
"    lastVersion=d.version;"
"  }catch(e){}"
"}"
"showTopLevel();"
"setInterval(checkVersion,1500);"
"</script></body></html>";

const char *page_wget_html =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Downloader — Nixly</title>"
NIXLY_STYLE
"</head><body>"
"<header><div class=\"hwrap\">"
"<div class=\"logo\"><span class=\"dot\"></span>Nixly</div>"
"<nav>"
"<a href=\"/\">Home</a>"
"<a href=\"/status\">Library</a>"
"<a href=\"/wget\" class=\"active\">Downloader</a>"
"</nav>"
"</div></header>"
"<main>"
"<h1>Downloader</h1>"
"<p class=\"lead\">Paste up to 10 direct URLs. Pick TV or Movie per row. Files land in their respective library folders.</p>"
"<form id=\"f\">"
"<div id=\"rows\"></div>"
"<button type=\"submit\">Start downloads</button>"
"<span id=\"result\"></span>"
"</form>"
"</main>"
"<script>"
"var rows=document.getElementById('rows');"
"var html='';"
"for(var i=0;i<10;i++){"
"  html+='<div class=\"row\"><label>'+(i+1)+'</label>'+"
"    '<input type=\"url\" name=\"url'+i+'\" placeholder=\"https://example.com/file.mkv\" autocomplete=\"off\">'+"
"    '<select name=\"type'+i+'\"><option value=\"movie\" selected>Movie</option>'+"
"    '<option value=\"tv\">TV</option></select></div>';"
"}"
"rows.innerHTML=html;"
"document.getElementById('f').addEventListener('submit',async function(e){"
"  e.preventDefault();"
"  var fd=new FormData(e.target);"
"  var body=new URLSearchParams(fd).toString();"
"  var r=await fetch('/api/downloads/add',{method:'POST',"
"    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});"
"  var d=await r.json();"
"  document.getElementById('result').textContent='Added '+d.added+' download'+(d.added===1?'':'s')+'.';"
"  setTimeout(function(){location.href='/';},700);"
"});"
"</script></body></html>";
