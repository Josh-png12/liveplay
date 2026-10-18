// Phase 2 end-to-end: real server + real MIR worker (Python/librosa) on synthetic audio.
// node server/tests/e2e/musical-analysis-e2e.js [server-executable]
// Skips (exit 0) when the optional worker environment is not installed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const exe = path.resolve(process.argv[2] || 'server/build/liveplay-server.exe');
const mirDir = path.resolve('server/tools/mir');
const python = path.join(mirDir, '.venv', 'Scripts', 'python.exe');
if (!fs.existsSync(python)) { console.log('SKIP musical analysis E2E: tools/mir/.venv is not installed'); process.exit(0); }
const buildRoot = path.resolve('server/build');
const root = fs.mkdtempSync(path.join(buildRoot, 'musical-e2e-'));
const inputs = path.join(root, 'inputs'); fs.mkdirSync(inputs);
const serverExe = path.join(root, path.basename(exe));
fs.copyFileSync(exe, serverExe);
for (const name of fs.readdirSync(path.dirname(exe))) if (name.endsWith('.dll')) fs.copyFileSync(path.join(path.dirname(exe), name), path.join(root, name));
const port = 4591;
let proc, log = '';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function api(route, body, options = {}) {
  const response = await fetch(`http://127.0.0.1:${port}${route}`, {
    method: body === undefined ? 'GET' : 'POST', body: body === undefined ? undefined : JSON.stringify(body),
    headers: body === undefined ? {} : { 'content-type': 'application/json' }, ...options, signal: AbortSignal.timeout(20000) });
  return { status: response.status, body: await response.json() };
}
async function start(env = {}) {
  log = '';
  proc = spawn(serverExe, ['--bind', '127.0.0.1', '--port', String(port), '--fs-root', inputs],
    { cwd: root, env: { ...process.env, LIVEPLAY_PLUS_DATA_DIR: path.join(root, 'plus'), ...env }, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  proc.stdout.on('data', d => { log += d; }); proc.stderr.on('data', d => { log += d; });
  for (let i = 0; i < 150; i++) {
    try { if ((await api('/api/health')).status === 200) return; } catch {}
    if (proc.exitCode !== null) throw new Error(`server exited: ${log}`);
    await sleep(100);
  }
  throw new Error(`server did not start: ${log}`);
}
async function stop() {
  if (!proc || proc.exitCode !== null) return;
  const exited = new Promise(resolve => proc.once('exit', resolve)); proc.kill(); await exited;
}
async function until(fn, label, ms = 300000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) { const value = await fn(); if (value) return value; await sleep(300); }
  throw new Error(`timed out: ${label}\n${log.slice(-1500)}`);
}
async function importAll(names) {
  const response = await api('/api/plus/imports', { files: names.map(n => path.join(inputs, n + '.wav')) });
  assert.equal(response.status, 202);
  const batch = await until(async () => { const b = (await api(`/api/plus/imports/${response.body.id}`)).body; return ['completed', 'failed'].includes(b.status) ? b : null; }, 'import');
  const ids = {}; names.forEach((n, i) => { ids[n] = batch.files[i].track_id; assert(ids[n], `${n} imported`); });
  return ids;
}
const musical = id => api(`/api/plus/analysis/tracks/${id}/musical`);
const ready = id => until(async () => { const r = (await musical(id)).body; if (r.status === 'failed') throw new Error(`analysis failed: ${JSON.stringify(r)}`); return r.status === 'ready' ? r : null; }, `analysis ${id}`);

async function main() {
  const made = spawnSync(python, ['-I', path.resolve('server/tests/mir/fixtures.py'), inputs], { encoding: 'utf8' });
  assert.equal(made.status, 0, made.stderr);
  await start();

  // ---- capabilities reflect the real provider
  let caps = (await api('/api/plus/capabilities')).body;
  const m = caps.musicalAnalysis;
  assert.equal(m.available, true, `provider should be available: ${JSON.stringify(m)}`);
  assert.equal(m.provider, 'librosa-mir');
  for (const key of ['bpm', 'beats', 'beatgrid', 'keyDetection', 'camelot', 'energyAnalysis', 'structureAnalysis', 'smartCueV1']) assert.equal(m[key], true, key);
  assert.equal(m.downbeats, false, 'downbeats must not be advertised');
  assert(m.cueIntents.some(i => i.id === 'strong-start'));
  assert.equal(caps.analysis.rhythm, true); assert.equal(caps.analysis.tonality, true);
  console.log('PASS capabilities report exactly what the provider can do (downbeats=false)');

  const ids = await importAll(['click120', 'click90', 'minor_f', 'major_c', 'silence', 'short', 'variable_energy', 'noise']);
  assert.equal((await musical(ids.click120)).body.status, 'absent');
  const beforeProject = (await api('/api/project')).body, beforeCues = (await api('/api/cues')).body;
  assert.equal((await api('/api/plus/analysis/tracks/track_nope/musical')).status, 404);
  assert.equal((await api(`/api/plus/analysis/tracks/track_${'0'.repeat(64)}/musical`)).status, 404);

  // ---- BPM + beats
  assert.equal((await api(`/api/plus/analysis/tracks/${ids.click120}/musical`, {})).status, 202);
  const click = await ready(ids.click120);
  const a = click.analysis;
  assert(Math.abs(a.bpm - 120) < 1.0, `bpm ${a.bpm}`);
  assert.equal(a.bpmConfidenceLabel, 'high'); assert.equal(a.bpmAmbiguous, false);
  const offsets = a.beatsMs.map(b => { const k = Math.round((b - 250) / 500); return Math.abs(b - (250 + k * 500)); });
  offsets.sort((x, y) => x - y);
  assert(offsets[Math.floor(offsets.length / 2)] < 15 && offsets[Math.floor(offsets.length * 0.9)] < 30, `beats are aligned with the audio (median ${offsets[Math.floor(offsets.length / 2)]} ms)`);
  assert(a.beatsMs.length >= 55, `${a.beatsMs.length} beats`);
  assert.equal(a.beatGrid.constantTempo, true); assert(Math.abs(a.beatGrid.firstBeatMs - 250) < 20);
  assert.equal(a.key, null, 'a click track has no key'); assert.equal(a.camelot, null);
  assert.deepEqual(a.downbeatsMs, []); assert.equal(a.downbeatsAvailable, false);
  assert.equal(a.provider, 'librosa-mir'); assert(a.providerVersion.startsWith('librosa')); assert.match(a.sourceHash, /^sha256:/);
  console.log(`PASS 120 BPM click: bpm=${a.bpm}, ${a.beatsMs.length} beats, median alignment error ${offsets[Math.floor(offsets.length / 2)]} ms, no key, no downbeats`);

  await api(`/api/plus/analysis/tracks/${ids.click90}/musical`, {});
  const c90 = (await ready(ids.click90)).analysis;
  assert(Math.abs(c90.bpm - 90) < 1.0, `bpm ${c90.bpm}`);
  console.log(`PASS 90 BPM click: bpm=${c90.bpm}`);

  // ---- key + Camelot + energy
  await api('/api/plus/analysis/musical', { mode: 'selected', track_ids: [ids.minor_f, ids.major_c, ids.silence, ids.short, ids.variable_energy, ids.noise] });
  const minor = (await ready(ids.minor_f)).analysis, major = (await ready(ids.major_c)).analysis;
  assert.deepEqual([minor.key, minor.scale, minor.camelot], ['F', 'minor', '4A']);
  assert.deepEqual([major.key, major.scale, major.camelot], ['C', 'major', '8B']);
  assert(minor.keyConfidence > 0.7);
  console.log('PASS key detection: F minor -> 4A, C major -> 8B');

  const silence = (await ready(ids.silence)).analysis;
  assert.equal(silence.bpm, null); assert.equal(silence.key, null); assert.equal(silence.energy, 0); assert.deepEqual(silence.sections, []);
  assert(silence.warnings.some(w => /no signal/.test(w)));
  const short = (await ready(ids.short)).analysis;
  assert.equal(short.bpm, null); assert(short.warnings.some(w => /too short/.test(w)));
  const noise = (await ready(ids.noise)).analysis;
  assert.equal(noise.key, null, 'noise has no key');
  assert(noise.bpm === null || noise.bpmConfidenceLabel === 'low', 'noise tempo must be flagged low confidence');
  console.log('PASS silence / short / noise: nulls and low-confidence flags, never invented values');

  const variable = (await ready(ids.variable_energy)).analysis;
  assert(variable.energy > 0 && variable.energy < 1); assert([1, 2, 3, 4, 5].includes(variable.energyLevel));
  assert(variable.energyCurve.values.length >= 30);
  const quiet = variable.energyCurve.values.slice(0, 8).reduce((x, y) => x + y) / 8, loud = variable.energyCurve.values.slice(14, 26).reduce((x, y) => x + y) / 12;
  assert(loud > quiet + 0.15, `energy curve tracks dynamics (${quiet.toFixed(2)} -> ${loud.toFixed(2)})`);
  let previousEnd = 0;
  for (const s of variable.sections) { assert(s.startMs >= previousEnd - 1 && s.endMs > s.startMs && s.confidence >= 0 && s.confidence <= 1); previousEnd = s.endMs; }
  assert(variable.sections.length >= 2);
  console.log(`PASS variable energy: ${variable.sections.map(s => s.type).join('/')} ordered, within bounds`);

  // ---- summaries, cue suggestions
  const summaries = (await api('/api/plus/analysis/musical/summaries')).body;
  assert.equal(Object.keys(summaries.summaries).length, 8); assert.equal(summaries.summaries[ids.minor_f].camelot, '4A');
  assert(!('beatsMs' in summaries.summaries[ids.click120]));
  const cues = (await api(`/api/plus/analysis/tracks/${ids.variable_energy}/cue-suggestions`)).body;
  assert.equal(cues.autoApply, false); assert.equal(cues.analysisState, 'ready'); assert(cues.inputs.musical && cues.inputs.energy);
  for (const s of cues.suggestions) assert(s.startMs >= 0 && s.endMs > s.startMs && s.endMs <= 40500 && s.reason && s.basis.length);
  const only = (await api(`/api/plus/analysis/tracks/${ids.variable_energy}/cue-suggestions?intents=strong-start,bogus`)).body;
  assert(only.suggestions.every(s => s.intent === 'strong-start')); assert(only.unavailable.some(u => u.intent === 'bogus'));
  console.log(`PASS cue suggestions are proposals only: ${cues.suggestions.map(s => s.intent).join(', ') || 'none'}`);

  // ---- musical analysis never touched project, cues, or the basic analysis record
  assert.deepEqual((await api('/api/project')).body, beforeProject); assert.deepEqual((await api('/api/cues')).body, beforeCues);
  assert.equal((await api(`/api/plus/analysis/tracks/${ids.click120}`)).status, 404, 'musical results do not create the loudness record');

  // ---- manual corrections (Phase 2.5): detected, corrected and effective values coexist
  const correct = (id, body) => api(`/api/plus/analysis/tracks/${id}/musical/corrections`, body);
  assert.equal((await correct(ids.click120, { set: { bpm: { mode: 'factor', value: 2 } } })).body.analysis.effective.bpm.toFixed(0), '240');
  let fixed = (await correct(ids.click120, { set: { bpm: { mode: 'factor', value: 2 } } })).body.analysis;
  assert(Math.abs(fixed.bpm - 120) < 1 && Math.abs(fixed.effective.bpm - 2 * fixed.bpm) < 1e-6, 'detected BPM is untouched, effective is doubled');
  assert.equal(fixed.effective.bpmSource, 'factor'); assert.equal(fixed.effective.gridSource, 'corrected'); assert.equal(fixed.beatsMs.length, a.beatsMs.length, 'detected beats are unchanged');
  fixed = (await correct(ids.click120, { reset: 'bpm' })).body.analysis;
  assert.equal(fixed.effective.bpmSource, 'detected');
  fixed = (await correct(ids.minor_f, { set: { key: 'Db', scale: 'major' } })).body.analysis;
  assert.deepEqual([fixed.key, fixed.scale, fixed.camelot], ['F', 'minor', '4A']);
  assert.deepEqual([fixed.effective.key, fixed.effective.scale, fixed.effective.camelot, fixed.effective.keySource], ['C#', 'major', '3B', 'corrected']);
  fixed = (await correct(ids.click120, { set: { firstBeatMs: 300 }, nudgeGridMs: 10 })).body.analysis;
  assert.equal(fixed.corrections.gridOffsetMs, 10); assert.equal(fixed.effective.gridSource, 'corrected');
  assert(Math.abs(fixed.effective.beatGrid.firstBeatMs - 310) < 1e-6 || Math.abs(fixed.effective.beatGrid.firstBeatMs - 310 + 500) < 1e-6);
  assert.equal((await correct(ids.click120, { set: { bpm: { mode: 'custom', value: 5 } } })).status, 400);
  assert.equal((await correct(ids.click120, { set: { key: 'H' } })).status, 400);
  assert.equal((await correct(ids.click120, { set: { nonsense: 1 } })).status, 400);
  assert.equal((await correct(ids.click120, { reset: 'everything' })).status, 400);
  assert.equal((await correct(`track_${'0'.repeat(64)}`, { reset: 'all' })).status, 404);
  const summaryNow = (await api('/api/plus/analysis/musical/summaries')).body.summaries;
  assert.equal(summaryNow[ids.minor_f].effectiveCamelot, '3B'); assert.equal(summaryNow[ids.minor_f].camelot, '4A'); assert.equal(summaryNow[ids.minor_f].corrected, true);
  const cueFromCorrected = (await api(`/api/plus/analysis/tracks/${ids.variable_energy}/cue-suggestions`)).body;
  assert.equal(cueFromCorrected.analysisState, 'ready');
  // re-analysis of the same audio keeps the operator's work
  await api(`/api/plus/analysis/tracks/${ids.minor_f}/musical`, { force: true });
  await until(async () => { const r = (await musical(ids.minor_f)).body; return r.job && r.job.status === 'completed' && r.analysis.corrections.key === 'C#' ? r : null; }, 'reanalysis keeps corrections');
  await correct(ids.minor_f, { reset: 'all' }); await correct(ids.click120, { reset: 'all' });
  assert.equal((await musical(ids.minor_f)).body.analysis.effective.corrected, false);
  console.log('PASS corrections: x2, key/scale override with Camelot recalculated, grid first beat + nudge, validation errors, kept across re-analysis, reset');

  // ---- controlled warm-up is explicit, idempotent and reports its state
  const warm = (await api('/api/plus/analysis/musical/warmup', {})).body;
  assert(['idle', 'warming', 'warm'].includes(warm.state), JSON.stringify(warm));
  console.log(`PASS warm-up request accepted (state ${warm.state})`);

  // ---- real-music validation runner (here over the synthetic fixtures: the folder is read in place)
  assert.equal((await api('/api/plus/validation/runs', { folder: path.join(root, 'plus') })).status, 403, 'folders outside the fs jail are refused');
  assert.equal((await api('/api/plus/validation/runs', { folder: path.join(inputs, 'nope') })).status, 400);
  const started = await api('/api/plus/validation/runs', { folder: inputs, recursive: false });
  assert.equal(started.status, 202); const runId = started.body.id;
  assert.equal((await api('/api/plus/validation/runs', { folder: inputs })).status, 409, 'one run at a time');
  const finishedRun = await until(async () => { const r = (await api(`/api/plus/validation/runs/${runId}`)).body; return ['completed', 'failed', 'cancelled'].includes(r.status) ? r : null; }, 'validation run');
  assert.equal(finishedRun.status, 'completed'); assert.equal(finishedRun.total, 9); assert.equal(finishedRun.done, 9);
  const byName = Object.fromEntries(finishedRun.entries.map(e => [e.filename, e]));
  assert(Math.abs(byName['click120.wav'].detectedBpm - 120) < 1 && byName['click120.wav'].analysisDurationMs > 0 && byName['click120.wav'].durationMs > 29000);
  assert.equal(byName['minor_f.wav'].detectedKey, 'F minor'); assert.equal(byName['minor_f.wav'].camelot, '4A');
  assert.equal(finishedRun.metrics.exactBpmAccuracy, null, 'no expectations -> no accuracy figures'); assert.equal(finishedRun.metrics.keyAccuracy, null);
  assert.equal((await api('/api/plus/library/tracks')).body.total, 8, 'validation imports nothing');
  const edit = (index, body) => api(`/api/plus/validation/runs/${runId}/entries/${index}`, body);
  const i120 = byName['click120.wav'].index, iF = byName['minor_f.wav'].index, i90 = byName['click90.wav'].index;
  assert.equal((await edit(i120, { expectedBpm: 2 })).status, 400); assert.equal((await edit(i120, { smartCueRating: 'Great' })).status, 400); assert.equal((await edit(i120, { expectedKey: 'H minor' })).status, 400);
  await edit(i120, { expectedBpm: 120, smartCueRating: 'Good', notes: 'click track' });
  await edit(i90, { expectedBpm: 180 });                     // octave-equivalent only
  const edited = await edit(iF, { expectedKey: 'Fm', expectedBpm: 120 });
  assert.equal(edited.body.entry.expectedKeyNormalized, 'F minor');
  const scored = (await api(`/api/plus/validation/runs/${runId}`)).body.metrics;
  assert.equal(scored.bpmSamples, 3); assert(Math.abs(scored.octaveEquivalentBpmAccuracy - 1) < 1e-9, JSON.stringify(scored));
  assert(scored.exactBpmAccuracy >= 1 / 3 - 1e-9 && scored.exactBpmAccuracy < 1); assert.equal(scored.keyAccuracy, 1); assert.equal(scored.camelotCompatibleAccuracy, 1);
  assert(scored.averageProcessingMs > 0); assert.equal(scored.smartCue.good, 1);
  const csv = await fetch(`http://127.0.0.1:${port}/api/plus/validation/runs/${runId}/export?format=csv`); const csvText = await csv.text();
  assert.match(csv.headers.get('content-type'), /text\/csv/); assert(csvText.startsWith('filename,duration,detectedBpm')); assert(csvText.includes('click track'));
  assert.equal(csvText.trim().split('\n').length, 10);
  const wrapped = (await api(`/api/plus/validation/runs/${runId}/export?format=json&wrap=1`)).body;
  assert.equal(wrapped.filename, `${runId}.json`); assert.equal(JSON.parse(wrapped.content).entries.length, 9);
  assert.equal((await api(`/api/plus/validation/runs/${runId}/export?format=xml`)).status, 400);
  assert.equal((await api('/api/plus/validation/runs/run_unknown')).status, 404);
  assert((await api('/api/plus/validation/runs')).body.runs.length >= 1);
  console.log(`PASS validation runner: ${finishedRun.total} files analysed in place, metrics only from operator expectations (exact ${scored.exactBpmAccuracy.toFixed(2)}, octave ${scored.octaveEquivalentBpmAccuracy.toFixed(2)}), CSV/JSON export`);

  // ---- cache / stale / persistence
  await stop();
  const file = path.join(root, 'plus', 'analysis', 'musical', `${ids.click120}.json`);
  const stored = JSON.parse(fs.readFileSync(file, 'utf8'));
  stored.sourceHash = 'sha256:' + 'f'.repeat(64); fs.writeFileSync(file, JSON.stringify(stored));
  fs.rmSync(path.join(root, 'plus', 'analysis', 'musical', 'index.json'));   // index is regenerable
  await start();
  const stale = (await musical(ids.click120)).body;
  assert.equal(stale.status, 'stale'); assert.equal(stale.analysis.bpm, stored.bpm); assert.equal(stale.analysis.stale, true);
  assert.equal((await musical(ids.minor_f)).body.status, 'ready', 'untouched analyses survive a restart');
  console.log('PASS persistence, regenerated index, and stale analysis kept (not deleted) when the source hash differs');

  // ---- batch modes + reanalyze
  const stalePass = (await api('/api/plus/analysis/musical', { mode: 'stale' })).body;
  assert.equal(stalePass.queued, 1);
  assert.equal((await ready(ids.click120)).analysis.stale, false);
  const missing = (await api('/api/plus/analysis/musical', { mode: 'missing' })).body;
  assert.equal(missing.queued, 0, 'Analyze Missing leaves finished tracks alone');
  assert.equal((await api('/api/plus/analysis/musical', { mode: 'bogus' })).status, 400);
  assert.equal((await api('/api/plus/analysis/musical', { mode: 'selected', track_ids: [] })).status, 400);
  console.log('PASS Analyze Stale / Analyze Missing / validation');

  // ---- cancellation: queue everything again, cancel at once
  const all = Object.values(ids);
  const requeued = (await api('/api/plus/analysis/musical', { mode: 'selected', track_ids: all, force: true })).body;
  assert.equal(requeued.queued, all.length);
  assert.equal((await api('/api/plus/analysis/cancel', {})).status, 400, 'cancel needs an explicit scope');
  await sleep(1500);
  const cancelled = (await api('/api/plus/analysis/cancel', { all: true })).body;
  assert(cancelled.cancelled >= 6, `cancelled ${cancelled.cancelled}`);
  await until(async () => { const p = (await api('/api/plus/analysis/musical/summaries')).body.progress; return p.counts.pending === 0 && p.counts.running === 0 ? p : null; }, 'queue drained');
  assert.equal((await api('/api/plus/analysis/musical/summaries')).body.progress.counts.cancelled >= 6, true);
  assert.equal((await musical(ids.minor_f)).body.status === 'ready', true, 'cancel keeps the previous result');
  console.log(`PASS Cancel Analysis: ${cancelled.cancelled} jobs cancelled, previous results kept`);

  // ---- worker crash: kill the worker mid-job; the server and the library keep working
  await api(`/api/plus/analysis/tracks/${ids.variable_energy}/musical`, { force: true });
  await sleep(2500);
  spawnSync('powershell', ['-NoProfile', '-Command', "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -like '*liveplay_mir_worker*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"]);
  const crashed = await until(async () => { const r = (await musical(ids.variable_energy)).body; return r.job && ['failed', 'completed'].includes(r.job.status) ? r : null; }, 'job after worker kill');
  assert.equal((await api('/api/plus/library/tracks')).status, 200, 'library keeps working after a worker crash');
  assert(Math.abs(crashed.analysis.bpm - 120) < 1, 'previous result survives a crash');
  if (crashed.job.status === 'failed') assert.match(crashed.job.error, /worker/);
  await api(`/api/plus/analysis/tracks/${ids.variable_energy}/musical`, { force: true });
  const recovered = await until(async () => { const r = (await musical(ids.variable_energy)).body; return r.job && r.job.status === 'completed' ? r : null; }, 'worker restarts after crash');
  assert.equal(recovered.status, 'ready');
  console.log(`PASS worker crash isolated (${crashed.job.status}), a fresh worker serves the next job`);
  await stop();

  // ---- missing provider: everything else keeps working
  await start({ LIVEPLAY_MIR_PYTHON: path.join(root, 'no-such-python.exe') });
  caps = (await api('/api/plus/capabilities')).body;
  assert.equal(caps.musicalAnalysis.available, false);
  for (const key of ['bpm', 'beats', 'beatgrid', 'downbeats', 'keyDetection', 'camelot', 'energyAnalysis', 'structureAnalysis']) assert.equal(caps.musicalAnalysis[key], false, key);
  assert(caps.musicalAnalysis.unavailableReason);
  assert.equal(caps.library, true);
  assert.equal((await api(`/api/plus/analysis/tracks/${ids.click120}/musical`, {})).status, 503);
  const kept = (await musical(ids.minor_f)).body;
  assert.equal(kept.status, 'stale' === kept.status ? 'stale' : 'ready'); assert(kept.analysis, 'stored analysis is still readable without the provider');
  assert.equal((await api('/api/plus/library/tracks')).body.total, 8);
  assert.equal(caps.musicalAnalysis.smartCueV1, caps.plus.waveform, 'Smart Cue only claims what its inputs allow');
  console.log('PASS missing provider: capabilities false, 503 on analyze, stored data and library unaffected');
  await stop();
}
main().then(() => { console.log('musical analysis E2E passed'); }).catch(e => { console.error(e); process.exitCode = 1; })
  .finally(async () => { await stop(); try { fs.rmSync(root, { recursive: true, force: true }); } catch {} });
