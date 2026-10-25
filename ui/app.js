// Mini Borg dashboard. Polls the Borgmaster once a second and re-renders.
'use strict';

const $ = (s, el = document) => el.querySelector(s);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const fmt = (v, d = 2) => (Math.round(v * 10 ** d) / 10 ** d).toFixed(d);
const hue = (jobId) => (jobId * 67 + 200) % 360;
const pct = (a, b) => (b > 0 ? Math.min(100, (100 * a) / b) : 0);
const TERMINAL = new Set(['finished', 'failed', 'oom', 'killed']);

let state = null;
let lastEvent = 0;
let selectedJob = null;
let hoverTask = null;
let firstEvents = true;

// ------------------------------------------------------------------ presets

const PRESETS = [
  { label: 'Web frontend', name: 'web-frontend', user: 'alice', type: 'service', priority: 200, tasks: 3, cpu: 0.25, ram_mb: 128, workload: 'service', distinct: true },
  { label: 'Batch analytics', name: 'analytics', user: 'bob', type: 'batch', priority: 100, tasks: 8, cpu: 0.3, ram_mb: 96, workload: 'cpu', duration: 120, threads: 1, distinct: false },
  { label: 'Free crawler', name: 'crawler', user: 'carol', type: 'batch', priority: 0, tasks: 6, cpu: 0.2, ram_mb: 64, workload: 'sleep', duration: 180, distinct: false },
  { label: 'Big prod job', name: 'payments', user: 'dave', type: 'service', priority: 250, tasks: 4, cpu: 0.6, ram_mb: 256, workload: 'service', distinct: true },
  { label: 'SSD database', name: 'db', user: 'erin', type: 'service', priority: 220, tasks: 2, cpu: 0.3, ram_mb: 200, workload: 'service', ckey: 'ssd', cval: 'true', distinct: true },
  { label: 'Memory hog (OOM)', name: 'mem-hog', user: 'frank', type: 'batch', priority: 100, tasks: 1, cpu: 0.2, ram_mb: 64, workload: 'mem', mb: 200, duration: 60, distinct: false },
  { label: 'Crashing service', name: 'flaky', user: 'grace', type: 'service', priority: 200, tasks: 1, cpu: 0.1, ram_mb: 48, workload: 'crash', duration: 8, distinct: false },
  { label: 'Monitoring agent', name: 'monitor', user: 'sre', type: 'service', priority: 300, tasks: 5, cpu: 0.05, ram_mb: 32, workload: 'service', distinct: true },
];

function applyPreset(p) {
  const f = $('#job-form');
  const set = (n, v) => { if (f.elements[n]) f.elements[n].value = v; };
  set('name', p.name); set('user', p.user); set('type', p.type); set('priority', p.priority);
  set('tasks', p.tasks); set('cpu', p.cpu); set('ram_mb', p.ram_mb); set('workload', p.workload);
  set('duration', p.duration ?? 60); set('threads', p.threads ?? 1); set('mb', p.mb ?? 150);
  set('ckey', p.ckey ?? ''); set('cval', p.cval ?? '');
  f.elements.distinct.checked = !!p.distinct;
  f.elements.chard.checked = true;
  syncWorkloadFields();
}

function syncWorkloadFields() {
  const w = $('#job-form').elements.workload.value;
  $('.w-threads').classList.toggle('hidden', w !== 'cpu');
  $('.w-mb').classList.toggle('hidden', w !== 'mem');
  $('.w-row').classList.toggle('hidden', w !== 'cpu' && w !== 'mem');
}

// ------------------------------------------------------------------ API

async function api(path, opts = {}) {
  const r = await fetch(path, { headers: { 'Content-Type': 'application/json' }, ...opts });
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(body.error || `HTTP ${r.status}`);
  return body;
}

