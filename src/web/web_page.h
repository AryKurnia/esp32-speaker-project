#pragma once

#include <Arduino.h>

// Halaman harus berdiri sendiri: AP speaker tidak punya akses internet.
static const char kWebPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Oke Gass</title>
<style>
:root {
  color-scheme: light dark;
  --bg: #f4f2ee; --panel: #fff; --text: #1d1c1a; --muted: #6d6a64;
  --line: #e2ded6; --accent: #c2410c; --on-accent: #fff;
  --warn-bg: #fef3c7; --warn-text: #78350f;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #161514; --panel: #211f1d; --text: #f1eee8; --muted: #a39e95;
    --line: #36322e; --accent: #fb923c; --on-accent: #1d1c1a;
    --warn-bg: #422006; --warn-text: #fde68a;
  }
}
* { box-sizing: border-box; }
/* Atribut hidden harus menang atas display: flex/grid di bawah. */
[hidden] { display: none !important; }
body {
  margin: 0 auto; max-width: 30rem; padding: 1.25rem 1rem 7rem;
  background: var(--bg); color: var(--text);
  font: 16px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
h1 { font-size: 1.4rem; margin: 0; }
h2 { font-size: .8rem; letter-spacing: .06em; text-transform: uppercase;
     color: var(--muted); margin: 0 0 .75rem; }
header { display: flex; justify-content: space-between; align-items: baseline;
         margin-bottom: 1rem; }
#mode { color: var(--muted); font-size: .85rem; }
section { background: var(--panel); border: 1px solid var(--line);
          border-radius: .75rem; padding: 1rem; margin-bottom: .75rem; }
.presets { display: grid; grid-template-columns: repeat(3, 1fr); gap: .5rem; }
button { font: inherit; font-weight: 600; min-height: 2.75rem; cursor: pointer;
         color: var(--text); background: transparent;
         border: 1px solid var(--line); border-radius: .5rem; }
button:active { background: var(--line); }
.primary { background: var(--accent); color: var(--on-accent); border-color: var(--accent); }
.row { margin-bottom: .9rem; }
.row:last-child { margin-bottom: 0; }
.head { display: flex; justify-content: space-between; font-size: .95rem; }
.value { font-variant-numeric: tabular-nums; font-weight: 600; }
input[type=range] { width: 100%; height: 2rem; margin: 0; accent-color: var(--accent); }
.switch { display: flex; justify-content: space-between; align-items: center; gap: 1rem; }
.switch input { width: 1.4rem; height: 1.4rem; accent-color: var(--accent); }
.hint { color: var(--muted); font-size: .85rem; margin: .5rem 0 0; }
footer { position: fixed; left: 0; right: 0; bottom: 0; background: var(--panel);
         border-top: 1px solid var(--line);
         padding: .6rem 1rem calc(.6rem + env(safe-area-inset-bottom, 0px)); }
.actions { max-width: 28rem; margin: 0 auto; display: grid;
           grid-template-columns: 2fr 1fr; gap: .5rem; }
#message { max-width: 28rem; margin: 0 auto .4rem; font-size: .85rem;
           color: var(--muted); text-align: center; min-height: 1.2em; }
#exit { position: fixed; inset: 0; background: rgb(0 0 0 / .55);
        display: flex; align-items: center; justify-content: center; padding: 1rem; }
.dialog { background: var(--panel); border-radius: .75rem; padding: 1.25rem;
          width: 100%; max-width: 24rem; text-align: center; }
.dialog p { margin: 0 0 .75rem; }
#seconds { font-size: 3.5rem; font-weight: 700; line-height: 1;
           font-variant-numeric: tabular-nums; margin: .25rem 0 .75rem; }
.warning { background: var(--warn-bg); color: var(--warn-text); border-radius: .5rem;
           padding: .6rem .75rem; font-size: .9rem; text-align: left; }
.dialog .actions { grid-template-columns: 1fr 1fr; }
</style>
</head>
<body>
<header><h1>Oke Gass</h1><span id="mode">Mode setting</span></header>

<section>
  <h2>Suara contoh</h2>
  <label class="switch">Putar musik contoh<input type="checkbox" id="sample"></label>
  <p class="hint" id="sample-hint">Bluetooth mati selama mode setting. Musik contoh
  diputar berulang supaya perubahan langsung terdengar.</p>
</section>
<section>
  <h2>Preset bass</h2>
  <div class="presets">
    <button data-cmd="bass:punch">Punch</button>
    <button data-cmd="bass:soft">Soft</button>
    <button data-cmd="bass:flat">Flat</button>
  </div>
</section>
<section><h2>Bass</h2><div id="bass"></div></section>
<section><h2>Level</h2><div id="level"></div></section>
<section>
  <h2>Mode</h2>
  <label class="switch">Mono + DSP<input type="checkbox" id="mono"></label>
  <p class="hint">Kalau dimatikan, bass, crossover, dan limiter tidak aktif.</p>
</section>
<section>
  <h2>Reset</h2>
  <button id="reset" style="width:100%">Kembalikan ke default</button>
</section>

<footer>
  <div id="message"></div>
  <div class="actions">
    <button id="save" class="primary">Simpan</button>
    <button id="done">Selesai</button>
  </div>
</footer>

