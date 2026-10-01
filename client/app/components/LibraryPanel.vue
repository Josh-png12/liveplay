<template>
  <section class="plus-library" role="dialog" aria-modal="true" aria-label="Biblioteca musical" @dragover.prevent.stop @drop.prevent.stop="dropFiles" @keydown.stop @keyup.stop @keydown.esc.prevent="$emit('close')">
    <header><h1>Biblioteca musical</h1><button @click="$emit('close')" aria-label="Cerrar biblioteca">Cerrar</button></header>
    <p>Biblioteca del servidor · Los archivos importados no se añaden al proyecto.</p>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <ul v-if="localFailures.length" role="alert"><li v-for="(file, index) in localFailures" :key="index" class="error">{{ file.name }}: {{ file.error }}</li></ul>
    <div v-if="capabilities && !capabilities.library" class="recovery">
      <h2>Biblioteca no disponible</h2><p>{{ recovery?.error || capabilities.storageError }}</p>
      <label>Backup <select v-model="backup"><option value="">Seleccionar</option><option v-for="name in recovery?.backups || []" :key="name">{{ name }}</option></select></label>
      <label><input v-model="confirmedRecovery" type="checkbox"> Confirmo reemplazar el catálogo actual conservando todos los assets.</label>
      <button :disabled="!confirmedRecovery || !backup" @click="recover('restore', backup)">Restaurar backup</button>
      <button :disabled="!confirmedRecovery" @click="recover('reset')">Crear catálogo vacío para reconstruir</button>
    </div>
    <template v-else>
      <div class="toolbar">
        <button :disabled="uploading || !capabilities?.library" @click="relinkId = ''; fileInput?.click()">Seleccionar archivos</button>
        <input ref="fileInput" type="file" :multiple="!relinkId" accept=".wav,.mp3,.flac,.ogg,.m4a,.aac,.aif,.aiff,.opus,.wma" hidden @change="selectedFiles">
        <span role="status">{{ uploading ? 'Enviando archivos…' : 'Arrastra varios archivos aquí para importar' }}</span>
        <button :disabled="loading" @click="refresh">Actualizar</button>
        <button :disabled="maintenance?.status === 'running'" @click="maintain('verify')">Verificar SHA-256</button>
        <button :disabled="maintenance?.status === 'running'" @click="maintain('rebuild')">Reconstruir desde assets</button>
      </div>
      <div class="filters">
        <label>Buscar <input v-model="search" type="search" placeholder="Título, artista o álbum"></label>
        <label>Formato <select v-model="format"><option value="">Todos</option><option v-for="value in formats" :key="value">{{ value }}</option></select></label>
        <label>Estado <select v-model="state"><option value="">Todos</option><option value="available">Disponible</option><option value="missing">Faltante</option><option value="corrupt">Integridad incorrecta</option></select></label>
        <span>{{ filtered.length }} tracks{{ loading ? ' · Cargando…' : '' }}</span>
      </div>
      <div class="table-scroll"><table>
        <thead><tr><th v-for="column in columns" :key="column.key"><button @click="setSort(column.key)">{{ column.label }} {{ sort === column.key ? (descending ? '↓' : '↑') : '' }}</button></th><th>Acciones</th></tr></thead>
        <tbody><tr v-for="track in visibleTracks" :key="track.id">
          <td>{{ track.title }} <span v-if="duplicates.has(track.id)" class="duplicate">Duplicado detectado</span></td>
          <td>{{ track.artist || '—' }}</td><td>{{ track.album || '—' }}</td>
          <td>{{ duration(track.duration_ms) }}</td><td>{{ track.format }}</td><td>{{ track.sample_rate || '—' }}</td><td>{{ track.channels || '—' }}</td><td>{{ size(track.size_bytes) }}</td>
          <td :class="{error: track.status === 'missing' || track.status === 'corrupt'}">{{ stateLabel(track.status || 'available') }}</td>
          <td><button @click="relinkId = track.id; fileInput?.click()">Re-vincular</button></td>
        </tr></tbody>
      </table></div>
      <div class="pagination"><button :disabled="page === 0" @click="page--">Anterior</button><span>Página {{ page + 1 }} / {{ Math.max(1, Math.ceil(filtered.length / 100)) }}</span><button :disabled="(page + 1) * 100 >= filtered.length" @click="page++">Siguiente</button></div>
      <section aria-label="Importaciones"><h2>Importaciones</h2>
        <p v-if="!batches.length">No hay importaciones en esta sesión.</p>
        <details v-for="batch in batches" :key="batch.id">
          <summary>{{ batch.status }} · {{ completedFiles(batch) }}/{{ batch.files.length }} archivos · {{ batch.id }}</summary>
          <progress :max="batch.files.length" :value="completedFiles(batch)" />
          <button v-if="['queued','running'].includes(batch.status)" :disabled="batch.cancel_requested" @click="cancel(batch)">Cancelar pendientes</button>
          <button v-if="batch.files.some(f => ['failed','cancelled'].includes(f.status))" :disabled="uploading" @click="retry(batch)">Reintentar pendientes o fallidos</button>
          <ul><li v-for="(file, index) in batch.files" :key="index"><strong>{{ file.source.name }}</strong> · {{ file.status }} <span class="error">{{ file.error }}</span></li></ul>
        </details>
      </section>
      <section v-if="maintenance" aria-label="Mantenimiento"><h2>Mantenimiento</h2><p>{{ maintenance.status }} · Verificados: {{ maintenance.checked }} · Faltantes: {{ maintenance.missing }} · Corruptos: {{ maintenance.corrupt }} · Recuperados: {{ maintenance.recovered }} · Eliminados: {{ maintenance.removed }}</p>
        <ul><li v-for="entry in maintenance.errors || []" :key="entry.track_id || entry.asset || entry.error" class="error">{{ entry.error }}</li></ul>
        <div v-if="maintenance.orphans?.length"><h3>Assets sin referencia</h3>
          <label v-for="name in maintenance.orphans" :key="name"><input v-model="selectedOrphans" :value="name" type="checkbox"> {{ name }}</label>
          <label><input v-model="confirmedCleanup" type="checkbox"> Confirmo eliminar permanentemente solo los assets seleccionados.</label>
          <button :disabled="!confirmedCleanup || !selectedOrphans.length || maintenance.status === 'running'" @click="cleanup">Eliminar seleccionados</button>
        </div>
      </section>
      <p class="analysis-info">Análisis musical: proveedores no instalados. La reproducción no depende del análisis.</p>
    </template>
  </section>
