#pragma once

namespace p2p {

// The dashboard, embedded so the client is a single self-contained
// binary with no asset directory to install or keep in step.
inline constexpr const char *kDashboardHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>P2P Client</title>
<style>
  :root {
    --bg:#0f1115; --panel:#171a21; --line:#252a34; --fg:#e6e9ef;
    --muted:#8b93a7; --accent:#4f8cff; --ok:#3fb950; --warn:#d29922; --err:#f85149;
    --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
  @media (prefers-color-scheme: light) {
    :root { --bg:#f6f7f9; --panel:#fff; --line:#e3e6ec; --fg:#1b1f27;
            --muted:#606a7e; }
  }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font:14px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif; }
  header { display:flex; align-items:center; gap:12px; padding:14px 20px;
           border-bottom:1px solid var(--line); background:var(--panel);
           position:sticky; top:0; z-index:5; flex-wrap:wrap; }
  h1 { font-size:15px; margin:0; font-weight:650; letter-spacing:.2px; }
  .dot { width:8px; height:8px; border-radius:50%; background:var(--err); }
  .dot.on { background:var(--ok); }
  .who { color:var(--muted); font-size:13px; }
  .spacer { flex:1; }
  main { padding:20px; display:grid; gap:16px;
         grid-template-columns:repeat(auto-fit,minmax(330px,1fr)); max-width:1400px; }
  .card { background:var(--panel); border:1px solid var(--line);
          border-radius:10px; padding:14px 16px; }
  .card h2 { font-size:12px; text-transform:uppercase; letter-spacing:.6px;
             color:var(--muted); margin:0 0 10px; font-weight:600; }
  .wide { grid-column:1/-1; }
  input, select, button {
    font:inherit; padding:7px 10px; border-radius:7px;
    border:1px solid var(--line); background:var(--bg); color:var(--fg); }
  button { background:var(--accent); border-color:var(--accent); color:#fff;
           cursor:pointer; font-weight:550; }
  button:hover { filter:brightness(1.08); }
  button.ghost { background:transparent; color:var(--fg); border-color:var(--line); }
  .row { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
  .row > input { flex:1; min-width:110px; }
  ul { list-style:none; margin:0; padding:0; }
  li { padding:7px 0; border-bottom:1px solid var(--line); font-family:var(--mono);
       font-size:13px; display:flex; justify-content:space-between; gap:10px; }
  li:last-child { border-bottom:0; }
  .empty { color:var(--muted); font-style:italic; font-family:inherit; }
  .bar { height:6px; background:var(--line); border-radius:99px; overflow:hidden; margin-top:6px; }
  .bar > i { display:block; height:100%; background:var(--accent); transition:width .4s; }
  .bar.done > i { background:var(--ok); }
  .dl { padding:10px 0; border-bottom:1px solid var(--line); }
  .dl:last-child { border-bottom:0; }
  .dl .top { display:flex; justify-content:space-between; gap:10px; font-family:var(--mono); font-size:13px; }
  .dl .sub { color:var(--muted); font-size:12px; margin-top:3px; }
  .tag { font-size:11px; padding:1px 7px; border-radius:99px; border:1px solid var(--line); color:var(--muted); }
  .tag.active { color:var(--accent); border-color:var(--accent); }
  .tag.done { color:var(--ok); border-color:var(--ok); }
  pre { background:var(--bg); border:1px solid var(--line); border-radius:8px;
        padding:10px 12px; font-family:var(--mono); font-size:12.5px;
        white-space:pre-wrap; word-break:break-word; max-height:260px;
        overflow:auto; margin:10px 0 0; }
  .hint { color:var(--muted); font-size:12px; margin-top:8px; }
</style>
</head>
<body>
<header>
  <span class="dot" id="dot"></span>
  <h1>P2P Client</h1>
  <span class="who" id="who">not logged in</span>
  <span class="spacer"></span>
  <span class="who" id="peer"></span>
</header>

<main>
  <section class="card" id="authCard">
    <h2>Session</h2>
    <div class="row">
      <input id="u" placeholder="username" autocomplete="off">
      <input id="p" placeholder="password" type="password" autocomplete="off">
    </div>
    <div class="row" style="margin-top:8px">
      <button onclick="run(['login',v('u'),v('p')])">Log in</button>
      <button class="ghost" onclick="run(['create_user',v('u'),v('p')])">Register</button>
      <button class="ghost" onclick="run(['logout'])">Log out</button>
    </div>
  </section>

  <section class="card">
    <h2>Groups</h2>
    <ul id="groups"><li class="empty">—</li></ul>
    <div class="row" style="margin-top:10px">
      <input id="g" placeholder="group id" autocomplete="off">
      <button onclick="run(['create_group',v('g')])">Create</button>
      <button class="ghost" onclick="run(['join_group',v('g')])">Join</button>
    </div>
    <div class="row" style="margin-top:8px">
      <button class="ghost" onclick="run(['list_files',v('g')])">List files</button>
      <button class="ghost" onclick="run(['list_requests',v('g')])">Requests</button>
      <button class="ghost" onclick="run(['leave_group',v('g')])">Leave</button>
    </div>
    <div class="row" style="margin-top:8px">
      <input id="who2" placeholder="username" autocomplete="off">
      <button class="ghost" onclick="run(['accept_request',v('g'),v('who2')])">Accept</button>
      <button class="ghost" onclick="run(['reject_request',v('g'),v('who2')])">Reject</button>
    </div>
  </section>

  <section class="card">
    <h2>Share &amp; fetch</h2>
    <div class="row">
      <input id="up" placeholder="/absolute/path/to/file" autocomplete="off">
      <button onclick="run(['upload_file',v('g'),v('up')])">Share</button>
    </div>
    <div class="row" style="margin-top:8px">
      <input id="fn" placeholder="filename" autocomplete="off">
      <input id="dest" placeholder="/destination/dir/" autocomplete="off">
    </div>
    <div class="row" style="margin-top:8px">
      <button onclick="run(['download_file',v('g'),v('fn'),v('dest')])">Download</button>
      <button class="ghost" onclick="run(['stop_share',v('g'),v('fn')])">Stop sharing</button>
    </div>
    <div class="hint">Uses the group id from the Groups panel.</div>
  </section>

  <section class="card wide">
    <h2>Transfers</h2>
    <div id="dls"><div class="empty">No downloads yet.</div></div>
  </section>

  <section class="card wide">
    <h2>Output</h2>
    <div class="row">
      <input id="cmd" placeholder="type any command, e.g. list_groups" autocomplete="off"
             onkeydown="if(event.key==='Enter')runRaw()">
      <button onclick="runRaw()">Run</button>
      <button class="ghost" onclick="document.getElementById('out').textContent=''">Clear</button>
    </div>
    <pre id="out"></pre>
  </section>
</main>

<script>
const v = id => document.getElementById(id).value.trim();
const out = () => document.getElementById('out');

function log(text) {
  const p = out();
  p.textContent += (p.textContent ? '\n' : '') + text.replace(/\s+$/,'');
  p.scrollTop = p.scrollHeight;
}

async function run(args) {
  args = args.filter(a => a !== '');
  if (!args.length) return;
  log('>>> ' + args.join(' '));
  try {
    const r = await fetch('/api/command', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({args})
    });
    const j = await r.json();
    if (j.output) log(j.output);
  } catch (e) { log('request failed: ' + e); }
  refresh();
}

function runRaw() {
  const el = document.getElementById('cmd');
  const parts = el.value.trim().split(/\s+/);
  el.value = '';
  run(parts);
}

function bytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n/1024).toFixed(1) + ' KB';
  if (n < 1073741824) return (n/1048576).toFixed(1) + ' MB';
  return (n/1073741824).toFixed(2) + ' GB';
}