async function submitJob(e) {
  e.preventDefault();
  const f = e.target.elements;
  const workload = { type: f.workload.value, duration: +f.duration.value };
  if (f.workload.value === 'cpu') workload.threads = +f.threads.value;
  if (f.workload.value === 'mem') workload.mb = +f.mb.value;
  const job = {
    name: f.name.value.trim(), user: f.user.value.trim() || 'user', type: f.type.value,
    priority: +f.priority.value, tasks: +f.tasks.value, cpu: +f.cpu.value, ram_mb: +f.ram_mb.value,
    workload, distinct: f.distinct.checked, constraints: [],
  };
  if (f.ckey.value.trim()) job.constraints.push({ key: f.ckey.value.trim(), value: f.cval.value.trim(), hard: f.chard.checked });
  const msg = $('#form-msg');
  try {
    const r = await api('/api/jobs', { method: 'POST', body: JSON.stringify(job) });
    msg.className = 'form-msg ok';
    msg.textContent = `Submitted ${job.name} (job ${r.id})`;
    selectedJob = r.id;
    poll();
  } catch (err) {
    msg.className = 'form-msg bad';
    msg.textContent = err.message;
  }
}

// ------------------------------------------------------------------ render: KPIs

function renderKpis(s) {
  const up = s.machines.filter((m) => m.status === 'up').length;
  const c = s.task_counts || {};
  const live = (c.running || 0) + (c.installing || 0) + (c.starting || 0);
  const sc = s.scheduler || {};
  const tiles = [
    { label: 'Machines up', value: `${up} / ${s.machines.length}`, meter: pct(up, s.machines.length) },
    { label: 'Running tasks', value: live, sub: `${c.installing || 0} installing · ${c.starting || 0} starting` },
    { label: 'Pending tasks', value: c.pending || 0, sub: `${c.finished || 0} finished · ${(c.failed || 0) + (c.oom || 0)} failed` },
    { label: 'CPU allocated', value: `${fmt(pct(s.allocated.cpu, s.capacity.cpu), 0)}%`, sub: `${fmt(s.allocated.cpu)} / ${fmt(s.capacity.cpu)} cores`, meter: pct(s.allocated.cpu, s.capacity.cpu) },
    { label: 'RAM allocated', value: `${fmt(pct(s.allocated.ram, s.capacity.ram), 0)}%`, sub: `${fmt(s.allocated.ram, 0)} / ${fmt(s.capacity.ram, 0)} MB`, meter: pct(s.allocated.ram, s.capacity.ram) },
    { label: 'Last scheduling pass', value: sc.elapsed_ms != null ? `${fmt(sc.elapsed_ms, 2)} ms` : '—',
      sub: sc.placed != null ? `${sc.placed} placed · ${sc.preempted} preempted · ${sc.feasibility_checks} checks` : 'no pending work yet' },
  ];
  $('#kpis').innerHTML = tiles.map((t) => `
    <div class="kpi"><div class="label">${t.label}</div><div class="value">${esc(t.value)}</div>
      ${t.sub ? `<div class="sub">${esc(t.sub)}</div>` : ''}
      ${t.meter != null ? `<div class="meter"><i style="width:${t.meter}%"></i></div>` : ''}</div>`).join('');
}

// ------------------------------------------------------------------ render: machines

function bar(label, alloc, prod, used, cap, unit, d) {
  const p = pct(prod, cap), np = Math.max(0, pct(alloc, cap) - p);
  return `<div>
    <div class="bar-label"><span>${label}</span><span>${fmt(alloc, d)} / ${fmt(cap, d)} ${unit} · used ${fmt(used, d)}</span></div>
    <div class="bar"><div class="seg prod" style="left:0;width:${p}%"></div>
      <div class="seg nonprod" style="left:${p}%;width:${np}%"></div>
      <div class="use" style="left:calc(${pct(used, cap)}% - 1px)"></div></div></div>`;
}

