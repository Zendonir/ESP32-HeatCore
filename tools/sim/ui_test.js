// End-to-end test of the web UI against the host simulator:
//   ./build/host/heatcore_sim --demo --port 8099 &
//   HC_SHOTS=/tmp node tools/sim/ui_test.js
const { chromium } = require('playwright');
const crypto = require('crypto');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1280, height: 900 } });
  const errors = [];
  p.on('pageerror', e => errors.push('pageerror ' + e.message));
  p.on('console', m => { if (m.type() === 'error' && !m.text().includes('WebSocket') && !m.text().includes('status of 422')) errors.push('console ' + m.text()); });
  const base = 'http://127.0.0.1:' + (process.env.HC_PORT || 8099) + '/';
  const routes = ['#/', '#/rooms', '#/sensors', '#/model', '#/rf', '#/rf/learn', '#/rf/monitor', '#/rf/tx', '#/rf/sensors', '#/rf/diag',
    '#/display', '#/display/pages', '#/display/settings', '#/display/status', '#/rules', '#/rules/catalog', '#/rules/test', '#/rules/log',
    '#/hw/outputs', '#/hw/inputs', '#/hw/analog', '#/hw/onewire', '#/hw/selftest', '#/sys/safety', '#/sys/network', '#/sys/mqtt', '#/sys/general', '#/sys/firmware'];
  await p.goto(base);
  for (const r of routes) {
    await p.goto(base + r); await p.waitForTimeout(900);
    const bad = await p.$('.card.bad'); if (bad) errors.push(r + ': ' + await bad.textContent());
    if (['#/', '#/display', '#/rf/learn', '#/rules', '#/hw/inputs', '#/rf/diag'].includes(r)) await p.screenshot({ path: (process.env.HC_SHOTS || '.') + '/' + r.replace(/[#/]/g, '_') + '.png', fullPage: true });
  }
  // ---- learn flow through the UI
  await p.goto(base + '#/rf/learn'); await p.waitForTimeout(800);
  await p.click('button:has-text("Lernmodus starten")');
  for (let i = 0; i < 2; i++) { await p.request.post(base + 'sim/rf', { data: { code: '0x77AB31', pulseUs: 410 } }); await p.waitForTimeout(500); }
  await p.waitForTimeout(1500);
  await p.click('button:has-text("Übernehmen")');
  await p.waitForTimeout(1000);
  const nameInput = await p.$('.form input');
  await nameInput.fill('Neuer Handsender');
  await p.click('button:has-text("+ Ereignis zuordnen")');
  await p.waitForTimeout(300);
  await p.click('button:has-text("+ Aktion")');
  await p.waitForTimeout(300);
  await p.screenshot({ path: (process.env.HC_SHOTS || '.') + '/learn_form.png', fullPage: true });
  await p.click('#main button.p:has-text("Speichern")');
  await p.waitForTimeout(1000);
  const devs = await (await p.request.get(base + 'api/rf/devices')).json();
  if(!devs.devices.some(d=>d.name==='Neuer Handsender'))errors.push('learned device not saved');
  console.log('devices after learn:', devs.devices.map(d => d.name + ' ' + d.buttons.map(b => b.code + ':' + JSON.stringify(b.bindings)).join(',')));
  // ---- rule editor: create rule
  await p.goto(base + '#/rules'); await p.waitForTimeout(900);
  await p.click('text=+ Neue Regel'); await p.waitForTimeout(500);
  const ed = (await p.$$('.card')).pop();
  await (await ed.$('input')).fill('Testregel UI');
  await (await ed.$$('input[type=checkbox]'))[0].check();
  await p.click('button:has-text("+ Aktion")'); await p.waitForTimeout(300);
  await p.click('button:has-text("Prüfen")'); await p.waitForTimeout(500);
  await p.click('#main .card:last-child button.p:has-text("Speichern")'); await p.waitForTimeout(800);
  const rules = await (await p.request.get(base + 'api/rules')).json();
  if(!rules.rules.some(r=>r.name==='Testregel UI'&&r.enabled))errors.push('rule not enabled');
  console.log('rules:', rules.rules.map(r => r.name + (r.enabled ? ' [on]' : ' [off]') + ' ' + JSON.stringify(r.actions)));
  // ---- firmware update: online (release from a mocked GitHub Pages) and file upload
  const img = Buffer.alloc(70000);
  for (let i = 0; i < img.length; i++) img[i] = (i * 131 + 7) & 0xff;
  img[0] = 0xe9; img[12] = 0x09; img[13] = 0x00; img.writeUInt32LE(0xabcd5432, 32);
  Buffer.from('HCFW1|KC868-A16V3|9.9.9\0').copy(img, 2000);
  const sha = crypto.createHash('sha256').update(img).digest('hex');
  const release = { version: '9.9.9', board: 'KC868-A16V3', date: '2026-10-29', notes: '- Testrelease', releaseUrl: 'https://example.test/rel',
    ota: { file: 'heatcore-ota.bin', size: img.length, sha256: sha } };
  await p.route('https://updates.example.test/**', route => {
    const u = route.request().url();
    const hdr = { 'Access-Control-Allow-Origin': '*' };
    if (u.endsWith('release.json')) return route.fulfill({ status: 200, headers: hdr, contentType: 'application/json', body: JSON.stringify(release) });
    if (u.endsWith('heatcore-ota.bin')) return route.fulfill({ status: 200, headers: hdr, contentType: 'application/octet-stream', body: img });
    return route.fulfill({ status: 404, headers: hdr, body: '' });
  });
  await p.request.put(base + 'api/config/system', { data: { updateUrl: 'https://updates.example.test/hc/' } });
  await p.goto(base + '#/sys/firmware'); await p.waitForTimeout(800);
  await p.click('button:has-text("Nach Updates suchen")');
  await p.waitForSelector('text=Verfügbar: 9.9.9', { timeout: 5000 }).catch(() => errors.push('release not shown'));
  await p.click('button:has-text("Jetzt installieren")');
  await p.waitForTimeout(12000);
  let fw = await (await p.request.get(base + 'api/firmware')).json();
  if (!String(fw.lastResult).includes('9.9.9 installiert')) errors.push('online update: ' + fw.lastResult);
  console.log('online update:', fw.lastResult);
  // a foreign file is rejected by the device-side check
  await p.goto(base + '#/sys/firmware'); await p.waitForTimeout(800);
  await p.setInputFiles('input[type=file]', { name: 'tasmota.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(5000, 0x41) });
  await p.click('button:has-text("Hochladen und installieren")');
  await p.waitForTimeout(1500);
  fw = await (await p.request.get(base + 'api/firmware')).json();
  if (!String(fw.lastResult).includes('keine ESP32-Firmware')) errors.push('foreign file: ' + fw.lastResult);
  console.log('foreign file:', fw.lastResult);
  await p.goto(base + '#/sys/firmware'); await p.waitForTimeout(800);
  if (!(await p.$('button:has-text("Vorherige Firmware starten")'))) errors.push('rollback button missing');
  await p.screenshot({ path: (process.env.HC_SHOTS || '.') + '/firmware.png', fullPage: true });
  // ---- display canvas has pixels
  await p.goto(base + '#/display'); await p.waitForTimeout(1500);
  const lit = await p.evaluate(() => { const c = document.querySelector('canvas'); const d = c.getContext('2d').getImageData(0, 0, 128, 64).data; let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i + 2] > 0) n++; return n; });
  console.log('lit OLED pixels:', lit);
  // mobile layout screenshot
  await p.setViewportSize({ width: 390, height: 800 }); await p.goto(base + '#/'); await p.waitForTimeout(1000);
  await p.screenshot({ path: (process.env.HC_SHOTS || '.') + '/mobile_dashboard.png', fullPage: true });
  console.log('ERRORS:', errors.length ? errors.join('\n') : 'none');
  await b.close();
  process.exit(errors.length ? 1 : 0);
})();
