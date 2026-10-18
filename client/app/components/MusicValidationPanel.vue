<template>
  <section class="validation" :aria-label="t('validation.title')">
    <h2>{{ t('validation.title') }}</h2>
    <p class="hint">{{ t('validation.description') }}</p>
    <div class="row">
      <label>{{ t('validation.folder') }} <input v-model="folder" type="text" :placeholder="t('validation.folderPlaceholder')" size="48"></label>
      <button v-if="canBrowse" @click="browse">{{ t('validation.browse') }}</button>
      <label><input v-model="recursive" type="checkbox"> {{ t('validation.recursive') }}</label>
      <button class="primary" :disabled="!folder || starting || running" @click="validation.start(folder, recursive)">{{ t('validation.start') }}</button>
      <button :disabled="!running" @click="validation.cancel()">{{ t('musical.cancelAnalysis') }}</button>
      <button :disabled="!run" @click="validation.download('json')">{{ t('validation.exportJson') }}</button>
      <button :disabled="!run" @click="validation.download('csv')">{{ t('validation.exportCsv') }}</button>
    </div>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <template v-if="run">
      <p role="status">{{ run.status }} · {{ run.done }}/{{ run.total }} · {{ run.folder }}<template v-if="run.error"> · <span class="error">{{ run.error }}</span></template></p>
      <progress :max="run.total" :value="run.done" />
      <dl v-if="metrics" class="metrics" :aria-label="t('validation.metrics')">
        <dt>{{ t('validation.exactBpm') }}</dt><dd>{{ pct(metrics.exactBpmAccuracy) }} <small>({{ metrics.bpmSamples }})</small></dd>
        <dt>{{ t('validation.octaveBpm') }}</dt><dd>{{ pct(metrics.octaveEquivalentBpmAccuracy) }}</dd>
        <dt>{{ t('validation.medianError') }}</dt><dd>{{ num(metrics.medianBpmError) }} BPM <small>({{ t('validation.octaveAdjusted') }}: {{ num(metrics.medianBpmErrorOctaveAdjusted) }})</small></dd>
        <dt>{{ t('validation.keyAccuracy') }}</dt><dd>{{ pct(metrics.keyAccuracy) }} <small>({{ metrics.keySamples }})</small></dd>
        <dt>{{ t('validation.camelotAccuracy') }}</dt><dd>{{ pct(metrics.camelotCompatibleAccuracy) }}</dd>
        <dt>{{ t('validation.failures') }}</dt><dd>{{ metrics.failures }}</dd>
        <dt>{{ t('validation.avgTime') }}</dt><dd>{{ num(metrics.averageProcessingMs, 0) }} ms <small>({{ t('validation.warm') }}: {{ num(metrics.averageWarmProcessingMs, 0) }} ms)</small></dd>
        <dt>{{ t('validation.smartCue') }}</dt><dd>{{ metrics.smartCue.good }} / {{ metrics.smartCue.acceptable }} / {{ metrics.smartCue.bad }} <small>({{ t('validation.goodRate') }}: {{ pct(metrics.smartCue.goodRate) }})</small></dd>
      </dl>
      <p v-if="metrics && metrics.bpmSamples === 0 && metrics.keySamples === 0" class="hint">{{ t('validation.noExpectations') }}</p>
      <div class="table-scroll"><table>
        <thead><tr><th>{{ t('validation.file') }}</th><th>{{ t('musical.bpm') }}</th><th>{{ t('validation.alternatives') }}</th><th>{{ t('musical.key') }}</th><th>{{ t('musical.camelot') }}</th><th>{{ t('musical.energy') }}</th><th>{{ t('musical.sections') }}</th><th>ms</th>
          <th>{{ t('validation.expectedBpm') }}</th><th>{{ t('validation.expectedKey') }}</th><th>{{ t('validation.smartCueRating') }}</th><th>{{ t('validation.notes') }}</th></tr></thead>
        <tbody><tr v-for="entry in run.entries" :key="entry.index" :class="entry.status">
          <td :title="entry.path">{{ entry.filename }}<div v-if="entry.error" class="error">{{ entry.error }}</div></td>
          <td>{{ entry.detectedBpm != null ? `${formatBpm(entry.detectedBpm)} (${entry.bpmConfidenceLabel || '—'})` : '—' }}</td>
          <td>{{ (entry.alternatives || []).map(a => formatBpm(a.bpm)).join(', ') || '—' }}</td>
          <td>{{ entry.detectedKey || '—' }}</td><td>{{ entry.camelot || '—' }}</td><td>{{ formatEnergy(entry.energy) }}</td>
          <td :title="(entry.sections || []).map(s => `${s.type} ${formatRange(s.startMs, s.endMs)}`).join('\n')">{{ (entry.sections || []).length }}</td>
          <td>{{ entry.analysisDurationMs != null ? Math.round(entry.analysisDurationMs) : '—' }}</td>
          <td><input type="number" min="20" max="400" step="0.1" :value="entry.expectedBpm ?? ''" :disabled="entry.status !== 'done'" @change="validation.edit(entry.index, { expectedBpm: numberOrNull(($event.target as HTMLInputElement).value) })"></td>
          <td><input type="text" size="9" :value="entry.expectedKey ?? ''" placeholder="F minor / 4A" :disabled="entry.status !== 'done'" @change="validation.edit(entry.index, { expectedKey: ($event.target as HTMLInputElement).value || null })"></td>
          <td><select :value="entry.smartCueRating ?? ''" :disabled="entry.status !== 'done'" :title="(entry.smartCue || []).map(s => `${s.intent} ${formatRange(s.startMs, s.endMs)}`).join('\n')" @change="validation.edit(entry.index, { smartCueRating: (($event.target as HTMLSelectElement).value || null) as any })"><option value="">—</option><option>Good</option><option>Acceptable</option><option>Bad</option></select></td>
          <td><input type="text" size="16" :value="entry.notes" :disabled="entry.status !== 'done'" @change="validation.edit(entry.index, { notes: ($event.target as HTMLInputElement).value })"></td>
        </tr></tbody>
      </table></div>
    </template>
  </section>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { formatBpm, formatEnergy, formatRange } from '~/utils/musical';