function renderMachines(s, tasksById) {
  const el = $('#machines');
  if (!s.machines.length) {
    el.innerHTML = '<div class="empty">No Borglets registered yet. Start them with <code>docker compose up</code>.</div>';
    return;
  }
  el.innerHTML = [...s.machines].sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true })).map((m) => {
    const attrs = Object.entries(m.attributes).map(([k, v]) => `<span class="attr">${esc(k)}=${esc(v)}</span>`).join('');
    const iso = m.cgroups ? '<span class="attr iso">cgroup v2</span>' : '<span class="attr noiso">no isolation</span>';
    const chips = m.tasks.map((id) => tasksById.get(id)).filter(Boolean)
      .sort((a, b) => b.priority - a.priority || a.job - b.job || a.index - b.index)
      .map((t) => {
        const cls = [t.state, selectedJob != null ? (t.job === selectedJob ? 'hl' : 'dim') : ''].join(' ');
        return `<span class="chip ${cls}" style="--h:${hue(t.job)}" data-task="${t.id}">${esc(t.job_name)}/${t.index}</span>`;
      }).join('');
    return `<div class="machine ${m.status}">
      <div class="m-head"><span class="dot"></span><span class="m-name">${esc(m.name)}</span>
        <span class="muted small">${m.status === 'down' ? 'DOWN' : ''}</span>
        <button class="m-fail" data-fail="${m.id}" ${m.status === 'down' ? 'disabled' : ''} title="Kill all tasks and stop answering polls for 20 s">Fail</button></div>
      <div class="attrs">${iso}${attrs}</div>
      <div class="bars">
        ${bar('CPU', m.allocated.cpu, m.allocated_prod.cpu, m.usage.cpu, m.capacity.cpu, 'cores', 2)}
        ${bar('RAM', m.allocated.ram, m.allocated_prod.ram, m.usage.ram, m.capacity.ram, 'MB', 0)}
      </div>
      <div class="chips">${chips || '<span class="muted small">idle</span>'}</div>
      <div class="pk" title="${esc(m.packages.join(', '))}">packages: ${m.packages.length ? esc(m.packages.join(', ')) : '—'}</div>
    </div>`;
  }).join('');
}

// ------------------------------------------------------------------ render: jobs

const STATE_COLORS = { running: 'var(--ok)', installing: 'var(--cyan)', starting: 'var(--cyan)', pending: 'var(--muted)', finished: '#4b5263', failed: 'var(--bad)', oom: 'var(--bad)', killed: '#3a3f4b' };

function renderJobs(s, tasksById) {
  const jobs = [...s.jobs].sort((a, b) => b.id - a.id);
  if (!jobs.length) {
    $('#jobs').innerHTML = '<tr><td class="empty">No jobs yet. Pick a preset on the left and submit it.</td></tr>';
    $('#job-detail').innerHTML = '';
    return;
  }
  const head = '<tr><th>Job</th><th>Priority</th><th>Type</th><th>Tasks</th><th>Per task</th><th>Workload</th><th></th></tr>';
  const rows = jobs.map((j) => {
    const total = j.tasks;
    const order = ['running', 'installing', 'starting', 'pending', 'finished', 'failed', 'oom', 'killed'];
    const mini = order.filter((k) => j.counts[k]).map((k) => `<i style="width:${pct(j.counts[k], total)}%;background:${STATE_COLORS[k]}"></i>`).join('');
    const txt = order.filter((k) => j.counts[k]).map((k) => `${j.counts[k]} ${k}`).join(' · ');
    const w = j.workload.type + (j.workload.type !== 'service' && j.workload.duration ? ` ${j.workload.duration}s` : '') + (j.workload.mb ? ` ${j.workload.mb}MB` : '');
    const cons = j.constraints.map((c) => `${c.key}=${c.value}${c.hard ? '' : '?'}`).join(' ');
    return `<tr class="job ${j.id === selectedJob ? 'sel' : ''}" data-job="${j.id}">
      <td><div class="jname" style="--h:${hue(j.id)}"><i></i>${esc(j.name)}</div><div class="counts">${esc(j.user)}${cons ? ' · ' + esc(cons) : ''}${j.distinct ? ' · spread' : ''}</div></td>
      <td><span class="badge b-${j.band}">${j.priority} ${j.band}</span></td>
      <td>${esc(j.type)}</td>
      <td><div class="mini">${mini}</div><div class="counts">${esc(txt)}</div></td>
      <td class="mono">${fmt(j.cpu)} cpu · ${j.ram_mb} MB</td>
      <td class="mono">${esc(w)}</td>
      <td><button class="kill" data-kill="${j.id}">${j.done ? 'Remove' : 'Kill'}</button></td></tr>`;
  }).join('');
  $('#jobs').innerHTML = head + rows;

  const sel = jobs.find((j) => j.id === selectedJob);
  if (!sel) { $('#job-detail').innerHTML = ''; return; }
  const ts = s.tasks.filter((t) => t.job === sel.id).sort((a, b) => a.index - b.index);
  $('#job-detail').innerHTML = `<div class="detail"><h3>${esc(sel.name)} · tasks</h3><table>
    <tr><th>#</th><th>State</th><th>Machine</th><th>Usage</th><th>Restarts</th><th>Preempted</th><th>Reason</th></tr>
    ${ts.map((t) => `<tr><td>${t.index}</td><td class="st-${t.state}">${t.state}</td><td>${TERMINAL.has(t.state) || t.state === 'pending' ? (t.machine_name !== '-' ? `<span class="muted">${esc(t.machine_name)}</span>` : '—') : esc(t.machine_name)}</td>
      <td class="mono">${t.state === 'running' ? `${fmt(t.cpu_usage)} cpu · ${fmt(t.mem_mb, 0)} MB` : '—'}</td>
      <td>${t.restarts}</td><td>${t.preemptions}</td><td class="reason">${esc(t.reason)}</td></tr>`).join('')}
  </table></div>`;
}

