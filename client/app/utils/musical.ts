// Pure helpers for the musical-analysis UI (no Vue, no DOM): formatting, status
// derivation and waveform-overlay geometry. Kept dependency-free so they can be
// unit-tested in plain Node (client/tests/musical-tests.js).

export type MusicalSectionType = 'intro' | 'verse' | 'chorus' | 'build' | 'drop' | 'break' | 'bridge' | 'outro' | 'unknown';
export interface MusicalSection { type: MusicalSectionType; startMs: number; endMs: number; confidence: number; energy?: number | null }
export interface MusicalSummary {
  trackId: string; provider: string; analyzedAt: string; sourceHash: string; stale: boolean;
  bpm: number | null; bpmConfidence: number | null; bpmConfidenceLabel: ConfidenceLabel | null; bpmAmbiguous: boolean;
  key: string | null; scale: 'major' | 'minor' | null; camelot: string | null; keyConfidence: number | null;
  energy: number | null; energyLevel: number | null; sectionCount: number; beatCount: number; hasDownbeats: boolean;
  // Phase 2.5: what consumers should use (detected value unless the operator corrected it).
  effectiveBpm?: number | null; effectiveKey?: string | null; effectiveScale?: 'major' | 'minor' | null;
  effectiveCamelot?: string | null; effectiveBpmConfidenceLabel?: ConfidenceLabel | null; corrected?: boolean;
}
export interface MusicalCorrections {
  bpm: { mode: 'factor' | 'custom'; value: number } | null;
  firstBeatMs: number | null; gridOffsetMs: number; key: string | null; scale: 'major' | 'minor' | null;
}
export interface EffectiveValues {
  bpm: number | null; bpmSource: 'detected' | 'factor' | 'custom'; bpmConfidence: number | null; bpmAmbiguous: boolean;
  beatGrid: BeatGrid | null; gridSource: 'detected' | 'corrected';
  key: string | null; scale: 'major' | 'minor' | null; camelot: string | null; keySource: 'detected' | 'corrected'; corrected: boolean;
}
export type ConfidenceLabel = 'high' | 'medium' | 'low';
export interface BeatGrid {
  firstBeatMs: number; intervalMs: number; tempo: number; confidence: number | null; constantTempo: boolean;
  segments: { startMs: number; endMs: number; bpm: number; beatCount?: number }[];
}
export interface MusicalAnalysis extends MusicalSummary {
  durationMs: number | null; providerVersion: string; warnings: string[];
  bpmAlternatives: { bpm: number; relation: string; support: number | null }[];
  beatsMs: number[]; downbeatsMs: number[]; downbeatsAvailable: boolean; beatGrid: BeatGrid | null;
  keyConfidenceLabel: ConfidenceLabel | null; energyConfidence: number | null;
  energyCurve: { hopMs: number; values: number[] } | null; sections: MusicalSection[];
  capabilities: Record<string, unknown>;
  corrections?: Partial<MusicalCorrections>; effective?: EffectiveValues | null;
}
export type AnalysisStatus = 'absent' | 'queued' | 'running' | 'ready' | 'stale' | 'failed' | 'unavailable';
export interface JobState { status: string; error?: string }
export interface CueSuggestion {
  intent: string; label: string; startMs: number; endMs: number; fadeInMs: number; fadeOutMs: number;
  confidence: number; confidenceLabel: ConfidenceLabel; reason: string; basis: string[];
}

export function confidenceLabel(value: number | null | undefined): ConfidenceLabel | null {
  if (value == null || !Number.isFinite(value)) return null;
  return value >= 0.75 ? 'high' : value >= 0.5 ? 'medium' : 'low';
}

/** "126.4" — one decimal, never more precision than the analysis has. */
export function formatBpm(bpm: number | null | undefined): string {
  return bpm == null || !Number.isFinite(bpm) ? '—' : (Math.round(bpm * 10) / 10).toFixed(1);
}
export function formatKey(summary: Pick<MusicalSummary, 'key' | 'scale'> | null | undefined): string {
  return summary?.key && summary.scale ? `${summary.key} ${summary.scale}` : '—';
}
/** Continuous 0..1 energy as the 1..5 level the UI shows next to the percentage. */
export function energyLevel(energy: number | null | undefined): number {
  if (energy == null || !Number.isFinite(energy) || energy <= 0) return 0;
  return Math.min(5, 1 + Math.floor(Math.min(energy, 0.9999) * 5));
}
export function formatEnergy(energy: number | null | undefined): string {
  return energy == null || !Number.isFinite(energy) ? '—' : `${Math.round(energy * 100)}%`;
}
export function formatClock(ms: number): string {
  const total = Math.max(0, Math.floor(ms / 1000));
  return `${Math.floor(total / 60)}:${(total % 60).toString().padStart(2, '0')}`;
}
export function formatRange(startMs: number, endMs: number): string { return `${formatClock(startMs)} – ${formatClock(endMs)}`; }

