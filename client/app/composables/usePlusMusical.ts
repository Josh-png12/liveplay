import { ref, computed, onBeforeUnmount } from 'vue';
import type { CueSuggestion, JobState, MusicalAnalysis, MusicalSummary } from '~/utils/musical';

export interface CueSuggestionSet {
  trackId: string; provider: string; analysisState: string; autoApply: boolean;
  suggestions: CueSuggestion[]; unavailable: { intent: string; reason: string }[];
  inputs: Record<string, boolean>;
}
export interface MusicalProgress { pending: number; running: number; completed: number; failed: number; cancelled: number }

/**
 * Client side of the Phase 2 musical-analysis API. Analysis is always an explicit
 * operator action: nothing here starts on import or on opening the library. Polling
 * only runs while work is queued, so an idle library costs no requests.
 */
export function usePlusMusical() {
  const server = useLiveplayServer();
  const summaries = ref<Record<string, MusicalSummary>>({});
  const jobs = ref<Record<string, JobState>>({});
  const progress = ref<MusicalProgress>({ pending: 0, running: 0, completed: 0, failed: 0, cancelled: 0 });
  const available = ref(false), error = ref('');
  const controllers = new Set<AbortController>();
  let disposed = false, timer: ReturnType<typeof setTimeout> | undefined, polling = false;

  async function request<T = any>(path: string, init: RequestInit = {}): Promise<T> {
    const controller = new AbortController(); controllers.add(controller);
    try { return await server.plusRequest<T>(`/api/plus${path}`, { ...init, signal: controller.signal }); }
    finally { controllers.delete(controller); }
  }
  const post = (path: string, body: unknown) => request(path, { method: 'POST', body: JSON.stringify(body) });
  const active = computed(() => progress.value.pending + progress.value.running);

  async function refresh() {
    try {
      const result = await request<{ summaries: Record<string, MusicalSummary>; progress: { counts: MusicalProgress; jobs: Record<string, JobState> }; available: boolean }>('/analysis/musical/summaries');
      summaries.value = result.summaries; jobs.value = result.progress.jobs; progress.value = result.progress.counts; available.value = result.available;
      error.value = '';
    } catch (e) { if (!disposed) error.value = String(e); }
  }
  /** Keeps refreshing while there is queued/running work, then stops by itself. */
  async function poll() {
    if (polling || disposed) return;
    polling = true;
    try {
      do { await refresh(); if (active.value > 0) await new Promise(resolve => { timer = setTimeout(resolve, 1500); }); } while (active.value > 0 && !disposed);
    } finally { polling = false; }
  }
  async function analyze(trackIds: string[], force = false) {
    if (!trackIds.length) return null;
    try { const result: any = await post('/analysis/musical', { mode: 'selected', track_ids: trackIds, force }); void poll(); return result; }
    catch (e) { error.value = String(e); return null; }
  }
  async function analyzeMode(mode: 'missing' | 'stale') {
    try { const result: any = await post('/analysis/musical', { mode }); void poll(); return result; }
    catch (e) { error.value = String(e); return null; }
  }
  async function cancel(trackIds?: string[]) {
    try { const result: any = await post('/analysis/cancel', trackIds?.length ? { track_ids: trackIds } : { all: true }); await refresh(); return result.cancelled as number; }
    catch (e) { error.value = String(e); return 0; }
  }
  async function details(trackId: string) {
    return request<{ status: string; state: string; job: JobState | null; analysis: MusicalAnalysis | null; available: boolean }>(`/analysis/tracks/${encodeURIComponent(trackId)}/musical`);
  }
  /** Asks the server to load the analysis worker in the background (skipped while playback is protected). Best effort. */
  async function warmup() { try { await post('/analysis/musical/warmup', {}); } catch { /* the first analysis simply pays the cold start */ } }
  /** Sets/clears manual corrections. The detected analysis is never changed; the response carries `effective`. */
  async function correct(trackId: string, change: { set?: Record<string, unknown>; reset?: string | string[]; nudgeGridMs?: number }) {
    const result: any = await post(`/analysis/tracks/${encodeURIComponent(trackId)}/musical/corrections`, change);
    await refresh();
    return result.analysis as MusicalAnalysis;
  }
  async function suggestions(trackId: string, intents: string[] = []) {
    const query = intents.length ? `?intents=${encodeURIComponent(intents.join(','))}` : '';
    return request<CueSuggestionSet>(`/analysis/tracks/${encodeURIComponent(trackId)}/cue-suggestions${query}`);
  }
  onBeforeUnmount(() => { disposed = true; clearTimeout(timer); for (const c of controllers) c.abort(); });
  return { summaries, jobs, progress, active, available, error, refresh, poll, analyze, analyzeMode, cancel, details, correct, warmup, suggestions };
}
export type PlusMusical = ReturnType<typeof usePlusMusical>;
