// Single source of truth for how track time maps onto a waveform canvas.
// Everything drawn on the canvas (peaks, IN/OUT, playhead, beats, downbeats,
// sections, suggestion highlight) and every click on it goes through these two
// functions, so zoom and horizontal scroll can never make them disagree.

export interface WaveformView {
  durationMs: number;
  widthPx: number;
  zoom: number;       // >= 1; 1 shows the whole track
  scrollMs: number;   // time at the left edge, clamped to [0, durationMs - spanMs]
}
export const MAX_ZOOM = 64;

export function makeView(durationMs: number, widthPx: number, zoom = 1, scrollMs = 0): WaveformView {
  const duration = Number.isFinite(durationMs) && durationMs > 0 ? durationMs : 0;
  const z = Math.min(MAX_ZOOM, Math.max(1, Number.isFinite(zoom) ? zoom : 1));
  const span = duration / z;
  const scroll = Math.min(Math.max(0, Number.isFinite(scrollMs) ? scrollMs : 0), Math.max(0, duration - span));
  return { durationMs: duration, widthPx: Math.max(1, widthPx), zoom: z, scrollMs: scroll };
}
export const viewSpanMs = (v: WaveformView): number => v.durationMs / v.zoom;
export const visibleRangeMs = (v: WaveformView): { fromMs: number; toMs: number } => ({ fromMs: v.scrollMs, toMs: v.scrollMs + viewSpanMs(v) });

/** Canvas x (in canvas pixels) of a track time. Not clamped: callers cull what is off-screen. */
export function timeMsToCanvasX(v: WaveformView, ms: number): number {
  const span = viewSpanMs(v);
  return span > 0 ? ((ms - v.scrollMs) / span) * v.widthPx : 0;
}
/** Track time under a canvas x, clamped to the track. Exact inverse of timeMsToCanvasX inside the track. */
export function canvasXToTimeMs(v: WaveformView, x: number): number {
  const ms = v.scrollMs + (x / v.widthPx) * viewSpanMs(v);
  return Math.min(v.durationMs, Math.max(0, ms));
}
/** Scroll position that centres `ms` (clamped), for "follow playhead" and zoom-around-cursor. */
export function scrollToCenter(v: WaveformView, ms: number): number {
  return makeView(v.durationMs, v.widthPx, v.zoom, ms - viewSpanMs(v) / 2).scrollMs;
}
/** Zoom while keeping the time under `anchorX` fixed on screen. */
export function zoomAround(v: WaveformView, zoom: number, anchorX: number): WaveformView {
  const anchorMs = canvasXToTimeMs(v, anchorX);
  const next = makeView(v.durationMs, v.widthPx, zoom, 0);
  return makeView(v.durationMs, v.widthPx, next.zoom, anchorMs - (anchorX / v.widthPx) * viewSpanMs(next));
}
/** Peak-bucket index range [first, last] covered by canvas column `x`, so no peak is skipped when zoomed out. */
export function bucketRangeForColumn(v: WaveformView, bucketCount: number, x: number): [number, number] {
  if (bucketCount <= 0 || v.durationMs <= 0) return [0, -1];
  const a = canvasXToTimeMs(v, x), b = canvasXToTimeMs(v, x + 1);
  const first = Math.min(bucketCount - 1, Math.floor((a / v.durationMs) * bucketCount));
  const last = Math.min(bucketCount - 1, Math.max(first, Math.ceil((b / v.durationMs) * bucketCount) - 1));
  return [first, last];
}
/** What the overlay renderer needs, derived from the same view. */
export const overlayView = (v: WaveformView) => ({ ...visibleRangeMs(v), toX: (ms: number) => timeMsToCanvasX(v, ms) });