/**
 * One status for the table cell. A running/queued job wins (the UI shows progress),
 * then the stored analysis state; a failed job only matters when no data exists.
 */
export function analysisStatus(opts: {
  available: boolean; summary?: MusicalSummary | null; trackHash?: string; job?: JobState | null;
}): AnalysisStatus {
  const { summary, job } = opts;
  if (job?.status === 'running') return 'running';
  if (job?.status === 'pending') return 'queued';
  if (summary) return summary.stale || (opts.trackHash && summary.sourceHash && summary.sourceHash !== opts.trackHash) ? 'stale' : 'ready';
  if (job?.status === 'failed') return 'failed';
  return opts.available ? 'absent' : 'unavailable';
}
/** Whether a track qualifies for "Analyze Missing": nothing usable and not already in flight. */
export function needsAnalysis(status: AnalysisStatus): boolean { return status === 'absent' || status === 'stale' || status === 'failed'; }

export const camelotColor = (camelot: string | null | undefined): string => {
  const match = /^(\d{1,2})([AB])$/.exec(camelot || '');
  if (!match) return 'transparent';
  return `hsl(${((Number(match[1]) - 1) * 30) % 360} ${match[2] === 'A' ? 55 : 70}% ${match[2] === 'A' ? 38 : 46}%)`;
};

export const sectionColor = (type: MusicalSectionType): string => ({
  intro: '#4aa3df', verse: '#6fbf73', chorus: '#f0a43c', build: '#d9c34a', drop: '#e5565b',
  break: '#8d6fd1', bridge: '#43b9a8', outro: '#4a7fb0', unknown: '#7a7f87',
} as Record<string, string>)[type] || '#7a7f87';

/**
 * Times to draw as markers inside [fromMs, toMs]: sorted input is binary-searched, and
 * when more would fit than `maxMarkers` it is thinned evenly so a 60-minute track
 * costs the same to draw as a 3-minute one. Returns the indices, not copies.
 */
export function visibleMarkers(timesMs: readonly number[], fromMs: number, toMs: number, maxMarkers: number): number[] {
  if (!timesMs.length || toMs <= fromMs || maxMarkers <= 0) return [];
  let lo = 0, hi = timesMs.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (timesMs[mid] < fromMs) lo = mid + 1; else hi = mid; }
  const first = lo;
  hi = timesMs.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (timesMs[mid] <= toMs) lo = mid + 1; else hi = mid; }
  const count = lo - first;
  if (count <= 0) return [];
  const stride = Math.max(1, Math.ceil(count / maxMarkers));
  const result: number[] = [];
  for (let i = first; i < lo; i += stride) result.push(i);
  return result;
}

/** Visible time window plus the shared time->x transform (see waveformView.ts). */
export interface OverlayView { fromMs: number; toMs: number; toX: (ms: number) => number }
export interface OverlayOptions { beats: boolean; downbeats: boolean; sections: boolean; suggestion?: { startMs: number; endMs: number } | null }
export interface CanvasLike {
  fillStyle: unknown; strokeStyle: unknown; lineWidth: number; globalAlpha: number;
  fillRect(x: number, y: number, w: number, h: number): void;
  beginPath(): void; moveTo(x: number, y: number): void; lineTo(x: number, y: number): void; stroke(): void;
}

/**
 * Draws beat/downbeat ticks, section bands and the suggestion highlight onto a canvas.
 * All beats share ONE path and ONE stroke call (never per-marker DOM or per-marker
 * strokes), and markers closer than `minSpacingPx` are thinned, so the cost is bounded
 * by the canvas width, not the track length.
 */
