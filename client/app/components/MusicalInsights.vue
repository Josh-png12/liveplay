<template>
  <section class="musical-insights" :aria-label="t('musical.title')">
    <div class="mi-head">
      <h3>{{ t('musical.title') }}</h3>
      <span class="status" :class="status">{{ t(`musical.status.${status}`) }}</span>
      <button :disabled="!canRun" @click="run(false)">{{ status === 'ready' ? t('musical.reanalyze') : t('musical.analyze') }}</button>
      <button v-if="status === 'running' || status === 'queued'" @click="musical.cancel([track.id])">{{ t('musical.cancelAnalysis') }}</button>
    </div>
    <p v-if="!musical.available.value" class="hint">{{ t('musical.providerMissing') }}</p>
    <p v-if="status === 'failed'" class="error" role="alert">{{ musical.jobs.value[track.id]?.error || t('musical.status.failed') }}</p>
    <p v-if="status === 'stale'" class="hint">{{ t('musical.staleHint') }}</p>
    <template v-if="analysis">
      <dl class="facts">
        <dt>{{ t('musical.bpm') }}</dt>
        <dd>{{ formatBpm(eff?.bpm ?? analysis.bpm) }} <span v-if="eff?.bpmSource && eff.bpmSource !== 'detected'" class="chip corrected">{{ t('musical.corrected') }}</span><span v-else-if="analysis.bpmConfidenceLabel" class="chip" :class="analysis.bpmConfidenceLabel" :title="String(analysis.bpmConfidence)">{{ t(`musical.confidence.${analysis.bpmConfidenceLabel}`) }}</span></dd>
        <dt>{{ t('musical.key') }}</dt>
        <dd>{{ formatKey({ key: eff?.key ?? analysis.key, scale: eff?.scale ?? analysis.scale }) }} <span v-if="eff?.keySource === 'corrected'" class="chip corrected">{{ t('musical.corrected') }}</span><span v-else-if="analysis.keyConfidence != null" class="chip" :class="confidenceLabel(analysis.keyConfidence) || 'low'">{{ t(`musical.confidence.${confidenceLabel(analysis.keyConfidence)}`) }}</span></dd>
        <dt>{{ t('musical.camelot') }}</dt>
        <dd><span v-if="eff?.camelot ?? analysis.camelot" class="camelot" :style="{ background: camelotColor(eff?.camelot ?? analysis.camelot) }">{{ eff?.camelot ?? analysis.camelot }}</span><template v-else>—</template></dd>
        <dt>{{ t('musical.energy') }}</dt>
        <dd><span class="bar"><i :style="{ width: formatEnergy(analysis.energy) }" /></span> {{ formatEnergy(analysis.energy) }} · {{ analysis.energyLevel || '—' }}/5</dd>
      </dl>
      <p v-if="analysis.bpmAmbiguous && eff?.bpmSource === 'detected'" class="warn" role="note">{{ t('musical.ambiguous', { alternatives: analysis.bpmAlternatives.map(a => formatBpm(a.bpm)).join(', ') }) }}</p>
      <p v-for="warning in analysis.warnings" :key="warning" class="hint">⚠ {{ warning }}</p>
      <details class="more corrections" :open="editing">
        <summary @click.prevent="editing = !editing">{{ t('musical.corrections') }}</summary>
        <p v-if="correctionError" class="error" role="alert">{{ correctionError }}</p>
        <h4>{{ t('musical.bpm') }}</h4>
        <dl class="facts small">
          <dt>{{ t('musical.detectedBpm') }}</dt><dd>{{ formatBpm(analysis.bpm) }}</dd>
          <dt>{{ t('musical.effectiveBpm') }}</dt><dd>{{ formatBpm(eff?.bpm ?? analysis.bpm) }} · {{ t(`musical.source.${eff?.bpmSource ?? 'detected'}`) }}</dd>
        </dl>
        <div v-if="analysis.bpmAlternatives.length" class="row"><span>{{ t('musical.alternatives') }}</span>
          <button v-for="alt in analysis.bpmAlternatives" :key="alt.relation + alt.bpm" @click="setBpm({ mode: 'custom', value: alt.bpm })">{{ formatBpm(alt.bpm) }}</button></div>
        <div class="row">
          <button :disabled="analysis.bpm == null" @click="setBpm({ mode: 'factor', value: 2 })">{{ t('musical.useDouble') }}</button>
          <button :disabled="analysis.bpm == null" @click="setBpm({ mode: 'factor', value: 0.5 })">{{ t('musical.useHalf') }}</button>
          <label>{{ t('musical.customBpm') }} <input v-model.number="customBpm" type="number" min="20" max="400" step="0.01"></label>
          <button :disabled="!(customBpm >= 20 && customBpm <= 400)" @click="setBpm({ mode: 'custom', value: customBpm })">{{ t('musical.apply') }}</button>
          <button :disabled="eff?.bpm == null" :title="t('musical.fine')" @click="setBpm({ mode: 'custom', value: round2((eff?.bpm ?? 0) - 0.1) })">−0.1</button>
          <button :disabled="eff?.bpm == null" :title="t('musical.fine')" @click="setBpm({ mode: 'custom', value: round2((eff?.bpm ?? 0) + 0.1) })">+0.1</button>
          <button :disabled="!corrections.bpm" @click="reset('bpm')">{{ t('musical.reset') }}</button>
        </div>
        <h4>{{ t('musical.key') }}</h4>
        <div class="row">
          <span>{{ t('musical.detectedKey') }}: {{ formatKey(analysis) }}</span>
          <label>{{ t('musical.correctedKey') }}
            <select :value="corrections.key ?? ''" @change="setKey(($event.target as HTMLSelectElement).value || null, corrections.scale ?? null)"><option value="">{{ t('musical.detected') }}</option><option v-for="name in PITCH_NAMES" :key="name" :value="name">{{ name }}</option></select></label>
          <label>{{ t('musical.scale') }}
            <select :value="corrections.scale ?? ''" @change="setKey(corrections.key ?? null, ($event.target as HTMLSelectElement).value || null)"><option value="">{{ t('musical.detected') }}</option><option value="major">major</option><option value="minor">minor</option></select></label>
          <span>{{ t('musical.camelot') }}: <span class="camelot" :style="{ background: camelotColor(eff?.camelot) }">{{ eff?.camelot || '—' }}</span></span>
          <button :disabled="!corrections.key && !corrections.scale" @click="reset('key')">{{ t('musical.reset') }}</button>
        </div>
        <h4>{{ t('musical.beatGrid') }}</h4>
        <div class="row">
          <span>{{ eff?.beatGrid ? t('musical.beatGridValue', { first: Math.round(eff.beatGrid.firstBeatMs), interval: eff.beatGrid.intervalMs.toFixed(1), tempo: formatBpm(eff.beatGrid.tempo) }) : '—' }} · {{ t(`musical.gridSource.${eff?.gridSource ?? 'detected'}`) }}</span>
        </div>
        <div class="row">
          <button :disabled="cursorMs == null || eff?.bpm == null" :title="t('musical.setFirstBeatHint')" @click="applyCorrection({ set: { firstBeatMs: Math.round(cursorMs ?? 0), gridOffsetMs: 0 } })">{{ t('musical.setFirstBeat') }}{{ cursorMs != null ? ` (${Math.round(cursorMs)} ms)` : '' }}</button>
          <select v-model.number="nudgeStep"><option :value="1">1 ms</option><option :value="5">5 ms</option><option :value="10">10 ms</option><option :value="50">50 ms</option></select>
          <button :disabled="eff?.bpm == null" @click="applyCorrection({ nudgeGridMs: -nudgeStep })">◀ {{ t('musical.gridLeft') }}</button>
          <button :disabled="eff?.bpm == null" @click="applyCorrection({ nudgeGridMs: nudgeStep })">{{ t('musical.gridRight') }} ▶</button>
          <span v-if="corrections.gridOffsetMs">{{ t('musical.gridOffset', { ms: corrections.gridOffsetMs }) }}</span>
          <button :disabled="!corrections.firstBeatMs && !corrections.gridOffsetMs && !corrections.bpm" @click="reset('all')">{{ t('musical.resetToDetected') }}</button>
        </div>
      </details>
      <details class="more">
        <summary>{{ t('musical.details') }}</summary>
        <dl class="facts small">
          <dt>{{ t('musical.provider') }}</dt><dd>{{ analysis.provider }} · {{ analysis.providerVersion }}</dd>
          <dt>{{ t('musical.analyzedAt') }}</dt><dd>{{ analysis.analyzedAt }}</dd>
          <dt>{{ t('musical.beatGrid') }}</dt>
          <dd v-if="analysis.beatGrid">{{ t('musical.beatGridValue', { first: Math.round(analysis.beatGrid.firstBeatMs), interval: analysis.beatGrid.intervalMs.toFixed(1), tempo: formatBpm(analysis.beatGrid.tempo) }) }}
            · {{ analysis.beatGrid.constantTempo ? t('musical.constantTempo') : t('musical.variableTempo', { count: analysis.beatGrid.segments.length }) }}</dd>
          <dd v-else>—</dd>
          <dt>{{ t('musical.beats') }}</dt><dd>{{ analysis.beatsMs.length }}</dd>
          <dt>{{ t('musical.downbeats') }}</dt><dd>{{ analysis.downbeatsAvailable ? analysis.downbeatsMs.length : t('musical.downbeatsUnavailable') }}</dd>
        </dl>
        <h4>{{ t('musical.sections') }}</h4>
        <p v-if="!analysis.sections.length" class="hint">{{ t('musical.noSections') }}</p>
        <ul class="sections"><li v-for="(section, index) in analysis.sections" :key="index">
          <span class="swatch" :style="{ background: sectionColor(section.type) }" />{{ t(`musical.section.${section.type}`) }}
          <span class="range">{{ formatRange(section.startMs, section.endMs) }}</span>
          <span class="chip" :class="confidenceLabel(section.confidence) || 'low'">{{ Math.round(section.confidence * 100) }}%</span>
        </li></ul>
      </details>
      <fieldset v-if="showOverlays" class="overlays"><legend>{{ t('musical.overlays') }}</legend>
        <label><input v-model="overlay.beats" type="checkbox" @change="emitOverlay"> {{ t('musical.showBeats') }}</label>
        <label :title="analysis.downbeatsAvailable ? '' : t('musical.downbeatsUnavailable')"><input v-model="overlay.downbeats" type="checkbox" :disabled="!analysis.downbeatsAvailable" @change="emitOverlay"> {{ t('musical.showDownbeats') }}</label>
        <label><input v-model="overlay.sections" type="checkbox" @change="emitOverlay"> {{ t('musical.showSections') }}</label>
      </fieldset>
    </template>
    <p v-else-if="status === 'absent'" class="hint">{{ t('musical.notAnalyzed') }}</p>

    <section v-if="showSuggestions" class="suggestions" :aria-label="t('musical.suggestions')">
      <div class="mi-head"><h4>{{ t('musical.suggestions') }}</h4><button :disabled="loadingSuggestions" @click="loadSuggestions">{{ t('musical.suggest') }}</button></div>
      <p v-if="suggestionNote" class="hint" role="status">{{ suggestionNote }}</p>
      <ul>
        <li v-for="s in visibleSuggestions" :key="s.intent" :class="{ selected: previewing === s.intent }">
          <div><strong>{{ intentLabel(s) }}</strong> <span class="range">{{ formatRange(s.startMs, s.endMs) }}</span>
            <span class="chip" :class="s.confidenceLabel">{{ t(`musical.confidence.${s.confidenceLabel}`) }}</span></div>
          <small>{{ s.reason }}</small>
          <div class="actions">
            <button @click="preview(s)">{{ t('musical.preview') }}</button>
            <button class="primary" @click="apply(s)">{{ t('musical.apply') }}</button>
            <button @click="reject(s)">{{ t('musical.reject') }}</button>
          </div>
        </li>
      </ul>
      <p v-if="loaded && !visibleSuggestions.length" class="hint">{{ t('musical.noSuggestions') }}</p>
    </section>
  </section>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref, watch } from 'vue';
