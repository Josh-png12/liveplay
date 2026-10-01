// Isolated server + temporary store. No active installation/project is touched.
// node server/tests/e2e/plus-library-e2e.js [server-executable]
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');

const exe = path.resolve(process.argv[2] || 'server/build/liveplay-server.exe');
const buildRoot = path.resolve('server/build');
fs.mkdirSync(buildRoot, { recursive: true });
const root = fs.mkdtempSync(path.join(buildRoot, 'plus-e2e-'));
// The existing main() parses narrow Windows argv using the system code page.
// Keep the CLI fs-root ASCII; Unicode filenames still exercise HTTP/path I/O.
const inputs = path.join(root, 'inputs');
fs.mkdirSync(inputs);
const serverExe = path.join(root, path.basename(exe));
fs.copyFileSync(exe, serverExe);
for (const name of fs.readdirSync(path.dirname(exe))) {
  if (name.endsWith('.dll')) fs.copyFileSync(path.join(path.dirname(exe), name), path.join(root, name));
}
const port = 4579;
let proc;
let log = '';
let token;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
function wav(amplitude) {
  const bytes = Buffer.alloc(44 + 48000 * 4);
  bytes.write('RIFF'); bytes.writeUInt32LE(bytes.length - 8, 4); bytes.write('WAVEfmt ', 8);
  bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(1, 20); bytes.writeUInt16LE(2, 22);
  bytes.writeUInt32LE(48000, 24); bytes.writeUInt32LE(192000, 28);
  bytes.writeUInt16LE(4, 32); bytes.writeUInt16LE(16, 34); bytes.write('data', 36);
  bytes.writeUInt32LE(bytes.length - 44, 40);
  for (let i = 0; i < 48000; i++) bytes.writeInt16LE(i % 100 < 50 ? amplitude : -amplitude, 44 + i * 4);
  return bytes;
}
async function api(route, body, options = {}) {
  const headers = { ...(body === undefined ? {} : { 'content-type': 'application/json' }),
    ...(token ? { authorization: `Bearer ${token}` } : {}), ...options.headers };
  const response = await fetch(`http://127.0.0.1:${port}${route}`, {
    method: body === undefined ? 'GET' : 'POST', body: body === undefined ? undefined : JSON.stringify(body),
    ...options, headers, signal: AbortSignal.timeout(10000)
  });
  return { status: response.status, body: await response.json() };
}
async function start() {
  proc = spawn(serverExe, ['--bind', '127.0.0.1', '--port', String(port),
    '--fs-root', inputs, '--max-upload-mb', '1'], { cwd: root, env: { ...process.env, LIVEPLAY_PLUS_DATA_DIR: path.join(root, 'plus') }, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  proc.stdout.on('data', data => { log += data; });
  proc.stderr.on('data', data => { log += data; });
  for (let i = 0; i < 100; i++) {
    try { if ((await api('/api/health')).status === 200) return; } catch {}
    if (proc.exitCode !== null) throw new Error(`server exited: ${log}`);
    await sleep(100);
  }
  throw new Error(`server did not start: ${log}`);
}
async function stop() {
  if (!proc || proc.exitCode !== null) return;
  const exited = new Promise(resolve => proc.once('exit', resolve));
  proc.kill();
  await exited;
}
async function done(id) {
  for (let i = 0; i < 300; i++) {
    const response = await api(`/api/plus/imports/${id}`);
    assert.equal(response.status, 200);
    if (['completed', 'cancelled', 'failed'].includes(response.body.status)) return response.body;
    await sleep(50);
  }
  throw new Error(`batch timed out: ${id}`);
}
async function upload(files) {
  const form = new FormData();
  for (const [name, bytes] of files) form.append('file', new Blob([bytes]), name);
  const response = await fetch(`http://127.0.0.1:${port}/api/plus/imports/upload`, {
    method: 'POST', body: form, headers: token ? { authorization: `Bearer ${token}` } : {},
    signal: AbortSignal.timeout(10000)
  });
  return { status: response.status, body: await response.json() };
}
async function main() {
  const a = path.join(inputs, 'primera canción.wav');
  const duplicate = path.join(inputs, 'renamed.wav');
  const b = path.join(inputs, 'second.wav');
  const bad = path.join(inputs, 'bad.wav');
  fs.writeFileSync(a, wav(1000)); fs.copyFileSync(a, duplicate); fs.writeFileSync(b, wav(2000));
  fs.writeFileSync(bad, 'not audio');
  await start();
  assert.equal((await api('/api/plus/capabilities')).body.library, true);
  assert.equal((await api('/api/plus/library/tracks')).body.total, 0);
  const beforeProject = (await api('/api/project')).body;
  const beforeCues = (await api('/api/cues')).body;
  assert.equal((await api('/api/plus/library/tracks?limit=501')).status, 400);
  assert.equal((await api('/api/plus/library/tracks?offset=-1')).status, 400);
  assert.equal((await api('/api/plus/library/tracks/unknown')).status, 404);
  assert.equal((await api('/api/plus/imports', { files: ['relative.wav'] })).status, 400);
  assert.equal((await api('/api/plus/imports', { files: [path.join(root, 'denied.wav')] })).status, 403);
  assert.equal((await api('/api/plus/imports', { files: [] })).status, 400);
  const body = { files: [a, duplicate, b, bad, path.join(inputs, 'missing.wav')], requestId: 'first-batch' };
  const [first, retry] = await Promise.all([api('/api/plus/imports', body), api('/api/plus/imports', body)]);
  assert.equal(first.status, 202); assert.equal(first.body.id, retry.body.id);
  const result = await done(first.body.id);
  assert.equal(result.status, 'completed');
  assert.deepEqual(result.files.map(file => file.status), ['imported', 'duplicate', 'imported', 'failed', 'failed']);
  assert.equal(result.files[0].track_id, result.files[1].track_id);
  assert.equal((await api('/api/plus/imports', { files: [b], requestId: 'first-batch' })).status, 400);
  let page = await api('/api/plus/library/tracks');
  assert.equal(page.body.total, 2);
  const track = (await api(`/api/plus/library/tracks/${result.files[0].track_id}`)).body;
  assert.equal(track.title, 'primera canción');
  assert.equal(track.duration_ms, 1000); assert.equal(track.sample_rate, 48000); assert.equal(track.channels, 2);
  const digest = crypto.createHash('sha256').update(fs.readFileSync(a)).digest('hex');
  assert.equal(track.content_hash, `sha256:${digest}`);
  assert.equal(track.id, `track_${digest}`);
  assert.equal(path.isAbsolute(JSON.parse(fs.readFileSync(path.join(root, 'plus/library/catalog.json'), 'utf8')).tracks[0].asset_path), false);
  assert.deepEqual(fs.readFileSync(track.asset_path), fs.readFileSync(a));
  assert.equal((await api('/api/plus/library/tracks?offset=1&limit=1')).body.tracks.length, 1);
  assert.deepEqual((await api('/api/project')).body, beforeProject);
  assert.deepEqual((await api('/api/cues')).body, beforeCues);
  console.log('PASS batch import, independent SHA-256, duplicate identity, validation and legacy state isolation');

  const uploaded = await upload([['uploaded.wav', wav(3000)], ['duplicate.wav', wav(1000)]]);
  assert.equal(uploaded.status, 202);
  const uploadResult = await done(uploaded.body.id);
  assert.deepEqual(uploadResult.files.map(file => file.status), ['imported', 'duplicate']);
  const uploadTrack = (await api(`/api/plus/library/tracks/${uploadResult.files[0].track_id}`)).body;
  assert.equal(uploadTrack.import_origin, 'upload'); assert.equal(uploadTrack.title, 'uploaded');
  assert.equal((await upload([['valid.wav', wav(4000)], ['bad:ads.wav', wav(5000)]])).status, 400);
  assert.equal(fs.readdirSync(path.join(root, 'plus/library/uploads')).length, 0);
  assert.equal((await upload([['large.wav', Buffer.alloc(1024 * 1024 + 1)]])).status, 413);
  console.log('PASS project-free multipart import, cleanup, filename safety and upload limit');

  // Force a long enough batch to exercise cancellation while processing files.
  const cancelling = await api('/api/plus/imports', { files: Array(1000).fill(a) });
  assert.equal(cancelling.status, 202);
  assert.equal((await api(`/api/plus/imports/${cancelling.body.id}/cancel`, {})).status, 200);
  const cancelled = await done(cancelling.body.id);
  assert.equal(cancelled.status, 'cancelled');
  assert(cancelled.files.some(file => file.status === 'cancelled'));
  assert.equal((await api('/api/plus/imports/missing/cancel', {})).status, 404);
  console.log('PASS cooperative batch cancellation and per-file progress');

  await stop(); await start();
  page = await api('/api/plus/library/tracks'); assert.equal(page.body.total, 3);
  assert.equal((await api(`/api/plus/imports/${first.body.id}`)).body.status, 'completed');
  assert.equal((await api(`/api/plus/library/tracks/${track.id}`)).body.content_hash, track.content_hash);
  assert.deepEqual((await api('/api/cues')).body, beforeCues);
  console.log('PASS catalog and batches survive server restart without engine cues');
  assert.deepEqual((await api('/api/plus/capabilities')).body.analysis, {basic:false,loudness:false,rhythm:false,tonality:false,structure:false,semantic:false,stems:false});
  assert.deepEqual((await api('/api/plus/analysis/jobs')).body.jobs, []);
  const analysis = await api('/api/plus/analysis/jobs', {track_id: track.id, provider: 'unavailable'});
  assert.equal(analysis.status, 202);
  for (let i=0;i<100;i++) { if ((await api('/api/plus/analysis/jobs')).body.jobs[0].status === 'failed') break; await sleep(20); }
  assert.equal((await api(`/api/plus/analysis/tracks/${track.id}`)).body.status, 'failed');
  assert.deepEqual((await api('/api/project')).body, beforeProject);
  assert.deepEqual((await api('/api/cues')).body, beforeCues);
  assert.equal((await api('/api/plus/library/maintenance', {operation:'cleanup', assets:['unknown'], confirm:false})).status, 400);
  console.log('PASS empty capabilities/scheduler and failed analysis preserve project and engine state');

  // New routes follow existing human-session policy; API tokens cannot browse
  // asset paths or initiate filesystem ingestion.
  const created = await api('/api/users', { name: 'plus-admin', password: 'Test-plus-library-123!' });
  assert.equal(created.status, 200);
  assert.equal((await api('/api/plus/capabilities')).status, 401);
  const login = await api('/api/auth/login', { name: 'plus-admin', password: 'Test-plus-library-123!' });
  assert.equal(login.status, 200); token = login.body.token;
  assert.equal((await api('/api/plus/library/tracks')).status, 200);
  const minted = await api('/api/tokens', { name: 'plus-test', password: 'Test-plus-library-123!' });
  assert.equal(minted.status, 200);
  const apiToken = minted.body.token;
  assert.equal(typeof apiToken, 'string');
  assert.equal((await api('/api/plus/library/tracks', undefined, { headers: { authorization: `Bearer ${apiToken}` } })).status, 403);
  assert.equal((await api('/api/plus/imports', { files: [a] }, { headers: { authorization: `Bearer ${apiToken}` } })).status, 403);
  console.log('PASS authentication and API-token filesystem boundary');
  await stop();

  // Corrupt Plus persistence disables only the extension, preserving legacy boot.
  fs.writeFileSync(path.join(root, 'plus/library/catalog.json'), '{');
  token = undefined;
  await start();
  const signedIn = await api('/api/auth/login', { name: 'plus-admin', password: 'Test-plus-library-123!' });
  token = signedIn.body.token;
  assert.equal((await api('/api/plus/capabilities')).body.library, false);
  assert.equal((await api('/api/plus/library/tracks')).status, 503);
  assert.equal((await api('/api/project')).status, 200);
  assert.equal(fs.readFileSync(path.join(root, 'plus/library/catalog.json'), 'utf8'), '{');
  console.log('PASS corrupt library fails closed without damaging legacy server');
  const recovery = await api('/api/plus/library/recovery');
  assert(recovery.body.backups.some(name => name.startsWith('catalog.corrupt.')));
  assert.equal((await api('/api/plus/library/recovery', {action:'reset'})).status, 400);
  assert.equal((await api('/api/plus/library/recovery', {action:'restore', backup:'catalog.backup.json', confirm:true})).status, 200);
  await stop(); await start();
  assert.equal((await api('/api/plus/capabilities')).body.library, true);
  assert.equal((await api('/api/plus/library/tracks')).body.total, 2); // Previous commit backup retained.
  fs.writeFileSync(path.join(root, 'plus/assets/unreferenced.bin'), 'orphan fixture');
  const maintain = async body => {
    assert.equal((await api('/api/plus/library/maintenance', body)).status, 202);
    for (let i=0;i<200;i++) { const report = (await api('/api/plus/library/maintenance')).body; if (report.status !== 'running') { assert.equal(report.status, 'completed'); return report; } await sleep(20); }
    throw new Error('maintenance timeout');
  };
  const report = await maintain({operation:'verify'});
  assert(report.orphans.includes('unreferenced.bin'));
  const cleaned = await maintain({operation:'cleanup', assets:['unreferenced.bin',path.basename(track.asset_path)], confirm:true});
  assert.equal(cleaned.removed, 1); assert(fs.existsSync(track.asset_path));
  const rebuilt = await maintain({operation:'rebuild'}); assert.equal(rebuilt.recovered, 1);
  console.log('PASS explicit verification, selected orphan cleanup and rebuild via API');
  if (process.argv.includes('--ui')) {
    await require('./plus-library-ui')({root, port, token, wav, api});
    assert.deepEqual((await api('/api/cues')).body, beforeCues);
  }
  console.log('PASS explicit backup recovery and restart without deleting assets');
}
main().catch(error => { console.error(error); console.error(log.slice(-8000)); process.exitCode = 1; })
  .finally(async () => {
    await stop();
    // Workspace-local unique fixture only.
    if (path.dirname(root) !== buildRoot) throw new Error('unsafe test cleanup target');
    fs.rmSync(root, { recursive: true, force: true });
  });