export function drawMusicalOverlays(ctx: CanvasLike, width: number, height: number, durationMs: number,
                                    analysis: Pick<MusicalAnalysis, 'beatsMs' | 'downbeatsMs' | 'sections'> | null | undefined,
                                    options: OverlayOptions, minSpacingPx = 4, view?: OverlayView): { beats: number; downbeats: number; sections: number } {
  const drawn = { beats: 0, downbeats: 0, sections: 0 };
  if (!(durationMs > 0) || width <= 0) return drawn;
  const fromMs = view ? view.fromMs : 0, toMs = view ? view.toMs : durationMs;
  const x = view ? view.toX : (ms: number) => (ms / durationMs) * width;
  if (analysis && options.sections) {
    ctx.globalAlpha = 0.22;
    const inView = analysis.sections.filter(section => section.endMs >= fromMs && section.startMs <= toMs);
    for (const section of inView) {
      ctx.fillStyle = sectionColor(section.type);
      ctx.fillRect(x(section.startMs), 0, Math.max(1, x(section.endMs) - x(section.startMs)), height);
      drawn.sections++;
    }
    ctx.globalAlpha = 0.9;
    for (const section of inView) { if (section.startMs >= fromMs) { ctx.fillStyle = sectionColor(section.type); ctx.fillRect(x(section.startMs), 0, 2, height); } }
    ctx.globalAlpha = 1;
  }
  const maxMarkers = Math.max(1, Math.floor(width / minSpacingPx));
  if (analysis && options.beats) {
    ctx.strokeStyle = 'rgba(255,255,255,0.35)'; ctx.lineWidth = 1; ctx.beginPath();
    for (const i of visibleMarkers(analysis.beatsMs, fromMs, toMs, maxMarkers)) { const px = Math.round(x(analysis.beatsMs[i])) + 0.5; ctx.moveTo(px, height * 0.7); ctx.lineTo(px, height); drawn.beats++; }
    ctx.stroke();
  }
  if (analysis && options.downbeats && analysis.downbeatsMs.length) {
    ctx.strokeStyle = 'rgba(255,90,90,0.9)'; ctx.lineWidth = 1.5; ctx.beginPath();
    for (const i of visibleMarkers(analysis.downbeatsMs, fromMs, toMs, maxMarkers)) { const px = Math.round(x(analysis.downbeatsMs[i])) + 0.5; ctx.moveTo(px, 0); ctx.lineTo(px, height); drawn.downbeats++; }
    ctx.stroke();
  }
  if (options.suggestion) {
    ctx.globalAlpha = 0.25; ctx.fillStyle = '#7dff9b';
    ctx.fillRect(x(options.suggestion.startMs), 0, Math.max(2, x(options.suggestion.endMs) - x(options.suggestion.startMs)), height);
    ctx.globalAlpha = 1;
  }
  return drawn;
}

const CAMELOT_MAJOR = [8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1], CAMELOT_MINOR = [5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10];
export const PITCH_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'] as const;
/** Instant Camelot preview for the key editor; the server remains authoritative (it recomputes on save). */
export function camelotFor(key: string | null | undefined, scale: string | null | undefined): string | null {
  const pitch = PITCH_NAMES.indexOf((key || '') as typeof PITCH_NAMES[number]);
  if (pitch < 0 || (scale !== 'major' && scale !== 'minor')) return null;
  return scale === 'major' ? `${CAMELOT_MAJOR[pitch]}B` : `${CAMELOT_MINOR[pitch]}A`;
}
/** Beats to draw/use: the corrected constant grid when the operator changed it, otherwise the detected beats. */
export function effectiveBeatsMs(analysis: Pick<MusicalAnalysis, 'beatsMs' | 'effective' | 'durationMs'> | null | undefined): number[] {
  if (!analysis) return [];
  const grid = analysis.effective?.gridSource === 'corrected' ? analysis.effective.beatGrid : null;
  if (!grid || !(grid.intervalMs > 0)) return analysis.beatsMs;
  const end = analysis.durationMs ?? 0, beats: number[] = [];
  for (let t = grid.firstBeatMs; t <= end && beats.length < 32768; t += grid.intervalMs) beats.push(t);
  return beats;
}
/** Detected value, corrected value and the one in effect — the three things the editor shows. */
export const bpmSourceLabel = (e: EffectiveValues | null | undefined): 'detected' | 'factor' | 'custom' => e?.bpmSource ?? 'detected';