import type { PlusTrack } from '~/types/plus';
import type { PlusMusical } from '~/composables/usePlusMusical';
import { PITCH_NAMES, analysisStatus, camelotColor, confidenceLabel, effectiveBeatsMs, formatBpm, formatEnergy, formatKey, formatRange, sectionColor } from '~/utils/musical';
import type { MusicalCorrections } from '~/utils/musical';
import type { CueSuggestion, MusicalAnalysis, OverlayOptions } from '~/utils/musical';

const props = withDefaults(defineProps<{ track: PlusTrack; musical: PlusMusical; showSuggestions?: boolean; showOverlays?: boolean; cursorMs?: number | null }>(), { showSuggestions: false, showOverlays: false, cursorMs: null });
const emit = defineEmits<{
  (e: 'overlay', value: { analysis: MusicalAnalysis | null; options: OverlayOptions }): void;
  (e: 'preview', value: CueSuggestion): void;
  (e: 'apply', value: CueSuggestion): void;
}>();
const { t } = useLocalization();
const analysis = ref<MusicalAnalysis | null>(null);
const overlay = reactive({ beats: true, downbeats: true, sections: true });
const editing = ref(false), correctionError = ref(''), customBpm = ref(120), nudgeStep = ref(5);
const eff = computed(() => analysis.value?.effective ?? null);
const corrections = computed<Partial<MusicalCorrections>>(() => analysis.value?.corrections ?? {});
const round2 = (v: number) => Math.round(v * 100) / 100;
async function applyCorrection(change: Parameters<PlusMusical['correct']>[1]) {
  correctionError.value = '';
  try { analysis.value = await props.musical.correct(props.track.id, change); emitOverlay(); }
  catch (e) { correctionError.value = String(e); }
}
const setBpm = (bpm: MusicalCorrections['bpm']) => applyCorrection({ set: { bpm } });
const setKey = (key: string | null, scale: string | null) => applyCorrection({ set: { key, scale } });
const reset = (what: string) => applyCorrection({ reset: what });
const suggestions = ref<CueSuggestion[]>([]), rejected = ref<Set<string>>(new Set()), loaded = ref(false), loadingSuggestions = ref(false);
const suggestionNote = ref(''), previewing = ref('');

