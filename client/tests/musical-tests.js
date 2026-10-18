const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const Module = require('node:module');
const filename = path.resolve(__dirname, '../app/utils/musical.ts');
const source = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022}}).outputText;
const moduleInstance = new Module(filename); moduleInstance._compile(source, filename);
const m = moduleInstance.exports;

// ---- formatting
assert.equal(m.formatBpm(126.449), '126.4'); assert.equal(m.formatBpm(120), '120.0'); assert.equal(m.formatBpm(null), '—'); assert.equal(m.formatBpm(NaN), '—');
assert.equal(m.formatKey({key: 'F', scale: 'minor'}), 'F minor'); assert.equal(m.formatKey({key: null, scale: null}), '—'); assert.equal(m.formatKey(null), '—');
assert.deepEqual([0, 0.01, 0.2, 0.5, 0.79, 0.8, 1, null, NaN].map(m.energyLevel), [0, 1, 2, 3, 4, 5, 5, 0, 0]);
assert.equal(m.formatEnergy(0.824), '82%'); assert.equal(m.formatEnergy(null), '—');
assert.equal(m.formatClock(0), '0:00'); assert.equal(m.formatClock(42000), '0:42'); assert.equal(m.formatClock(128000), '2:08'); assert.equal(m.formatRange(38000, 72000), '0:38 – 1:12');
assert.deepEqual([0.96, 0.75, 0.6, 0.2, null].map(m.confidenceLabel), ['high', 'high', 'medium', 'low', null]);
assert.equal(m.camelotColor('8B') === m.camelotColor('8A'), false); assert.equal(m.camelotColor('nope'), 'transparent'); assert.equal(m.camelotColor(null), 'transparent');
assert.equal(m.sectionColor('drop') !== m.sectionColor('intro'), true); assert.equal(m.sectionColor('made-up'), m.sectionColor('unknown'));

// ---- status derivation (job beats stored state; stale detected by hash; failed only when nothing is stored)
const summary = (extra = {}) => ({sourceHash: 'sha256:a', stale: false, ...extra});
assert.equal(m.analysisStatus({available: true}), 'absent');
assert.equal(m.analysisStatus({available: false}), 'unavailable');
assert.equal(m.analysisStatus({available: true, summary: summary(), trackHash: 'sha256:a'}), 'ready');
assert.equal(m.analysisStatus({available: true, summary: summary(), trackHash: 'sha256:b'}), 'stale');
assert.equal(m.analysisStatus({available: true, summary: summary({stale: true}), trackHash: 'sha256:a'}), 'stale');
assert.equal(m.analysisStatus({available: true, summary: summary(), trackHash: 'sha256:a', job: {status: 'running'}}), 'running');
assert.equal(m.analysisStatus({available: true, summary: summary(), job: {status: 'pending'}}), 'queued');
assert.equal(m.analysisStatus({available: true, job: {status: 'failed'}}), 'failed');
assert.equal(m.analysisStatus({available: true, summary: summary(), trackHash: 'sha256:a', job: {status: 'failed'}}), 'ready');
assert.equal(m.analysisStatus({available: true, summary: summary(), job: {status: 'cancelled'}}), 'ready');
assert.deepEqual(['absent', 'stale', 'failed', 'ready', 'running', 'queued', 'unavailable'].map(m.needsAnalysis), [true, true, true, false, false, false, false]);

// ---- marker selection: viewport + thinning, bounded no matter how long the track is
const beats = Array.from({length: 10000}, (_, i) => i * 500);
assert.deepEqual(m.visibleMarkers(beats, 1000, 3000, 100), [2, 3, 4, 5, 6]);
assert.deepEqual(m.visibleMarkers(beats, 1000, 3000, 100).map(i => beats[i]), [1000, 1500, 2000, 2500, 3000]);
assert.equal(m.visibleMarkers(beats, 0, 5_000_000, 200).length <= 200, true);
const thinned = m.visibleMarkers(beats, 0, 5_000_000, 200);
assert.equal(thinned[0], 0); assert.equal(thinned.every((v, i) => i === 0 || v > thinned[i - 1]), true);
assert.deepEqual(m.visibleMarkers([], 0, 1000, 10), []); assert.deepEqual(m.visibleMarkers(beats, 1000, 1000, 10), []); assert.deepEqual(m.visibleMarkers(beats, 6_000_000, 7_000_000, 10), []);

