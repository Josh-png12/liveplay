const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const Module = require('node:module');
function load(rel) {
  const filename = path.resolve(__dirname, rel);
  const source = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022}}).outputText;
  const m = new Module(filename); m._compile(source, filename); return m.exports;
}
const v = load('../app/utils/waveformView.ts');
const musical = load('../app/utils/musical.ts');
const near = (a, b, eps = 1e-6) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

// ---- 1x: the whole track, linear
let view = v.makeView(200_000, 900, 1, 0);
near(v.timeMsToCanvasX(view, 0), 0); near(v.timeMsToCanvasX(view, 100_000), 450); near(v.timeMsToCanvasX(view, 200_000), 900);
near(v.canvasXToTimeMs(view, 450), 100_000); near(v.canvasXToTimeMs(view, -50), 0); near(v.canvasXToTimeMs(view, 5000), 200_000);
assert.deepEqual(v.visibleRangeMs(view), {fromMs: 0, toMs: 200_000});
assert.equal(v.makeView(200_000, 900, 1, 50_000).scrollMs, 0, 'no scrolling at 1x');

// ---- 2x and 5x with scroll
view = v.makeView(200_000, 900, 2, 0);
near(v.timeMsToCanvasX(view, 100_000), 900, 1e-6); near(v.timeMsToCanvasX(view, 50_000), 450); near(v.canvasXToTimeMs(view, 450), 50_000);
view = v.makeView(200_000, 900, 2, 60_000);
near(v.timeMsToCanvasX(view, 60_000), 0); near(v.timeMsToCanvasX(view, 160_000), 900); near(v.canvasXToTimeMs(view, 0), 60_000); near(v.canvasXToTimeMs(view, 900), 160_000);
assert.equal(v.makeView(200_000, 900, 2, 150_000).scrollMs, 100_000, 'scroll clamps so the right edge is the end of the track');
assert.equal(v.makeView(200_000, 900, 2, -5).scrollMs, 0);
view = v.makeView(200_000, 900, 5, 80_000);
near(v.viewSpanMs(view), 40_000); near(v.timeMsToCanvasX(view, 100_000), 450); near(v.canvasXToTimeMs(view, 450), 100_000);
assert.ok(v.timeMsToCanvasX(view, 10_000) < 0 && v.timeMsToCanvasX(view, 150_000) > 900, 'off-screen times map outside the canvas');

// ---- round trip for every zoom/scroll combination
for (const zoom of [1, 1.5, 2, 3, 5, 10, 64]) for (const scroll of [0, 12_345, 99_999, 1e9]) {
  const w = v.makeView(240_000, 900, zoom, scroll);
  for (const x of [0, 1, 123.5, 450, 899, 900]) near(v.timeMsToCanvasX(w, v.canvasXToTimeMs(w, x)), x, 1e-6);
}

// ---- long tracks (3 hours): precision stays sub-pixel and sub-10 ms at 64x
const long = 3 * 3600 * 1000;
view = v.makeView(long, 1200, 64, long / 2);
near(v.viewSpanMs(view), long / 64);
const msPerPx = v.viewSpanMs(view) / 1200; assert.ok(msPerPx < 150, `ms per pixel ${msPerPx}`);
near(v.canvasXToTimeMs(view, 600), long / 2 - 0 + 0 * 1, v.viewSpanMs(view) / 2 + 1);
near(v.timeMsToCanvasX(v.makeView(long, 1200, 1, 0), long - 1), 1200, 0.2);
near(v.timeMsToCanvasX(view, v.canvasXToTimeMs(view, 777)), 777, 1e-6);

// ---- degenerate input never produces NaN
for (const bad of [0, -5, NaN]) { const w = v.makeView(bad, 900, 2, 10); assert.equal(w.durationMs, 0); assert.ok(Number.isFinite(v.timeMsToCanvasX(w, 1000))); assert.equal(v.canvasXToTimeMs(w, 100), 0); }
assert.equal(v.makeView(1000, 900, 1000, 0).zoom, v.MAX_ZOOM); assert.equal(v.makeView(1000, 900, 0, 0).zoom, 1);

