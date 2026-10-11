<template>
  <section class="plus-library" role="dialog" aria-modal="true" :aria-label="t('plusLibrary.title')" @dragover.prevent.stop @drop.prevent.stop="dropFiles" @keydown.stop @keyup.stop @keydown.esc.prevent="$emit('close')">
    <header><h1>{{ t('plusLibrary.title') }}</h1><button @click="$emit('close')" :aria-label="t('plusLibrary.closeLabel')">{{ t('plusLibrary.close') }}</button></header>
    <p>{{ t('plusLibrary.description') }}</p>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <ul v-if="localFailures.length" role="alert"><li v-for="(file, index) in localFailures" :key="index" class="error">{{ file.name }}: {{ file.error }}</li></ul>
    <div v-if="capabilities && !capabilities.library" class="recovery">
      <h2>{{ t('plusLibrary.unavailable') }}</h2><p>{{ recovery?.error || capabilities.storageError }}</p>
      <label>{{ t('plusLibrary.backup') }} <select v-model="backup"><option value="">{{ t('plusLibrary.select') }}</option><option v-for="name in recovery?.backups || []" :key="name">{{ name }}</option></select></label>
      <label><input v-model="confirmedRecovery" type="checkbox"> {{ t('plusLibrary.confirmRecovery') }}</label>
      <button :disabled="!confirmedRecovery || !backup" @click="recover('restore', backup)">{{ t('plusLibrary.restoreBackup') }}</button>
      <button :disabled="!confirmedRecovery" @click="recover('reset')">{{ t('plusLibrary.emptyCatalog') }}</button>
    </div>
    <template v-else>
      <div class="toolbar">
        <button :disabled="uploading || !capabilities?.library" @click="relinkId = ''; fileInput?.click()">{{ t('plusLibrary.chooseFiles') }}</button>
        <input ref="fileInput" type="file" :multiple="!relinkId" accept=".wav,.mp3,.flac,.ogg,.m4a,.aac,.aif,.aiff,.opus,.wma" hidden @change="selectedFiles">
        <span role="status">{{ uploading ? t('plusLibrary.uploading') : t('plusLibrary.dropHint') }}</span>
        <button :disabled="loading" @click="refresh">{{ t('plusLibrary.refresh') }}</button>
        <button :disabled="maintenance?.status === 'running'" @click="maintain('verify')">{{ t('plusLibrary.verify') }}</button>
        <button :disabled="maintenance?.status === 'running'" @click="maintain('rebuild')">{{ t('plusLibrary.rebuild') }}</button>
        <label><input type="checkbox" :checked="capabilities?.performanceMode" @change="performanceChanged"> {{ t('plusLibrary.performanceMode') }}</label>
      </div>
      <div class="filters">
        <label>{{ t('plusLibrary.search') }} <input v-model="search" type="search" :placeholder="t('plusLibrary.searchPlaceholder')"></label>
        <label>{{ t('plusLibrary.format') }} <select v-model="format"><option value="">{{ t('plusLibrary.all') }}</option><option v-for="value in formats" :key="value">{{ value }}</option></select></label>
        <label>{{ t('plusLibrary.state') }} <select v-model="state"><option value="">{{ t('plusLibrary.all') }}</option><option value="available">{{ t('plusLibrary.available') }}</option><option value="missing">{{ t('plusLibrary.missing') }}</option><option value="corrupt">{{ t('plusLibrary.corrupt') }}</option></select></label>
        <span>{{ t('plusLibrary.trackCount', {count: filtered.length}) }}{{ loading ? ` · ${t('plusLibrary.loading')}` : '' }}</span>
      </div>
      <div class="table-scroll"><table>
        <thead><tr><th><input v-model="allVisibleSelected" type="checkbox" :aria-label="t('plusLibrary.selectAll')"></th><th v-for="column in columns" :key="column.key"><button @click="setSort(column.key)">{{ t(column.label) }} {{ sort === column.key ? (descending ? '↓' : '↑') : '' }}</button></th><th>{{ t('plusLibrary.actions') }}</th></tr></thead>
        <tbody><tr v-for="track in visibleTracks" :key="track.id">
          <td><input v-model="selectedTrackIds" :value="track.id" type="checkbox" :aria-label="track.title"></td>
          <td>{{ track.title }} <span v-if="duplicates.has(track.id)" class="duplicate">{{ t('plusLibrary.duplicate') }}</span></td>
          <td>{{ track.artist || '—' }}</td><td>{{ track.album || '—' }}</td>
          <td>{{ duration(track.duration_ms) }}</td><td>{{ track.format }}</td><td>{{ track.sample_rate || '—' }}</td><td>{{ track.channels || '—' }}</td><td>{{ size(track.size_bytes) }}</td>
          <td :class="{error: track.status === 'missing' || track.status === 'corrupt'}">{{ stateLabel(track.status || 'available') }}</td>
          <td><button @click="relinkId = track.id; fileInput?.click()">{{ t('plusLibrary.relink') }}</button></td>
        </tr></tbody>
      </table></div>
      <div class="pagination"><button :disabled="page === 0" @click="page--">{{ t('plusLibrary.previous') }}</button><span>{{ t('plusLibrary.page', {page: page + 1, pages: Math.max(1, Math.ceil(filtered.length / 100))}) }}</span><button :disabled="(page + 1) * 100 >= filtered.length" @click="page++">{{ t('plusLibrary.next') }}</button></div>
      <section :aria-label="t('plusLibrary.imports')"><h2>{{ t('plusLibrary.imports') }}</h2>
        <p v-if="!batches.length">{{ t('plusLibrary.noImports') }}</p>
        <details v-for="batch in batches" :key="batch.id">
          <summary>{{ batch.status }} · {{ t('plusLibrary.filesProgress', {done: completedFiles(batch), total: batch.files.length}) }} · {{ batch.id }}</summary>
          <progress :max="batch.files.length" :value="completedFiles(batch)" />
          <button v-if="['queued','running'].includes(batch.status)" :disabled="batch.cancel_requested" @click="cancel(batch)">{{ t('plusLibrary.cancel') }}</button>
          <button v-if="batch.files.some(f => ['failed','cancelled'].includes(f.status))" :disabled="uploading" @click="retry(batch)">{{ t('plusLibrary.retry') }}</button>
          <ul><li v-for="(file, index) in batch.files" :key="index"><strong>{{ file.source.name }}</strong> · {{ file.status }} <span class="error">{{ file.error }}</span></li></ul>
        </details>
      </section>
      <section v-if="maintenance" :aria-label="t('plusLibrary.maintenance')"><h2>{{ t('plusLibrary.maintenance') }}</h2><p>{{ t('plusLibrary.maintenanceStats', maintenance) }}</p>
        <ul><li v-for="entry in maintenance.errors || []" :key="entry.track_id || entry.asset || entry.error" class="error">{{ entry.error }}</li></ul>
        <div v-if="maintenance.orphans?.length"><h3>{{ t('plusLibrary.orphans') }}</h3>
          <label v-for="name in maintenance.orphans" :key="name"><input v-model="selectedOrphans" :value="name" type="checkbox"> {{ name }}</label>
          <label><input v-model="confirmedCleanup" type="checkbox"> {{ t('plusLibrary.confirmCleanup') }}</label>
          <button :disabled="!confirmedCleanup || !selectedOrphans.length || maintenance.status === 'running'" @click="cleanup">{{ t('plusLibrary.deleteSelected') }}</button>
      </div>
      <section class="loudness" :aria-label="t('plusLibrary.loudnessTitle')"><h2>{{ t('plusLibrary.loudnessTitle') }}</h2>
        <p>{{ t('plusLibrary.loudnessDescription') }}</p><div class="toolbar"><button :disabled="!selectedTrackIds.length" @click="analyzeLoudness">{{ t('plusLibrary.analyzeLoudness') }}</button><button :disabled="!selectedTrackIds.length" @click="prepareLoudness('preview')">{{ t('plusLibrary.previewGain') }}</button><button :disabled="!prepared.length" @click="prepareLoudness('apply')">{{ t('plusLibrary.applyRecommendedGain') }}</button><button :disabled="!selectedTrackIds.length" @click="prepareLoudness('reset')">{{ t('plusLibrary.resetGain') }}</button></div>
        <label>{{ t('plusLibrary.targetLufs') }} <input v-model.number="loudness.target_lufs" type="number" step="0.5"></label><label>{{ t('plusLibrary.maxPositiveGain') }} <input v-model.number="loudness.max_positive_gain_db" type="number" step="0.5"></label><label>{{ t('plusLibrary.maxNegativeGain') }} <input v-model.number="loudness.max_negative_gain_db" type="number" step="0.5"></label><label>{{ t('plusLibrary.truePeakCeiling') }} <input v-model.number="loudness.true_peak_ceiling_dbtp" type="number" step="0.5"></label>
        <table v-if="prepared.length"><thead><tr><th>{{ t('plusLibrary.titleColumn') }}</th><th>{{ t('plusLibrary.currentLufs') }}</th><th>{{ t('plusLibrary.recommendedGain') }}</th><th>{{ t('plusLibrary.estimatedPeak') }}</th></tr></thead><tbody><tr v-for="row in prepared" :key="row.track_id"><td>{{ trackName(row.track_id) }}</td><td>{{ row.current_lufs ?? '—' }}</td><td>{{ row.recommended_gain_db ?? row.status }}</td><td>{{ row.estimated_true_peak_dbtp ?? '—' }}</td></tr></tbody></table>
      </section>
      </section>
      <p class="analysis-info">{{ t('plusLibrary.analysisInfo') }}</p>
    </template>
  </section>
