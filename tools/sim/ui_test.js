// End-to-end test of the web UI against the host simulator:
//   ./build/host/heatcore_sim --demo --port 8099 &
//   HC_SHOTS=/tmp node tools/sim/ui_test.js
const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1280, height: 900 } });
  const errors = [];
  p.on('pageerror', e => errors.push('pageerror ' + e.message));
  p.on('console', m => { if (m.type() === 'error' && !m.text().includes('WebSocket')) errors.push('console ' + m.text()); });
  const base = 'http://127.0.0.1:' + (process.env.HC_PORT || 8099) + '/';
  const routes = ['#/', '#/rooms', '#/sensors', '#/model', '#/rf', '#/rf/learn', '#/rf/monitor', '#/rf/tx', '#/rf/sensors', '#/rf/diag',
    '#/display', '#/display/pages', '#/display/settings', '#/display/status', '#/rules', '#/rules/catalog', '#/rules/test', '#/rules/log',
    '#/hw/outputs', '#/hw/inputs', '#/hw/analog', '#/hw/onewire', '#/hw/selftest', '#/sys/safety', '#/sys/network', '#/sys/mqtt', '#/sys/general'];
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
