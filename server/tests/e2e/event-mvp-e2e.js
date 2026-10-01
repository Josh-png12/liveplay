// Event MVP API smoke: import -> tag -> waveform -> recommendations.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');

const exe = path.resolve(process.argv[2] || 'server/build/liveplay-server.exe');
const root = fs.mkdtempSync(path.join(path.resolve('server/build'), 'event-mvp-e2e-'));
const inputs = path.join(root, 'inputs'); fs.mkdirSync(inputs);
const serverExe = path.join(root, path.basename(exe)); fs.copyFileSync(exe, serverExe);
for (const name of fs.readdirSync(path.dirname(exe))) if (name.endsWith('.dll')) fs.copyFileSync(path.join(path.dirname(exe), name), path.join(root, name));
const wavPath = path.join(inputs, 'event.wav');
const bytes = Buffer.alloc(44 + 48000 * 4); bytes.write('RIFF'); bytes.writeUInt32LE(bytes.length - 8, 4); bytes.write('WAVEfmt ', 8); bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(1, 20); bytes.writeUInt16LE(2, 22); bytes.writeUInt32LE(48000, 24); bytes.writeUInt32LE(192000, 28); bytes.writeUInt16LE(4, 32); bytes.writeUInt16LE(16, 34); bytes.write('data', 36); bytes.writeUInt32LE(bytes.length - 44, 40); for (let i = 0; i < 48000; i++) { bytes.writeInt16LE(i % 100 < 50 ? 7000 : -7000, 44 + i * 4); } fs.writeFileSync(wavPath, bytes);
const port = 4586; let proc; const wait = ms => new Promise(r => setTimeout(r, ms));
async function api(route, options = {}) { const r = await fetch(`http://127.0.0.1:${port}${route}`, { ...options, headers: { 'content-type': 'application/json', ...(options.headers || {}) } }); return { status: r.status, body: await r.json() }; }
async function main() {
  proc = spawn(serverExe, ['--bind', '127.0.0.1', '--port', String(port), '--fs-root', inputs, '--max-upload-mb', '1'], { cwd: root, env: { ...process.env, LIVEPLAY_PLUS_DATA_DIR: path.join(root, 'plus') }, windowsHide: true });
  for (let i = 0; i < 100; i++) { try { if ((await api('/api/health')).status === 200) break; } catch {} await wait(100); }
  const caps = await api('/api/plus/capabilities'); assert.equal(caps.status, 200); assert.equal(caps.body.plus.eventMvp, true);
  const batch = await api('/api/plus/imports', { method: 'POST', body: JSON.stringify({ files: [wavPath] }) }); assert.equal(batch.status, 202);
  let result; for (let i = 0; i < 200; i++) { result = await api(`/api/plus/imports/${batch.body.id}`); if (['completed', 'failed', 'cancelled'].includes(result.body.status)) break; await wait(50); }
  assert.equal(result.body.status, 'completed'); const trackId = result.body.files[0].track_id;
  const tags = await api(`/api/plus/library/tracks/${trackId}/tags`, { method: 'PATCH', body: JSON.stringify({ tags: { energy: 3, event_roles: ['entry'], school_approved: true } }) }); assert.equal(tags.status, 200); assert.equal(tags.body.tags.energy, 3);
  const wf = await api(`/api/plus/library/tracks/${trackId}/waveform`, { method: 'POST', body: '{}' }); assert.equal(wf.status, 202);
  let state; for (let i = 0; i < 200; i++) { state = await api(`/api/plus/library/tracks/${trackId}/waveform`); if (['ready', 'failed'].includes(state.body.status)) break; await wait(50); } assert.equal(state.body.status, 'ready'); assert.ok(state.body.channels.length >= 1);
  const rec = await api('/api/plus/recommendations', { method: 'POST', body: JSON.stringify({ eventRole: 'entry', schoolApproved: true, limit: 5 }) }); assert.equal(rec.status, 200); assert.equal(rec.body.tracks[0].id, trackId);
  console.log('Event MVP API flow passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => { if (proc && proc.exitCode === null) proc.kill(); });