const { t } = useLocalization();
const validation = usePlusValidation();
const { run, error, starting } = validation;
const folder = ref(''), recursive = ref(false);
const electronApi: any = (globalThis as any).electronAPI?.liveplayServer;
const canBrowse = !!electronApi?.chooseMediaFolder;
const running = computed(() => !!run.value && ['queued', 'running'].includes(run.value.status));
const metrics = computed(() => run.value?.metrics ?? null);
const pct = (v: number | null | undefined) => (v == null ? '—' : `${Math.round(v * 1000) / 10}%`);
const num = (v: number | null | undefined, digits = 2) => (v == null ? '—' : v.toFixed(digits));
const numberOrNull = (text: string) => (text.trim() === '' ? null : Number(text));
async function browse() { const chosen = await electronApi.chooseMediaFolder(); if (chosen) folder.value = chosen; }
onMounted(() => { void validation.loadLatest(); });
</script>

<style scoped>
.validation { margin-top: 18px; border-top: 1px solid var(--color-border); padding-top: 12px; }
.row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin: 6px 0; } .hint { color: var(--color-text-secondary); } .error { color: #ff8d8d; }
.metrics { display: grid; grid-template-columns: max-content 1fr; gap: 3px 14px; margin: 10px 0; } .metrics dt { color: var(--color-text-secondary); } .metrics dd { margin: 0; }
.table-scroll { overflow: auto; max-height: 40vh; } table { border-collapse: collapse; white-space: nowrap; font-size: 12px; } th { position: sticky; top: 0; background: var(--color-background); text-align: left; padding-right: 10px; } td { padding: 4px 10px 4px 0; border-bottom: 1px solid var(--color-border); }
tr.failed td { background: #e5565b18; } input, select, button { font: inherit; color: inherit; background: var(--color-surface); border: 1px solid var(--color-border); border-radius: 5px; padding: 3px 7px; } input[type=number] { width: 70px; } button { cursor: pointer; } button.primary { border-color: var(--color-accent); } button:disabled { opacity: .45; cursor: default; }
progress { width: 260px; }
</style>
