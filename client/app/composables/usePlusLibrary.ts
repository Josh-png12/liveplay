import { ref, onBeforeUnmount } from 'vue';
import type { PlusTrack, PlusBatch } from '~/types/plus';
export function usePlusLibrary() {
  const server = useLiveplayServer();
  const { t } = useLocalization();
  const tracks = ref<PlusTrack[]>([]), batches = ref<PlusBatch[]>([]);
  const capabilities = ref<any>(null), error = ref(''), uploading = ref(false), loading = ref(false);
  const maintenance = ref<any>(null), recovery = ref<any>(null);
  const uploads = new Map<string, File[]>();
  const localFailures = ref<{name: string; error: string}[]>([]);
  const controllers = new Set<AbortController>();
  let disposed = false, timer: ReturnType<typeof setTimeout>;
  const key = () => `liveplay.plus.batches:${server.serverUrl}`;
  async function request<T = any>(path: string, init: RequestInit = {}): Promise<T> {
    const controller = new AbortController(); controllers.add(controller);
    try { return await server.plusRequest<T>(`/api/plus${path}`, { ...init, signal: controller.signal }); }
    finally { controllers.delete(controller); }
  }
  const post = (path: string, body: any) => request(path, { method: 'POST', body: JSON.stringify(body) });
  function remember() { try { localStorage.setItem(key(), JSON.stringify(batches.value.map(b => b.id).slice(-64))); } catch {} }
  async function refresh() {
    if (loading.value) return;
    loading.value = true;
    try {
      capabilities.value = await request('/capabilities');
      if (!capabilities.value.library) { recovery.value = await request('/library/recovery'); return; }
      const loaded: PlusTrack[] = [];
      for (let offset = 0; !disposed; offset += 500) {
        const page = await request<{tracks: PlusTrack[]; total: number}>(`/library/tracks?offset=${offset}&limit=500`);
        loaded.push(...page.tracks);
        if (offset + page.tracks.length >= page.total) break;
      }
      tracks.value = loaded; error.value = '';
    } catch (e) { if (!disposed) error.value = String(e); }
    finally { loading.value = false; }
  }
  async function poll() {
    try {
      let changed = false;
      for (let i = 0; i < batches.value.length && !disposed; i++) {
        const batch = batches.value[i];
        if (!['queued', 'running'].includes(batch.status)) continue;
        try { batches.value[i] = await request<PlusBatch>(`/imports/${batch.id}`); }
        catch (e) {
          if (String(e).includes('404')) { batch.status = 'expired'; changed = true; continue; }
          throw e;
        }
        if (!['queued', 'running'].includes(batches.value[i].status)) changed = true;
      }
      if (maintenance.value?.status === 'running') {
        maintenance.value = await request('/library/maintenance');
        if (maintenance.value.status !== 'running') changed = true;
      }
      if (changed) await refresh();
    } catch (e) { if (!disposed) error.value = String(e); }
    finally { if (!disposed) timer = setTimeout(poll, 750); }
  }
  async function initialize() {
    await refresh();
    try {
      const ids = JSON.parse(localStorage.getItem(key()) || '[]');
      for (const id of ids.slice(-64)) {
        try { batches.value.push(await request<PlusBatch>(`/imports/${encodeURIComponent(id)}`)); } catch {}
      }
    } catch {}
    if (!disposed) void poll();
  }
  async function importFiles(files: File[], relinkTrackId = '') {
    if (uploading.value) return;
    uploading.value = true; error.value = ''; localFailures.value = [];
    try {
      if (!capabilities.value?.library) throw new Error(t('plusLibrary.libraryUnavailable'));
      if (relinkTrackId && files.length !== 1) throw new Error(t('plusLibrary.singleRelink'));
      const limit = capabilities.value.maxAssetBytes || 256 * 1024 * 1024;
      // Bound multipart bodies including header overhead; requests yield to the UI.
      const groups: File[][] = []; let group: File[] = [], bytes = 0;
      for (const file of files) {
        if (!/\.(wav|mp3|flac|ogg|m4a|aac|aif|aiff|opus|wma)$/i.test(file.name) || file.size === 0 || file.size + 4096 > limit) {
          localFailures.value.push({name: file.name, error: t('plusLibrary.invalidFile')}); continue;
        }
        if (group.length >= 100 || bytes + file.size + 4096 > limit) { groups.push(group); group = []; bytes = 0; }
        group.push(file); bytes += file.size + 4096;
      }
      if (group.length) groups.push(group);
      for (const entries of groups) {
        if (disposed) break;
        const form = new FormData(); for (const file of entries) form.append('file', file, file.name);
        const suffix = relinkTrackId ? `?relinkTrackId=${encodeURIComponent(relinkTrackId)}` : '';
        const batch = await request<PlusBatch>(`/imports/upload${suffix}`, { method: 'POST', body: form });
        uploads.set(batch.id, entries); batches.value.push(batch); remember();
      }
    } catch (e) { if (!disposed) error.value = String(e); }
    finally { uploading.value = false; }
  }
  async function cancel(batch: PlusBatch) {
    try { const next = await post(`/imports/${batch.id}/cancel`, {}); Object.assign(batch, next); }
    catch (e) { error.value = String(e); }
  }
  async function retry(batch: PlusBatch) {
    const failed = batch.files.map((file, index) => ({ file, index })).filter(x => ['failed', 'cancelled'].includes(x.file.status));
    try {
      if (batch.origin === 'server_path') {
        const next = await post('/imports', { files: failed.map(x => x.file.source.path), relinkTrackId: batch.relink_track_id || '' }); batches.value.push(next); remember();
      } else {
        try { const next: any = await post(`/imports/${batch.id}/retry`, {}); batches.value.push(next); remember(); }
        catch {
          const files = uploads.get(batch.id);
          if (!files) throw new Error(t('plusLibrary.retryReselect'));
          await importFiles(failed.map(x => files[x.index]), batch.relink_track_id || '');
        }
      }
    } catch (e) { error.value = String(e); }
  }
  async function maintain(operation: string, assets: string[] = [], confirm = false) {
    try { maintenance.value = await post('/library/maintenance', { operation, assets, confirm }); }
    catch (e) { error.value = String(e); }
  }
  async function recover(action: string, backup = '') {
    try { await post('/library/recovery', { action, backup, confirm: true }); error.value = t('plusLibrary.recoverySaved'); }
    catch (e) { error.value = String(e); }
  }
  async function setPerformance(enabled: boolean) {
    try { const value: any = await post('/performance', { enabled }); if (capabilities.value) capabilities.value.performanceMode = value.enabled; }
    catch (e) { error.value = String(e); }
  }
  async function requestTrackWaveform(trackId: string) {
    return request(`/library/tracks/${encodeURIComponent(trackId)}/waveform`, { method: 'POST', body: '{}' });
  }
  async function getTrackWaveform(trackId: string) {
    return request<any>(`/library/tracks/${encodeURIComponent(trackId)}/waveform`);
  }
  async function updateTrackTags(trackId: string, tags: any) {
    const updated = await request<PlusTrack>(`/library/tracks/${encodeURIComponent(trackId)}/tags`, { method: 'PATCH', body: JSON.stringify(tags) });
    const index = tracks.value.findIndex(track => track.id === trackId); if (index >= 0) tracks.value[index] = updated;
    return updated;
  }
  async function recommend(requestBody: any) {
    return request<{ provider: string; tracks: PlusTrack[] }>('/recommendations', { method: 'POST', body: JSON.stringify(requestBody) });
  }
  onBeforeUnmount(() => { disposed = true; clearTimeout(timer); for (const c of controllers) c.abort(); });
  return { tracks, batches, capabilities, error, uploading, loading, maintenance, recovery, localFailures, initialize, refresh, importFiles, cancel, retry, maintain, recover, setPerformance, requestTrackWaveform, getTrackWaveform, updateTrackTags, recommend };
}
