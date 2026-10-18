import { onBeforeUnmount, ref } from 'vue';

export interface ValidationEntry {
  index: number; filename: string; path: string; status: 'pending' | 'done' | 'failed';
  durationMs?: number | null; detectedBpm?: number | null; bpmConfidence?: number | null; bpmConfidenceLabel?: string | null; bpmAmbiguous?: boolean;
  alternatives?: { bpm: number; relation: string }[]; detectedKey?: string | null; keyConfidence?: number | null; camelot?: string | null;
  energy?: number | null; sections?: { type: string; startMs: number; endMs: number }[]; analysisDurationMs?: number | null;
  smartCue?: { intent: string; startMs: number; endMs: number; confidence: number }[]; error: string;
  expectedBpm: number | null; expectedKey: string | null; notes: string; smartCueRating: 'Good' | 'Acceptable' | 'Bad' | null;
}
export interface ValidationMetrics {
  analysed: number; failures: number; failureList: { filename: string; error: string }[]; bpmSamples: number;
  exactBpmAccuracy: number | null; octaveEquivalentBpmAccuracy: number | null; medianBpmError: number | null; medianBpmErrorOctaveAdjusted: number | null;
  keySamples: number; keyAccuracy: number | null; camelotCompatibleAccuracy: number | null;
  averageProcessingMs: number | null; averageWarmProcessingMs: number | null;
  smartCue: { good: number; acceptable: number; bad: number; unrated: number; goodRate: number | null; goodOrAcceptableRate: number | null };
}
export interface ValidationRun {
  id: string; folder: string; recursive: boolean; status: string; total: number; done: number; error?: string;
  entries: ValidationEntry[]; metrics?: ValidationMetrics; createdAt: string;
}

/** Client for the Diagnostics "real music validation" runner. Reads a folder in place; imports nothing. */
export function usePlusValidation() {
  const server = useLiveplayServer();
  const run = ref<ValidationRun | null>(null), error = ref(''), starting = ref(false);
  let timer: ReturnType<typeof setTimeout> | undefined, disposed = false;
  const request = <T = any>(path: string, init: RequestInit = {}) => server.plusRequest<T>(`/api/plus/validation${path}`, init);

  async function refresh() {
    if (!run.value) return;
    try { run.value = await request<ValidationRun>(`/runs/${run.value.id}`); error.value = ''; }
    catch (e) { error.value = String(e); }
  }
  async function poll() {
    clearTimeout(timer);
    await refresh();
    if (!disposed && run.value && ['queued', 'running'].includes(run.value.status)) timer = setTimeout(poll, 1000);
  }
  async function loadLatest() {
    try { const list = await request<{ runs: { id: string }[] }>('/runs'); if (list.runs.length) { run.value = await request<ValidationRun>(`/runs/${list.runs[0].id}`); void poll(); } }
    catch (e) { error.value = String(e); }
  }
  async function start(folder: string, recursive: boolean) {
    starting.value = true; error.value = '';
    try { run.value = await request<ValidationRun>('/runs', { method: 'POST', body: JSON.stringify({ folder, recursive }) }); void poll(); }
    catch (e) { error.value = String(e); }
    finally { starting.value = false; }
  }
  async function cancel() { if (run.value) { try { await request(`/runs/${run.value.id}/cancel`, { method: 'POST', body: '{}' }); await poll(); } catch (e) { error.value = String(e); } } }
  async function edit(index: number, fields: Partial<Pick<ValidationEntry, 'expectedBpm' | 'expectedKey' | 'notes' | 'smartCueRating'>>) {
    if (!run.value) return;
    try {
      const result = await request<{ entry: ValidationEntry; metrics: ValidationMetrics }>(`/runs/${run.value.id}/entries/${index}`, { method: 'POST', body: JSON.stringify(fields) });
      run.value.entries[index] = { ...run.value.entries[index], ...result.entry }; run.value.metrics = result.metrics; error.value = '';
    } catch (e) { error.value = String(e); await refresh(); }
  }
  async function download(format: 'json' | 'csv') {
    if (!run.value) return;
    try {
      const file = await request<{ filename: string; mime: string; content: string }>(`/runs/${run.value.id}/export?format=${format}&wrap=1`);
      const url = URL.createObjectURL(new Blob([file.content], { type: `${file.mime};charset=utf-8` }));
      const link = document.createElement('a'); link.href = url; link.download = file.filename; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (e) { error.value = String(e); }
  }
  onBeforeUnmount(() => { disposed = true; clearTimeout(timer); });
  return { run, error, starting, loadLatest, start, cancel, edit, download };
}