</template>
<script setup lang="ts">
import { computed, ref, watch, onMounted } from 'vue';
import type { PlusTrack } from '~/types/plus';
import { filterLibrary, completedFiles } from '~/utils/library';
defineEmits<{ (e: 'close'): void }>();
const { tracks, batches, capabilities, error, uploading, loading, maintenance, recovery, localFailures, initialize, refresh, importFiles, cancel, retry, maintain, recover, setPerformance } = usePlusLibrary();
const { t } = useLocalization();
const fileInput = ref<HTMLInputElement>();
const relinkId = ref(''), search = ref(''), format = ref(''), state = ref(''), sort = ref<keyof PlusTrack>('title'), descending = ref(false), page = ref(0);
const backup = ref(''), confirmedRecovery = ref(false), confirmedCleanup = ref(false), selectedOrphans = ref<string[]>([]);
const server = useLiveplayServer(); const selectedTrackIds = ref<string[]>([]), prepared = ref<any[]>([]);
const loudness = ref({ target_lufs: -12, max_positive_gain_db: 6, max_negative_gain_db: -12, true_peak_ceiling_dbtp: -1 });
const allVisibleSelected = computed({ get: () => visibleTracks.value.length > 0 && visibleTracks.value.every(track => selectedTrackIds.value.includes(track.id)), set: (checked: boolean) => { const visible = visibleTracks.value.map(track => track.id); selectedTrackIds.value = checked ? [...new Set([...selectedTrackIds.value, ...visible])] : selectedTrackIds.value.filter(id => !visible.includes(id)); } });
const columns: {key: keyof PlusTrack; label: string}[] = [
  {key:'title',label:'plusLibrary.titleColumn'}, {key:'artist',label:'plusLibrary.artist'}, {key:'album',label:'plusLibrary.album'}, {key:'duration_ms',label:'plusLibrary.duration'},
  {key:'format',label:'plusLibrary.format'}, {key:'sample_rate',label:'plusLibrary.sampleRate'}, {key:'channels',label:'plusLibrary.channels'}, {key:'size_bytes',label:'plusLibrary.size'}, {key:'status',label:'plusLibrary.state'}
];
const formats = computed(() => [...new Set(tracks.value.map(t => t.format))].sort());
const filtered = computed(() => filterLibrary(tracks.value, search.value, format.value, state.value, sort.value, descending.value));
const visibleTracks = computed(() => filtered.value.slice(page.value * 100, (page.value + 1) * 100));
const duplicates = computed(() => new Set(batches.value.flatMap(b => b.files.filter(f => f.status === 'duplicate').map(f => f.track_id))));
watch([search, format, state, sort, descending], () => { page.value = 0; });
watch(() => filtered.value.length, count => { page.value = Math.min(page.value, Math.max(0, Math.ceil(count / 100) - 1)); });
function setSort(key: keyof PlusTrack) { if (sort.value === key) descending.value = !descending.value; else { sort.value = key; descending.value = false; } }
function selectedFiles(event: Event) {
  const input = event.target as HTMLInputElement;
  const files = Array.from(input.files || []); input.value = '';
  if (files.length) void importFiles(files, relinkId.value);
  relinkId.value = '';
}
function dropFiles(event: DragEvent) { if (!uploading.value) void importFiles(Array.from(event.dataTransfer?.files || [])); }
function performanceChanged(event: Event) { void setPerformance((event.target as HTMLInputElement).checked); }
function cleanup() { void maintain('cleanup', [...selectedOrphans.value], true); selectedOrphans.value = []; confirmedCleanup.value = false; }
async function analyzeLoudness() { try { await Promise.all(selectedTrackIds.value.map(track_id => server.plusRequest('/api/plus/analysis/jobs', { method: 'POST', body: JSON.stringify({ track_id, provider: 'ffmpeg-basic' }) }))); } catch (e) { error.value = String(e); } }
async function prepareLoudness(action: 'preview'|'apply'|'reset') { try { const result: any = await server.plusRequest('/api/plus/loudness/prepare', { method: 'POST', body: JSON.stringify({ action, track_ids: selectedTrackIds.value, settings: loudness.value }) }); prepared.value = result.tracks || []; if (action !== 'preview') await refresh(); } catch (e) { error.value = String(e); } }
const trackName = (id: string) => tracks.value.find(track => track.id === id)?.title || id;
const duration = (ms: number) => `${Math.floor(ms / 60000)}:${Math.floor(ms / 1000 % 60).toString().padStart(2, '0')}`;
const size = (bytes: number) => `${(bytes / 1024 / 1024).toFixed(2)} MiB`;
const stateLabel = (value: string) => ({available:t('plusLibrary.available'),missing:t('plusLibrary.missingAsset'),corrupt:t('plusLibrary.hashMismatch')}[value] || value);
onMounted(initialize);
</script>
<style scoped lang="scss">
.plus-library { position: fixed; inset: 24px; z-index: 1200; background: var(--color-background); color: var(--color-text-primary); border: 1px solid var(--color-border); border-radius: 10px; padding: 24px; overflow: auto; box-shadow: 0 10px 60px #0008; font-size: 13px; }
header, .toolbar, .filters, .pagination { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; margin-bottom: 16px; }
header { justify-content: space-between; } h1 { font-size: 22px; margin: 0; } h2 { font-size: 16px; }
button, input, select { font: inherit; color: inherit; background: var(--color-surface); border: 1px solid var(--color-border); border-radius: 5px; padding: 7px 10px; }
button { cursor: pointer; } button:disabled { opacity: .45; cursor: default; } button:focus-visible, input:focus-visible, select:focus-visible { outline: 2px solid var(--color-accent); }
.table-scroll { overflow: auto; max-height: 48vh; } table { width: 100%; border-collapse: collapse; white-space: nowrap; } th { position: sticky; top: 0; background: var(--color-background); text-align: left; } th button { border: 0; background: none; padding-left: 0; }
td { padding: 10px 12px 10px 0; border-bottom: 1px solid var(--color-border); } .error { color: #ff8d8d; } .duplicate { display: inline-block; padding: 2px 5px; background: #ca9c3322; color: #e5bb5a; font-size: 11px; }
details { padding: 8px 0; border-bottom: 1px solid var(--color-border); } summary { cursor: pointer; } progress { margin: 12px; width: 180px; } li { padding: 3px; } label { display: inline-flex; gap: 6px; align-items: center; } section label { display: flex; margin: 8px 0; } .analysis-info { color: var(--color-text-secondary); margin-top: 24px; } .loudness{margin-top:20px;border-top:1px solid var(--color-border);padding-top:12px}.loudness table{margin-top:10px}.loudness input{width:70px}
</style>