<div id="exit" hidden>
  <div class="dialog" role="dialog" aria-live="polite">
    <div id="counting">
      <p>Kembali ke mode Bluetooth dalam</p>
      <div id="seconds">7</div>
      <p class="warning" id="unsaved" hidden>Ada perubahan yang belum disimpan.
      Perubahan itu akan hilang kecuali kamu menekan Simpan sebelum waktu habis.</p>
      <div class="actions">
        <button id="cancel" class="primary">Batal</button>
        <button id="exit-save">Simpan</button>
      </div>
    </div>
    <div id="gone" hidden>
      <p><strong>Speaker kembali ke mode Bluetooth.</strong></p>
      <p class="hint">WiFi speaker sudah mati. Sambungkan HP ke WiFi biasa,
      lalu sambungkan Bluetooth seperti biasa.</p>
    </div>
  </div>
</div>

<script>
const GROUPS = {
  bass: [
    ['punch_db', 'Punch', 0, 8, 0.5, ' dB'],
    ['punch_hz', 'Frekuensi punch', 50, 150, 5, ' Hz'],
    ['hpf_hz', 'Batas bawah woofer', 30, 100, 5, ' Hz'],
    ['bass_db', 'Shelf', -6, 6, 0.5, ' dB'],
    ['bass_hz', 'Frekuensi shelf', 40, 300, 5, ' Hz'],
  ],
  level: [
    ['gain_w', 'Woofer', 0, 2, 0.1, ''],
    ['gain_t', 'Tweeter', 0, 2, 0.1, ''],
    ['freq', 'Crossover', 200, 10000, 100, ' Hz'],
    ['wav_vol', 'Volume chime', 0, 1, 0.1, ''],
  ],
};
const $ = (id) => document.getElementById(id);
const sliders = {};
const message = $('message');
let queue = Promise.resolve();
let exiting = false;
let gone = false;

function showValue(item, value) {
  item.label.textContent = Number(value).toFixed(item.step < 1 ? 1 : 0) + item.unit;
}

function render(status) {
  for (const [key, item] of Object.entries(sliders)) {
    // Jangan timpa slider yang sedang dipegang.
    if (document.activeElement === item.input) continue;
    item.input.value = status[key];
    showValue(item, status[key]);
  }
  $('mono').checked = status.mono;
  $('sample').checked = status.sample_on;
  $('sample').disabled = !status.sample_ok;
  if (!status.sample_ok) {
    $('sample-hint').textContent = 'File data/sample.wav tidak ada atau formatnya salah ' +
        '(harus WAV 16-bit 44,1 kHz). Upload lewat Upload Filesystem Image.';
  }
  if (status.dirty) message.textContent = 'Ada perubahan yang belum disimpan';
  else if (message.textContent !== 'Tersimpan') message.textContent = '';

  exiting = status.exit_in >= 0;
  $('exit').hidden = !exiting && !gone;
  if (exiting) {
    $('seconds').textContent = Math.ceil(status.exit_in / 1000);
    $('unsaved').hidden = !status.dirty;
    $('exit-save').hidden = !status.dirty;
  }
}

function showGone() {
  gone = true;
  $('exit').hidden = false;
  $('counting').hidden = true;
  $('gone').hidden = false;
}

// Satu request per waktu: server di ESP32 melayani klien satu per satu.
function request(path, options) {
  const run = () => fetch(path, options).then((response) => {
    if (!response.ok) throw new Error(response.status);
    return response.json();
  }).then(render).catch((error) => {
    // Gagal saat hitung mundur berarti speaker sudah restart.
    if (exiting) showGone();
    else message.textContent = 'Speaker tidak terhubung. Mode setting mungkin sudah selesai.';
    throw error;
  });
  queue = queue.then(run, run);
  return queue;
}

function send(command) {
  return request('/cmd', {method: 'POST', body: new URLSearchParams({c: command})})
      .catch(() => {});
}

for (const [group, rows] of Object.entries(GROUPS)) {
  const parent = $(group);
  for (const [key, name, min, max, step, unit] of rows) {
    const row = document.createElement('div');
    row.className = 'row';
    const head = document.createElement('div');
    head.className = 'head';
    const title = document.createElement('span');
    title.textContent = name;
    const label = document.createElement('span');
    label.className = 'value';
    head.append(title, label);
    const input = document.createElement('input');
    input.type = 'range';
    input.setAttribute('aria-label', name);
    Object.assign(input, {min, max, step});
    row.append(head, input);
    parent.append(row);

    const item = {input, label, step, unit};
    sliders[key] = item;
    input.addEventListener('input', () => showValue(item, input.value));
    // Kirim saat slider dilepas: setiap perubahan filter me-reset state-nya.
    input.addEventListener('change', () => {
      send(key + ':' + input.value).then(() => input.blur());
    });
  }
}

document.querySelectorAll('.presets button').forEach((button) => {
  button.addEventListener('click', () => send(button.dataset.cmd));
});
$('mono').addEventListener('change', (event) => {
  send('mono:' + (event.target.checked ? 'on' : 'off'));
});
$('sample').addEventListener('change', (event) => {
  send('sample:' + (event.target.checked ? 'on' : 'off'));
});
$('save').addEventListener('click', () => {
  send('save').then(() => { message.textContent = 'Tersimpan'; });
});
$('exit-save').addEventListener('click', () => send('save'));
$('done').addEventListener('click', () => send('config:off'));
$('cancel').addEventListener('click', () => send('config:cancel'));
$('reset').addEventListener('click', () => {
  if (confirm('Kembalikan semua setting ke default? Setting tersimpan ikut diganti.')) {
    send('reset');
  }
});

// Polling status: menangkap hitung mundur yang dimulai dari tombol BOOT.
function poll() {
  if (gone) return;
  request('/status?poll=1').catch(() => {}).then(() => {
    setTimeout(poll, exiting ? 300 : 2000);
  });
}
poll();
</script>
</body>
</html>
)HTML";