function renderPending(s) {
  const p = s.tasks.filter((t) => t.state === 'pending').sort((a, b) => b.priority - a.priority || a.id - b.id);
  $('#pending').innerHTML = p.length
    ? p.map((t) => `<div class="prow"><span class="badge b-${t.band}">${t.priority}</span><span class="mono">${esc(t.job_name)}/${t.index}</span><span class="reason" title="${esc(t.reason)}">${esc(t.reason)}</span></div>`).join('')
    : '<div class="empty">Nothing pending: every task has a machine.</div>';
}

// ------------------------------------------------------------------ events

function renderEvents(events) {
  const el = $('#events');
  const showSched = $('#ev-sched').checked;
  const html = events.filter((e) => showSched || e.kind !== 'schedule').map((e) => {
    const t = new Date(e.time * 1000).toLocaleTimeString([], { hour12: false });
    return `<div class="ev k-${e.kind} ${firstEvents ? '' : 'new'}"><span class="t">${t}</span><span>${esc(e.msg)}</span></div>`;
  }).reverse().join('');
  if (html) el.insertAdjacentHTML('afterbegin', html);
  while (el.children.length > 400) el.lastChild.remove();
  firstEvents = false;
}

// ------------------------------------------------------------------ tooltip

function tipFor(id) {
  const t = state && state.tasks.find((x) => x.id === id);
  if (!t) return null;
  return `<b>${esc(t.job_name)}/${t.index}</b> <span class="badge b-${t.band}">${t.priority} ${t.band}</span>
    <div class="kv"><span>state</span><span class="st-${t.state}">${t.state}</span>
    <span>machine</span><span>${esc(t.machine_name)}</span>
    <span>limit</span><span>${fmt(t.cpu)} cpu · ${t.ram_mb} MB</span>
    <span>usage</span><span>${fmt(t.cpu_usage)} cpu · ${fmt(t.mem_mb, 0)} MB</span>
    <span>restarts</span><span>${t.restarts}</span>
    <span>preempted</span><span>${t.preemptions}×</span>
    <span>note</span><span>${esc(t.reason)}</span></div>`;
}

function moveTip(e) {
  const chip = e.target.closest && e.target.closest('[data-task]');
  const tip = $('#tip');
  if (!chip) { hoverTask = null; tip.classList.add('hidden'); return; }
  hoverTask = +chip.dataset.task;
  const html = tipFor(hoverTask);
  if (!html) return;
  tip.innerHTML = html;
  tip.classList.remove('hidden');
  const x = Math.min(e.clientX + 14, window.innerWidth - tip.offsetWidth - 8);
  const y = Math.min(e.clientY + 14, window.innerHeight - tip.offsetHeight - 8);
  tip.style.left = `${x}px`;
  tip.style.top = `${y}px`;
}