// ---- overlay drawing: one stroke per layer regardless of beat count
function recorder() {
  const calls = {stroke: 0, moveTo: 0, fillRect: 0, beginPath: 0};
  return {calls, ctx: {fillStyle: '', strokeStyle: '', lineWidth: 1, globalAlpha: 1,
    fillRect() { calls.fillRect++; }, beginPath() { calls.beginPath++; }, moveTo() { calls.moveTo++; }, lineTo() {}, stroke() { calls.stroke++; }}};
}
const analysis = {beatsMs: beats, downbeatsMs: beats.filter((_, i) => i % 4 === 0),
  sections: [{type: 'intro', startMs: 0, endMs: 1_000_000}, {type: 'drop', startMs: 1_000_000, endMs: 3_000_000}]};
let r = recorder();
let drawn = m.drawMusicalOverlays(r.ctx, 900, 180, 5_000_000, analysis, {beats: true, downbeats: true, sections: true, suggestion: {startMs: 100, endMs: 2000}});
assert.equal(r.calls.stroke, 2, 'one stroke for all beats, one for all downbeats');
assert.equal(drawn.beats <= 225 && drawn.beats > 50, true, `beats thinned to canvas width (${drawn.beats})`);
assert.equal(drawn.sections, 2);
assert.equal(r.calls.moveTo, drawn.beats + drawn.downbeats);
r = recorder(); drawn = m.drawMusicalOverlays(r.ctx, 900, 180, 5_000_000, analysis, {beats: false, downbeats: false, sections: false});
assert.deepEqual(drawn, {beats: 0, downbeats: 0, sections: 0}); assert.equal(r.calls.stroke + r.calls.fillRect + r.calls.moveTo, 0, 'every overlay can be turned off');
r = recorder(); drawn = m.drawMusicalOverlays(r.ctx, 900, 180, 5_000_000, {beatsMs: [], downbeatsMs: [], sections: []}, {beats: true, downbeats: true, sections: true});
assert.equal(r.calls.stroke, 1, 'no downbeats available -> nothing is drawn for them'); assert.equal(drawn.downbeats, 0);
assert.deepEqual(m.drawMusicalOverlays(recorder().ctx, 900, 180, 0, analysis, {beats: true, downbeats: true, sections: true}), {beats: 0, downbeats: 0, sections: 0});
assert.deepEqual(m.drawMusicalOverlays(recorder().ctx, 900, 180, 1000, null, {beats: true, downbeats: true, sections: true}), {beats: 0, downbeats: 0, sections: 0});
console.log('PASS Musical analysis formatting, status derivation, marker thinning and bounded overlay drawing');

// ---- Phase 2.5: Camelot preview and effective beats
assert.equal(m.camelotFor('F', 'minor'), '4A'); assert.equal(m.camelotFor('C', 'major'), '8B'); assert.equal(m.camelotFor('C#', 'minor'), '12A'); assert.equal(m.camelotFor('B', 'major'), '1B');
assert.equal(m.camelotFor('H', 'major'), null); assert.equal(m.camelotFor('C', null), null); assert.equal(m.camelotFor(null, 'minor'), null);
const names = m.PITCH_NAMES; const maj = [8,3,10,5,12,7,2,9,4,11,6,1], min = [5,12,7,2,9,4,11,6,1,8,3,10];
names.forEach((n, i) => { assert.equal(m.camelotFor(n, 'major'), maj[i] + 'B'); assert.equal(m.camelotFor(n, 'minor'), min[i] + 'A'); });
const detected = {beatsMs: [100, 600, 1100], durationMs: 3000, effective: {gridSource: 'detected', beatGrid: {firstBeatMs: 100, intervalMs: 500}}};
assert.deepEqual(m.effectiveBeatsMs(detected), [100, 600, 1100], 'uncorrected grid keeps the detected beats');
const corrected = {beatsMs: [100, 600], durationMs: 2000, effective: {gridSource: 'corrected', beatGrid: {firstBeatMs: 50, intervalMs: 500}}};
assert.deepEqual(m.effectiveBeatsMs(corrected), [50, 550, 1050, 1550], 'corrected grid is regenerated from first beat and interval');
assert.deepEqual(m.effectiveBeatsMs(null), []); assert.equal(m.effectiveBeatsMs({beatsMs: [1], durationMs: 10, effective: null}).length, 1);
console.log('PASS Camelot preview matches the server wheel; effective beats follow the corrected grid');
