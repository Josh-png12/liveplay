// Real renderer test using installed Edge + its JSON/CDP interface. No new npm dependency.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { spawn } = require('node:child_process');
const assert = require('node:assert/strict');
module.exports = async function libraryUI({ root, port, token, wav, api }) {
  const executable = process.env.LIVEPLAY_TEST_BROWSER || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
  assert(fs.existsSync(executable), 'Install Edge or set LIVEPLAY_TEST_BROWSER to a Chromium-compatible browser');
  const publicRoot = path.resolve('client/.output/public');
  assert(fs.existsSync(path.join(publicRoot, 'index.html')), 'Run npm run build:client before the UI E2E');
  const staticServer = http.createServer((req, response) => {
    let target;
    try { target = path.resolve(publicRoot, '.' + decodeURIComponent(new URL(req.url, 'http://localhost').pathname)); }
    catch { response.writeHead(400).end(); return; }
    if (!target.startsWith(publicRoot + path.sep) && target !== publicRoot) { response.writeHead(403).end(); return; }
    if (fs.existsSync(target) && fs.statSync(target).isDirectory()) target = path.join(target, 'index.html');
    if (!fs.existsSync(target)) { response.writeHead(404).end(); return; }
    const mime = { '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html', '.json': 'application/json', '.svg': 'image/svg+xml' }[path.extname(target)] || 'application/octet-stream';
    response.writeHead(200, { 'content-type': mime }); fs.createReadStream(target).pipe(response);
  });
  await new Promise(resolve => staticServer.listen(4578, '127.0.0.1', resolve));
  const browser = spawn(executable, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--remote-debugging-port=4581', `--user-data-dir=${path.join(root, 'browser-profile')}`, 'about:blank'], { windowsHide: true, stdio: 'ignore' });
  let socket;
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  try {
    let tabs;
    for (let i = 0; i < 100; i++) {
      try { tabs = await (await fetch('http://127.0.0.1:4581/json/list')).json(); if (tabs.some(t => t.type === 'page')) break; } catch {}
      await sleep(100);
    }
    assert(tabs?.some(t => t.type === 'page'), 'headless browser starts');
    socket = new WebSocket(tabs.find(t => t.type === 'page').webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    let sequence = 0; const pending = new Map(); const browserLog = [];
    socket.onmessage = event => { const msg = JSON.parse(event.data); if (msg.id) { const p = pending.get(msg.id); pending.delete(msg.id); if (msg.error) p?.reject(new Error(JSON.stringify(msg.error))); else p?.resolve(msg.result); } else if (msg.method === 'Network.loadingFailed' || msg.method === 'Runtime.consoleAPICalled' || msg.method === 'Runtime.exceptionThrown') browserLog.push(msg); };
    const command = (method, params = {}) => new Promise((resolve, reject) => { const id = ++sequence; pending.set(id, {resolve, reject}); socket.send(JSON.stringify({id, method, params})); });
    const evaluate = async expression => {
      const result = await command('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
      if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
      return result.result.value;
    };
    const wait = async expression => { for (let i = 0; i < 200; i++) { if (await evaluate(expression)) return; await sleep(100); } throw new Error(`UI timeout: ${expression}\n${await evaluate('document.body.innerText')}\n${JSON.stringify(browserLog).slice(-12000)}`); };
    const click = async text => evaluate(`Array.from(document.querySelectorAll('button')).find(b => b.textContent.trim() === ${JSON.stringify(text)}).click()`);
    await command('Page.enable'); await command('Runtime.enable'); await command('Network.enable');
    await command('Page.addScriptToEvaluateOnNewDocument', {source: `localStorage.setItem('liveplay.serverUrl', 'http://127.0.0.1:${port}'); localStorage.setItem('liveplay.authToken', ${JSON.stringify(token)});`});
    await command('Page.navigate', {url: 'http://127.0.0.1:4578/'});
    await wait(`!!document.querySelector('.plus-library-launch')`);
    assert.equal(await evaluate(`localStorage.getItem('liveplay.serverUrl')`), `http://127.0.0.1:${port}`);
    await click('Biblioteca musical');
    await wait(`document.querySelectorAll('.plus-library tbody tr').length >= 3`);
    const choose = async (files, drop = false) => {
      const values = files.map(([name, bytes]) => ({ name, bytes: bytes.toString('base64') }));
      await evaluate(`(() => { const dt = new DataTransfer(); for (const f of ${JSON.stringify(values)}) dt.items.add(new File([Uint8Array.from(atob(f.bytes), c => c.charCodeAt(0))], f.name, {type:'audio/wav'}));
        ${drop ? "document.querySelector('.plus-library').dispatchEvent(new DragEvent('drop', {dataTransfer:dt,bubbles:true,cancelable:true}));" : "const input = document.querySelector('.plus-library input[type=file]'); input.files=dt.files; input.dispatchEvent(new Event('change', {bubbles:true}));"} })()`);
    };
    await choose([['ui selected.wav', wav(6000)], ['ui duplicate.wav', wav(1000)]]);
    await wait(`document.querySelector('.plus-library').textContent.includes('Duplicado detectado') && document.querySelector('.plus-library').textContent.includes('ui selected')`);
    await choose([['ui dragged.wav', wav(7000)], ['bad.wav', Buffer.from('not audio')]], true);
    await wait(`document.querySelector('.plus-library').textContent.includes('ui dragged') && document.querySelector('.plus-library').textContent.includes('failed')`);
    await evaluate(`(() => {const search = document.querySelector('.plus-library input[type=search]'); search.value='ui selected'; search.dispatchEvent(new Event('input',{bubbles:true}));})()`);
    await wait(`document.querySelectorAll('.plus-library tbody tr').length === 1`);
    assert((await evaluate(`document.querySelector('.plus-library tbody').textContent`)).includes('ui selected'));
    await evaluate(`(() => {const search = document.querySelector('.plus-library input[type=search]'); search.value=''; search.dispatchEvent(new Event('input',{bubbles:true}));})()`);
    await click('Actualizar');
    await wait(`document.querySelectorAll('.plus-library tbody tr').length >= 5`);
    await evaluate(`document.querySelector('.plus-library th button').click()`);
    assert((await evaluate(`document.querySelector('.plus-library th button').textContent`)).includes('↓'));
    const page = (await api('/api/plus/library/tracks')).body;
    const missing = page.tracks.find(t => t.title === 'ui selected'); fs.unlinkSync(missing.asset_path);
    await click('Actualizar');
    await wait(`document.querySelector('.plus-library tbody').textContent.includes('Asset faltante')`);
    await evaluate(`document.querySelectorAll('.plus-library .filters select')[1].value='missing'; document.querySelectorAll('.plus-library .filters select')[1].dispatchEvent(new Event('change',{bubbles:true}))`);
    await wait(`document.querySelectorAll('.plus-library tbody tr').length === 1`);
    await click('Re-vincular'); await choose([['replacement.wav', wav(6000)]]);
    await wait(`document.querySelectorAll('.plus-library tbody tr').length === 0`);
    await evaluate(`document.querySelectorAll('.plus-library .filters select')[1].value=''; document.querySelectorAll('.plus-library .filters select')[1].dispatchEvent(new Event('change',{bubbles:true}))`);
    assert.equal((await api(`/api/plus/library/tracks/${missing.id}`)).body.status, 'available');
    await click('Cerrar'); await click('Biblioteca musical');
    await wait(`document.querySelectorAll('.plus-library details').length >= 3`);
    console.log('PASS real Library UI: multi-select, drag/drop, duplicates, errors, search, sorting, missing filter, relink and batch restoration');
  } finally {
    if (socket) socket.close();
    if (browser.exitCode === null) { const exited = new Promise(resolve => browser.once('exit', resolve)); browser.kill(); await exited; }
    await new Promise(resolve => staticServer.close(resolve));
  }
};