const status = computed(() => analysisStatus({
  available: props.musical.available.value, summary: props.musical.summaries.value[props.track.id],
  trackHash: props.track.content_hash, job: props.musical.jobs.value[props.track.id],
}));
const canRun = computed(() => props.musical.available.value && status.value !== 'running' && status.value !== 'queued' && props.track.status !== 'missing' && props.track.status !== 'corrupt');
const visibleSuggestions = computed(() => suggestions.value.filter(s => !rejected.value.has(`${props.track.id}:${s.intent}`)));
const intentLabel = (s: CueSuggestion) => { const key = `musical.intent.${s.intent}`; const text = t(key); return text === key ? s.label : text; };

async function load() {
  suggestions.value = []; loaded.value = false; suggestionNote.value = ''; previewing.value = '';
  try { analysis.value = (await props.musical.details(props.track.id)).analysis; } catch { analysis.value = null; }
  emitOverlay();
}
function emitOverlay() { emit('overlay', { analysis: analysis.value ? { ...analysis.value, beatsMs: effectiveBeatsMs(analysis.value) } : null, options: { beats: overlay.beats, downbeats: overlay.downbeats && !!analysis.value?.downbeatsAvailable, sections: overlay.sections, suggestion: previewing.value ? (suggestions.value.find(s => s.intent === previewing.value) || null) : null } }); }
async function run(force: boolean) { await props.musical.analyze([props.track.id], force || status.value === 'ready'); }
async function loadSuggestions() {
  loadingSuggestions.value = true; suggestionNote.value = '';
  try {
    const set = await props.musical.suggestions(props.track.id);
    suggestions.value = set.suggestions; loaded.value = true;
    if (set.analysisState === 'stale') suggestionNote.value = t('musical.staleHint');
    else if (!set.inputs.musical && !set.inputs.waveform) suggestionNote.value = t('musical.suggestNeedsData');
  } catch (e) { suggestionNote.value = String(e); }
  finally { loadingSuggestions.value = false; }
}
function preview(s: CueSuggestion) { previewing.value = s.intent; emitOverlay(); emit('preview', s); }
function apply(s: CueSuggestion) { emit('apply', s); previewing.value = ''; emitOverlay(); suggestionNote.value = t('musical.applied', { name: intentLabel(s) }); }
function reject(s: CueSuggestion) {
  const next = new Set(rejected.value); next.add(`${props.track.id}:${s.intent}`); rejected.value = next;
  if (previewing.value === s.intent) { previewing.value = ''; emitOverlay(); }
}
onMounted(() => { void props.musical.refresh(); });
// A finished analysis (this track's job leaving running/queued) refreshes the details.
watch(() => props.track.id, load, { immediate: true });
watch(status, (now, before) => { if (['running', 'queued'].includes(before as string) && now === 'ready') void load(); });
</script>