</template>
<script setup lang="ts">
import { computed, ref, watch, onMounted } from 'vue';
import type { PlusTrack } from '~/types/plus';
import { filterLibrary, completedFiles } from '~/utils/library';
defineEmits<{ (e: 'close'): void }>();
const { tracks, batches, capabilities, error, uploading, loading, maintenance, recovery, localFailures, initialize, refresh, importFiles, cancel, retry, maintain, recover } = usePlusLibrary();
const fileInput = ref<HTMLInputElement>();
const relinkId = ref(''), search = ref(''), format = ref(''), state = ref(''), sort = ref<keyof PlusTrack>('title'), descending = ref(false), page = ref(0);
const backup = ref(''), confirmedRecovery = ref(false), confirmedCleanup = ref(false), selectedOrphans = ref<string[]>([]);
const columns: {key: keyof PlusTrack; label: string}[] = [
  {key:'title',label:'Título'}, {key:'artist',label:'Artista'}, {key:'album',label:'Álbum'}, {key:'duration_ms',label:'Duración'},
  {key:'format',label:'Formato'}, {key:'sample_rate',label:'Sample rate'}, {key:'channels',label:'Canales'}, {key:'size_bytes',label:'Tamaño'}, {key:'status',label:'Estado'}
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
function cleanup() { void maintain('cleanup', [...selectedOrphans.value], true); selectedOrphans.value = []; confirmedCleanup.value = false; }
const duration = (ms: number) => `${Math.floor(ms / 60000)}:${Math.floor(ms / 1000 % 60).toString().padStart(2, '0')}`;
const size = (bytes: number) => `${(bytes / 1024 / 1024).toFixed(2)} MiB`;
const stateLabel = (value: string) => ({available:'Disponible',missing:'Asset faltante',corrupt:'SHA-256 incorrecto'}[value] || value);
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
details { padding: 8px 0; border-bottom: 1px solid var(--color-border); } summary { cursor: pointer; } progress { margin: 12px; width: 180px; } li { padding: 3px; } label { display: inline-flex; gap: 6px; align-items: center; } section label { display: flex; margin: 8px 0; } .analysis-info { color: var(--color-text-secondary); margin-top: 24px; }
</style>