// ------------------------------------------------------------------ polling

async function poll() {
  try {
    const [s, ev] = await Promise.all([api('/api/state'), api(`/api/events?since=${lastEvent}`)]);
    state = s;
    lastEvent = ev.last;
    $('#conn').textContent = 'connected';
    $('#conn').className = 'conn ok';
    const sel = $('#policy');
    if (document.activeElement !== sel) sel.value = s.policy;
    const tasksById = new Map(s.tasks.map((t) => [t.id, t]));
    renderKpis(s);
    renderMachines(s, tasksById);
    renderJobs(s, tasksById);
    renderPending(s);
    renderEvents(ev.events);
    if (hoverTask != null) {
      const html = tipFor(hoverTask);
      if (html) $('#tip').innerHTML = html;
    }
  } catch (err) {
    $('#conn').textContent = 'master unreachable';
    $('#conn').className = 'conn bad';
  }
}

// ------------------------------------------------------------------ results tab (Plan A)

function parseCsv(text) {
  const lines = text.trim().split(/\r?\n/);
  const head = lines.shift().split(',');
  return lines.map((l) => {
    const v = l.split(',');
    return Object.fromEntries(head.map((h, i) => [h, isNaN(+v[i]) || v[i] === '' ? v[i] : +v[i]]));
  });
}

async function loadCsv(name) {
  try {
    const r = await fetch(`/results/${name}.csv`, { cache: 'no-store' });
    if (!r.ok) return null;
    return parseCsv(await r.text());
  } catch { return null; }
}

function hbars(rows, color) {
  const max = Math.max(...rows.map((r) => r.value), 1);
  return rows.map((r) => `<div class="hbar"><span>${esc(r.label)}</span>
    <div class="track"><div class="fill" style="width:${pct(r.value, max)}%;background:${r.color || color || 'var(--accent)'}"></div></div>
    <span class="num">${esc(r.text ?? r.value)}</span></div>`).join('');
}

function table(rows, cols) {
  return `<table class="res-table"><tr>${cols.map((c) => `<th>${esc(c[1])}</th>`).join('')}</tr>
    ${rows.map((r) => `<tr>${cols.map((c) => `<td>${esc(typeof r[c[0]] === 'number' && !Number.isInteger(r[c[0]]) ? fmt(r[c[0]], 2) : r[c[0]])}</td>`).join('')}</tr>`).join('')}</table>`;
}

const change = (a, b) => `${a >= b ? '+' : ''}${fmt((100 * (a - b)) / b, 1)}%`;

