'use strict';

const $ = (id) => document.getElementById(id);
const form = $('config-form');
let pollTimer = null;
let pollInterval = 1000;

function setBadge(el, text, cls) {
  el.textContent = text;
  el.className = 'badge ' + (cls || '');
}

function formatUptime(seconds) {
  const s = Math.floor(seconds);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm ' + (s % 60) + 's';
}

async function api(path, options) {
  const res = await fetch(path, options);
  const type = res.headers.get('Content-Type') || '';
  const body = type.startsWith('application/json') ? await res.json() : await res.text();
  if (!res.ok) {
    const detail = body && body.detail ? body.detail : (body.error || res.statusText);
    throw new Error(res.status + ' ' + detail);
  }
  return body;
}

async function refreshStatus() {
  try {
    const s = await api('/api/status');
    const t = s.telemetry;
    $('s-hostname').textContent = s.hostname;
    $('s-uptime').textContent = formatUptime(t.uptime_s);
    $('s-temp').textContent = t.cpu_temp_c === null ? 'n/a' : t.cpu_temp_c.toFixed(1) + ' °C';
    $('s-load').textContent = t.load_1m.toFixed(2);
    $('s-mem').textContent = (t.mem_available_kb / 1024).toFixed(0) + ' / ' +
      (t.mem_total_kb / 1024).toFixed(0) + ' MiB';
    if (s.server) {
      $('s-req').textContent = s.server.requests_handled;
      $('s-conns').textContent = s.server.connections_active;
    }
    setBadge($('device-kind'), s.device);
    setBadge($('conn'), 'online', 'ok');
  } catch (err) {
    setBadge($('conn'), 'offline', 'bad');
  }
}

function schedulePoll() {
  clearInterval(pollTimer);
  pollTimer = setInterval(refreshStatus, pollInterval);
}

async function loadConfig() {
  const cfg = await api('/api/config');
  for (const [key, value] of Object.entries(cfg)) {
    if (form.elements[key]) form.elements[key].value = value;
  }
  pollInterval = Math.max(250, cfg.telemetry_interval_ms);
  schedulePoll();
}

async function refreshLog() {
  // /api/logs streams NDJSON with chunked transfer encoding.
  const res = await fetch('/api/logs?limit=200');
  const text = await res.text();
  const lines = text.trim().split('\n').filter(Boolean).map((line) => {
    const e = JSON.parse(line);
    return e.ts + '  ' + e.level.toUpperCase().padEnd(5) + '  ' + e.msg;
  });
  $('log').textContent = lines.reverse().join('\n');
}

function showMessage(text, ok) {
  const el = $('form-msg');
  el.textContent = text;
  el.className = ok ? 'ok' : 'bad';
}

form.addEventListener('submit', async (event) => {
  event.preventDefault();
  const data = Object.fromEntries(new FormData(form).entries());
  data.telemetry_interval_ms = Number(data.telemetry_interval_ms);
  try {
    await api('/api/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data),
    });
    showMessage('Saved', true);
    await loadConfig();
    await refreshLog();
  } catch (err) {
    showMessage(err.message, false);
  }
});

$('reload').addEventListener('click', () => loadConfig().then(() => showMessage('', true)));
$('refresh-log').addEventListener('click', refreshLog);
$('reboot').addEventListener('click', async () => {
  if (!confirm('Reboot the device?')) return;
  try {
    await api('/api/reboot', { method: 'POST' });
    await refreshLog();
  } catch (err) {
    showMessage(err.message, false);
  }
});

loadConfig().catch((err) => showMessage(err.message, false));
refreshStatus();
refreshLog();
