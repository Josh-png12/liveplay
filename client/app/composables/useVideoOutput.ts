import { computed, ref } from 'vue';

const available = ref(false), avDriftMs = ref(0), maxDriftMs = ref(0), corrections = ref(0), error = ref(''), warning = ref(''), videoPositionSeconds = ref(0), videoErrors = ref(0);
let wired = false;

// Electron owns the secondary window. This composable only feeds it positions
// reported by the existing server-owned audio transport; it never emits audio.
export function useVideoOutput() {
  const api = () => import.meta.client ? (window as any).electronAPI?.videoOutput : null;
  async function open(options: { displayId?: number; fullscreen?: boolean } = {}) {
    const bridge = api(); if (!bridge) { error.value = 'Video output requires Electron'; return null; }
    const state = await bridge.open(options); available.value = !!state.available; return state;
  }
  async function play(sourcePath: string, positionSeconds: number, options: any = {}) {
    const bridge = api(); if (!bridge) return false; await open(options); const result = await bridge.command({ type: 'play', source: sourcePath, position: positionSeconds, ...options }); if (!result?.ok) error.value = result?.error || 'Video output failed'; return !!result?.ok;
  }
  async function sync(audioPositionSeconds: number) {
    const bridge = api(); if (!bridge || !available.value) return;
    const drift = (audioPositionSeconds - videoPositionSeconds.value) * 1000;
    avDriftMs.value = drift; maxDriftMs.value = Math.max(maxDriftMs.value, Math.abs(drift));
    if (Math.abs(drift) > 80) corrections.value++;
    await bridge.command({ type: 'sync', position: audioPositionSeconds });
  }
  async function idle(kind: 'black' | 'logo' | 'image' = 'black', source = '') { const bridge = api(); if (bridge) await bridge.command({ type: 'idle', kind, source }); }
  async function stop(transition: 'cut' | 'fade-out' | 'fade-to-black' = 'cut', durationMs = 350) { const bridge = api(); if (bridge) await bridge.command({ type: 'stop', transition, durationMs }); }
  async function identifyDisplays() { return api()?.identify?.() || []; }
  async function status() { return api()?.status?.() || null; }
  if (import.meta.client && !wired) { wired = true; api()?.onMetrics?.((metrics: any) => { videoPositionSeconds.value = Number(metrics.positionMs || 0) / 1000; if (metrics.videoError) { videoErrors.value++; error.value = metrics.videoError; } }); api()?.onStatus?.((state: any) => { available.value = !!state.available; warning.value = state.warning || ''; if (state.error) error.value = state.error; }); }
  return { available: computed(() => available.value), avDriftMs, maxDriftMs, corrections, error, warning, videoPositionSeconds, videoErrors, open, play, sync, idle, stop, identifyDisplays, status };
}