async function renderResults() {
  const [wl, pol, seg, bkt, scal, rec, alpha] = await Promise.all(['workload', 'policies', 'segregation', 'buckets', 'scalability', 'reclamation', 'hybrid_alpha'].map(loadCsv));
  const el = $('#results');
  if (!wl && !pol && !seg && !bkt && !scal && !rec && !alpha) {
    el.innerHTML = `<div class="card res-empty"><h2>No simulation results yet</h2>
      <p>Run Plan A, then reload this tab:</p>
      <p><code>make sim && ./build/bin/borgsim --workload trace all</code></p>
      <p class="muted">The master serves CSV files from <code>results/</code>.</p></div>`;
    return;
  }
  const cards = [];
  if (wl) {
    const w = wl[0];
    cards.push(`<div class="card res-card"><h3>Workload</h3><div class="ref">Snapshot packed from scratch for every experiment (§5)</div>
      ${table([{ k: 'source', v: w.source }, { k: 'machines', v: w.machines }, { k: 'tasks', v: `${w.tasks} (${w.prod_tasks} prod)` },
        { k: 'CPU requested / capacity', v: `${fmt((100 * w.cpu_requested) / w.cpu_capacity, 0)}%` },
        { k: 'RAM requested / capacity', v: `${fmt((100 * w.ram_requested) / w.ram_capacity, 0)}%` },
        { k: 'seeds per configuration', v: w.seeds }, { k: 'policy (non-policy experiments)', v: w.policy }], [['k', ''], ['v', '']])}</div>`);
  }
  if (pol) {
    const best = pol.find((r) => r.policy === 'best_fit');
    const worst = pol.find((r) => r.policy === 'worst_fit');
    const hy = pol.find((r) => r.policy === 'hybrid');
    cards.push(`<div class="card res-card"><h3>Scoring policies</h3><div class="ref">§3.2 · machines needed after compaction (p90 of seeds), lower is better</div>
      ${hbars(pol.map((r) => ({ label: r.policy, value: r.p90_machines })))}
      ${table(pol, [['policy', 'policy'], ['p90_machines', 'p90'], ['min_machines', 'min'], ['max_machines', 'max'], ['stranded_cpu_pct', 'stranded CPU %'], ['stranded_ram_pct', 'stranded RAM %']])}
      ${best && hy ? `<p class="finding">Hybrid vs best fit: <b>${change(hy.p90_machines, best.p90_machines)}</b> machines${worst ? `, vs worst fit: <b>${change(hy.p90_machines, worst.p90_machines)}</b>` : ''}. Paper: hybrid packs 3–5% better than best fit.</p>` : ''}</div>`);
  }
  if (seg) {
    const get = (cfg, rec) => (seg.find((r) => r.config === cfg && (+r.reclamation || 0) === rec) || {}).p90_machines;
    const variants = [1, 0].filter((rec) => get('shared', rec) != null);
    const rows = variants.flatMap((rec) => {
      const tag = rec ? 'with reclamation' : 'limits only';
      return [{ label: `shared · ${tag}`, value: get('shared', rec), color: 'var(--ok)' },
        { label: `segregated · ${tag}`, value: get('segregated_total', rec), color: 'var(--bad)',
          text: `${get('segregated_total', rec)} (${get('prod_only', rec)}+${get('nonprod_only', rec)})` }];
    });
    const finding = variants.map((rec) => `${rec ? 'with' : 'without'} reclamation <b>${change(get('segregated_total', rec), get('shared', rec))}</b>`).join(', ');
    cards.push(`<div class="card res-card"><h3>Segregating prod and non-prod</h3><div class="ref">Fig. 5 · one shared cell vs separate prod and non-prod cells</div>
      ${hbars(rows)}
      <p class="finding">Segregation needs more machines: ${finding}. Paper: 20–30% more in the median cell, mostly because non-prod work in a shared cell runs in resources that prod reserves but does not use. Our reservations come from an assumed usage model, so the size of the gap depends on it.</p></div>`);
  }
  if (bkt) {
    const exact = bkt.find((r) => r.variant === 'exact');
    cards.push(`<div class="card res-card"><h3>Bucketed resource requests</h3><div class="ref">Fig. 8 · rounding requests up to powers of two</div>
      ${hbars(bkt.map((r) => ({ label: r.variant === 'exact' ? 'exact requests' : `buckets ≥ 1/${Math.round(1 / r.min_bucket)}`, value: r.p90_machines, text: r.variant === 'exact' ? r.p90_machines : `${r.p90_machines} (${change(r.p90_machines, exact.p90_machines)})` })))}
      <p class="finding">Fine-grained requests save machines. Paper: bucketing would need 30–50% more resources in the median case.</p></div>`);
  }
  if (scal) {
    cards.push(`<div class="card res-card"><h3>Scheduler scalability</h3><div class="ref">§3.4 · one full scheduling pass with each optimization</div>
      ${hbars(scal.map((r) => ({ label: r.variant.replace(/_/g, ' '), value: r.elapsed_ms, text: `${fmt(r.elapsed_ms, 0)} ms` })), 'var(--purple)')}
      ${table(scal, [['variant', 'variant'], ['feasibility_checks', 'feasibility checks'], ['score_computations', 'scores'], ['cache_hits', 'cache hits'], ['pending', 'pending']])}
      <p class="finding">Equivalence classes and relaxed randomization cut the work by orders of magnitude (paper: a pass went from over 3 days to a few hundred seconds). Our score is only a few arithmetic operations, so the score cache's hash lookups cost more than they save here; Borg's scoring is far more expensive.</p></div>`);
  }
  if (rec) {
    const g = Object.fromEntries(rec.map((r) => [r.variant, r.p90_machines]));
    cards.push(`<div class="card res-card"><h3>Resource reclamation</h3><div class="ref">Fig. 10 · non-prod tasks scheduled against reservations</div>
      ${hbars([{ label: 'with reclamation', value: g.with_reclamation, color: 'var(--ok)' }, { label: 'without reclamation', value: g.without_reclamation, color: 'var(--bad)' }])}
      <p class="finding">Without reclamation the cell needs <b>${change(g.without_reclamation, g.with_reclamation)}</b> machines. Paper: ~20% of the workload runs in reclaimed resources. Reservations here come from an assumed usage model (no usage data loaded), so the magnitude depends on it.</p></div>`);
  }
  if (alpha) {
    const a0 = alpha.find((r) => r.alpha === 0);
    const a10 = alpha.find((r) => r.alpha === 10);
    cards.push(`<div class="card res-card"><h3>Hybrid stranding weight α</h3><div class="ref">Ablation · score = −(free_cpu + free_ram) − α·|free_cpu − free_ram|, α = 0 is best fit</div>
      ${hbars(alpha.map((r) => ({ label: `α = ${r.alpha}${r.alpha === 10 ? ' (default)' : ''}`, value: r.p90_machines, color: r.alpha === 10 ? 'var(--ok)' : 'var(--accent)' })))}
      ${a0 && a10 ? `<p class="finding">At α = 10 the hybrid needs <b>${change(a10.p90_machines, a0.p90_machines)}</b> machines compared with best fit (α = 0); the gains level off there. The paper does not publish its hybrid formula; this one is ours.</p>` : ''}</div>`);
  }
  el.innerHTML = cards.join('');
}