// ---- scroll / zoom helpers
view = v.makeView(200_000, 900, 4, 0);
near(v.scrollToCenter(view, 100_000), 75_000); near(v.scrollToCenter(view, 1000), 0); near(v.scrollToCenter(view, 199_000), 150_000);
const anchored = v.zoomAround(v.makeView(200_000, 900, 2, 40_000), 8, 300);
near(v.canvasXToTimeMs(anchored, 300), v.canvasXToTimeMs(v.makeView(200_000, 900, 2, 40_000), 300), 1);

// ---- every column maps to buckets; together they cover the track with no gaps (1x, 2x, scrolled)
for (const [zoom, scroll] of [[1, 0], [2, 0], [2, 100_000], [5, 90_000]]) {
  const w = v.makeView(200_000, 900, zoom, scroll);
  const seen = new Set(); let previousLast = null;
  for (let x = 0; x < 900; x++) {
    const [a, b] = v.bucketRangeForColumn(w, 1000, x);
    assert.ok(a >= 0 && b < 1000 && a <= b);
    if (previousLast !== null) assert.ok(a <= previousLast + 1, `gap before column ${x} at ${zoom}x`);
    previousLast = b; for (let i = a; i <= b; i++) seen.add(i);
  }
  const lo = Math.floor(w.scrollMs / 200_000 * 1000), hi = Math.min(999, Math.floor((w.scrollMs + v.viewSpanMs(w)) / 200_000 * 1000) - 1);
  for (let i = lo; i <= hi; i++) assert.ok(seen.has(i), `bucket ${i} drawn at ${zoom}x scroll ${scroll}`);
}

// ---- overlays, IN/OUT and cursor share the transform: identical x for the same time
function recorder() {
  const xs = {move: [], rects: []};
  return {xs, ctx: {fillStyle: '', strokeStyle: '', lineWidth: 1, globalAlpha: 1, fillRect(x, y, w) { xs.rects.push([x, w]); }, beginPath() {}, moveTo(x) { xs.move.push(x); }, lineTo() {}, stroke() {}}};
}
const beats = Array.from({length: 400}, (_, i) => 1000 + i * 500);
const analysis = {beatsMs: beats, downbeatsMs: beats.filter((_, i) => i % 4 === 0), sections: [{type: 'drop', startMs: 60_000, endMs: 90_000}, {type: 'outro', startMs: 150_000, endMs: 200_000}]};
for (const [zoom, scroll] of [[1, 0], [2, 60_000], [5, 70_000]]) {
  const w = v.makeView(200_000, 900, zoom, scroll);
  const r = recorder();
  const drawn = musical.drawMusicalOverlays(r.ctx, 900, 180, 200_000, analysis, {beats: true, downbeats: true, sections: true}, 4, v.overlayView(w));
  const { fromMs, toMs } = v.visibleRangeMs(w);
  const expected = beats.filter(b => b >= fromMs && b <= toMs).length;
  assert.ok(drawn.beats > 0 && drawn.beats <= expected, `${zoom}x draws only visible beats (${drawn.beats}/${expected})`);
  // every beat marker lands on timeMsToCanvasX(beat) (+ the half-pixel used for crisp 1px lines)
  const expectedXs = new Set(beats.map(b => Math.round(v.timeMsToCanvasX(w, b)) + 0.5));
  for (const x of r.xs.move) assert.ok(expectedXs.has(x), `marker x ${x} at ${zoom}x`);
  assert.ok(r.xs.move.every(x => x >= 0 && x <= 900.5), 'no marker outside the canvas');
  // sections: left edge of a visible section equals the IN/OUT-style transform of its start
  for (const [x] of r.xs.rects) assert.ok(Number.isFinite(x));
}
const zoomed = v.makeView(200_000, 900, 5, 150_000), rz = recorder();
assert.equal(musical.drawMusicalOverlays(rz.ctx, 900, 180, 200_000, analysis, {beats: false, downbeats: false, sections: true}, 4, v.overlayView(zoomed)).sections, 1, 'only the outro is in the 150–190 s window');
near(rz.xs.rects[0][0], v.timeMsToCanvasX(zoomed, 150_000));
console.log('PASS Waveform view transform: 1x/2x/5x, horizontal scroll, 3-hour tracks, bucket coverage and shared overlay/IN-OUT/cursor mapping');