<style scoped>
.musical-insights { border: 1px solid var(--color-border); border-radius: 8px; padding: 10px 12px; margin: 10px 0; font-size: 13px; }
.mi-head { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; } h3, h4 { margin: 0; font-size: 14px; }
h4 { margin: 10px 0 4px; } button { font: inherit; color: inherit; background: var(--color-surface); border: 1px solid var(--color-border); border-radius: 5px; padding: 4px 9px; cursor: pointer; } button:disabled { opacity: .45; cursor: default; }
button.primary { border-color: var(--color-accent); } .hint { color: var(--color-text-secondary); margin: 6px 0; } .error { color: #ff8d8d; } .warn { color: #e5bb5a; margin: 6px 0; }
.status { font-size: 11px; padding: 2px 7px; border-radius: 10px; background: #7a7f8744; } .status.ready { background: #3fa35f55; } .status.stale, .status.queued { background: #ca9c3355; } .status.running { background: #4a9fdf55; } .status.failed { background: #e5565b55; }
.facts { display: grid; grid-template-columns: max-content 1fr; gap: 4px 14px; margin: 8px 0; } .facts dt { color: var(--color-text-secondary); } .facts dd { margin: 0; } .facts.small { font-size: 12px; }
.chip { font-size: 10px; padding: 1px 6px; border-radius: 9px; text-transform: uppercase; background: #7a7f8755; } .chip.high { background: #3fa35f66; } .chip.medium { background: #ca9c3366; } .chip.low { background: #e5565b55; } .chip.corrected { background: #4a9fdf66; text-transform: none; } .row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin: 4px 0; } input[type=number], select { font: inherit; color: inherit; background: var(--color-surface); border: 1px solid var(--color-border); border-radius: 5px; padding: 3px 6px; } input[type=number] { width: 80px; }
.camelot { display: inline-block; min-width: 34px; text-align: center; padding: 1px 8px; border-radius: 5px; font-weight: 600; color: #fff; }
.bar { display: inline-block; width: 90px; height: 8px; background: #7a7f8744; border-radius: 4px; vertical-align: middle; overflow: hidden; } .bar i { display: block; height: 100%; background: linear-gradient(90deg, #4aa3df, #e5565b); }
.sections { list-style: none; padding: 0; margin: 0; } .sections li { display: flex; align-items: center; gap: 8px; padding: 2px 0; } .swatch { width: 10px; height: 10px; border-radius: 2px; } .range { color: var(--color-text-secondary); font-variant-numeric: tabular-nums; }
.overlays { border: 0; padding: 0; margin: 8px 0 0; display: flex; gap: 14px; flex-wrap: wrap; } legend { padding: 0; font-size: 11px; color: var(--color-text-secondary); } label { display: inline-flex; gap: 5px; align-items: center; }
.suggestions { margin-top: 10px; border-top: 1px solid var(--color-border); padding-top: 6px; } .suggestions ul { list-style: none; padding: 0; margin: 6px 0; } .suggestions li { padding: 6px 0; border-bottom: 1px solid var(--color-border); display: grid; gap: 4px; } .suggestions li.selected { background: #7dff9b14; } .actions { display: flex; gap: 6px; }
</style>