// ------------------------------------------------------------------ wiring

function init() {
  $('#presets').innerHTML = PRESETS.map((p, i) => `<button type="button" data-preset="${i}">${esc(p.label)}</button>`).join('');
  $('#presets').addEventListener('click', (e) => { const b = e.target.closest('[data-preset]'); if (b) applyPreset(PRESETS[+b.dataset.preset]); });
  $('#band-pick').addEventListener('click', (e) => { const b = e.target.closest('[data-p]'); if (b) $('#job-form').elements.priority.value = b.dataset.p; });
  $('#job-form').addEventListener('submit', submitJob);
  $('#job-form').elements.workload.addEventListener('change', syncWorkloadFields);
  applyPreset(PRESETS[0]);

  $('#policy').addEventListener('change', async (e) => {
    try { await api('/api/config', { method: 'POST', body: JSON.stringify({ policy: e.target.value }) }); poll(); } catch (err) { alert(err.message); }
  });
  $('#machines').addEventListener('click', async (e) => {
    const b = e.target.closest('[data-fail]');
    if (b) { b.disabled = true; await api(`/api/machines/${b.dataset.fail}/fail`, { method: 'POST' }).catch(() => {}); poll(); }
  });
  $('#jobs').addEventListener('click', async (e) => {
    const k = e.target.closest('[data-kill]');
    if (k) {
      e.stopPropagation();
      await api(`/api/jobs/${k.dataset.kill}`, { method: 'DELETE' }).catch(() => {});
      poll();
      return;
    }
    const row = e.target.closest('[data-job]');
    if (row) { const id = +row.dataset.job; selectedJob = selectedJob === id ? null : id; poll(); }
  });
  document.addEventListener('mousemove', moveTip);

  const showTab = (name) => {
    document.querySelectorAll('.tab').forEach((x) => x.classList.toggle('active', x.dataset.tab === name));
    $('#tab-cell').classList.toggle('hidden', name !== 'cell');
    $('#tab-results').classList.toggle('hidden', name !== 'results');
    if (name === 'results') renderResults();
  };
  document.querySelectorAll('.tab').forEach((t) => t.addEventListener('click', () => {
    history.replaceState(null, '', t.dataset.tab === 'results' ? '#results' : '#');
    showTab(t.dataset.tab);
  }));
  showTab(location.hash === '#results' ? 'results' : 'cell');

  poll();
  setInterval(poll, 1000);
}

init();