function renderDownloads(list) {
  const box = document.getElementById('dls');
  if (!list.length) { box.innerHTML = '<div class="empty">No downloads yet.</div>'; return; }
  box.innerHTML = list.map(d => {
    const pct = d.total_pieces ? Math.round(100*d.completed_pieces/d.total_pieces) : 0;
    const done = d.total_pieces > 0 && d.completed_pieces === d.total_pieces;
    const tag = done ? '<span class="tag done">complete</span>'
              : d.active ? '<span class="tag active">downloading</span>'
              : '<span class="tag">stopped</span>';
    const detail = d.total_pieces
      ? `${d.completed_pieces}/${d.total_pieces} pieces · ${bytes(d.total_size)}`
      : 'fetching metadata…';
    return `<div class="dl">
      <div class="top"><span>${d.filename} <span class="tag">${d.group}</span></span>${tag}</div>
      <div class="sub">${detail}${d.failed ? ' · <span style="color:var(--err)">'+d.failed+' failed</span>' : ''}</div>
      <div class="bar ${done?'done':''}"><i style="width:${pct}%"></i></div>
    </div>`;
  }).join('');
}

async function refresh() {
  try {
    const s = await (await fetch('/api/status')).json();
    document.getElementById('dot').className = 'dot' + (s.logged_in ? ' on' : '');
    document.getElementById('who').textContent =
      s.logged_in ? 'signed in as ' + s.username : 'not logged in';
    document.getElementById('peer').textContent = 'peer port ' + s.peer_port;
    const gl = document.getElementById('groups');
    gl.innerHTML = s.groups.length
      ? s.groups.map(g => `<li><span>${g}</span></li>`).join('')
      : '<li class="empty">' + (s.logged_in ? 'none yet' : 'log in to see groups') + '</li>';
    renderDownloads(s.downloads);
  } catch (e) { document.getElementById('dot').className = 'dot'; }
}

refresh();
setInterval(refresh, 1500);
</script>
</body>
</html>
)HTML";

} // namespace p2p
