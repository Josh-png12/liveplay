<template>
  <section class="event-mvp" role="dialog" aria-modal="true" :aria-label="t('eventMvp.title')">
    <header class="event-header"><div><h1>{{ t('eventMvp.title') }}</h1><p>{{ t('eventMvp.description') }}</p></div><button @click="$emit('close')">{{ t('eventMvp.close') }}</button></header>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <div class="event-grid">
      <section class="library-column">
        <div class="toolbar"><button @click="initialize">{{ t('eventMvp.refresh') }}</button><button :disabled="!currentProject" @click="makeTemplate">{{ t('eventMvp.template') }}</button></div>
        <input v-model="search" class="search" type="search" :placeholder="t('eventMvp.search')">
        <div class="track-list"><button v-for="track in filteredTracks" :key="track.id" class="track-row" :class="{selected: selectedTrack?.id === track.id}" @click="selectTrack(track)"><strong>{{ track.media?.has_video ? '🎬' : '🎵' }} {{ track.title }}</strong><span>{{ track.artist || t('eventMvp.unknownArtist') }} · {{ duration(track.duration_ms) }} <template v-if="track.media?.has_video">· {{ track.media.width }}×{{ track.media.height }} · {{ track.media.video_codec }} / {{ track.media.audio_codec || '—' }}</template></span></button></div>
        <p v-if="!currentProject" class="hint">{{ t('eventMvp.openProject') }}</p>
      </section>
      <section class="editor-column">
        <template v-if="selectedTrack">
          <h2>{{ selectedTrack.title }}</h2><p>{{ selectedTrack.artist }} · {{ selectedTrack.album }}</p>
          <div class="waveform-wrap"><canvas ref="waveformCanvas" width="900" height="180" @click="seekWaveform" /><span v-if="waveformStatus !== 'ready'" class="waveform-status">{{ waveformStatus }}</span></div>
          <div class="wave-controls"><label>IN <input v-model.number="startMs" type="number" min="0" :max="durationMs"></label><label>OUT <input v-model.number="endMs" type="number" min="0" :max="durationMs"></label><label>{{ t('eventMvp.zoom') }} <input v-model.number="zoom" type="range" min="1" max="10"></label></div>
          <MusicalInsights :track="selectedTrack" :musical="musical" show-suggestions show-overlays @overlay="onMusicalOverlay" @preview="previewSuggestion" @apply="applySuggestion" />
          <div class="editor-actions"><button @click="ensureCue">{{ cueForTrack ? t('eventMvp.preview') : t('eventMvp.createCue') }}</button><button v-if="selectedTrack.media?.has_video" @click="previewVideo">🎬 {{ t('eventMvp.preview') }}</button><button :disabled="!cueForTrack" @click="playSelected">{{ t('eventMvp.play') }}</button><button :disabled="!cueForTrack" @click="stopSelected">{{ t('eventMvp.stop') }}</button></div>
          <div class="presets"><span>{{ t('eventMvp.presets') }}</span><button v-for="preset in presets" :key="preset.id" :disabled="!cueForTrack" @click="applyPreset(preset.id)">{{ preset.label }}</button></div>
          <div class="tags"><h3>{{ t('eventMvp.tags') }}</h3><label>{{ t('eventMvp.energy') }} <input v-model.number="tagEnergy" type="number" min="0" max="5"></label><label>{{ t('eventMvp.role') }} <input v-model="tagRole"></label><label><input v-model="tagApproved" type="checkbox"> {{ t('eventMvp.approved') }}</label><button @click="saveTags">{{ t('eventMvp.saveTags') }}</button></div>
        </template>
        <p v-else class="hint">{{ t('eventMvp.selectTrack') }}</p>
      </section>
      <section class="board-column">
        <div class="board-header"><h2>{{ t('eventMvp.showBoard') }}</h2><button @click="speechMode">{{ speechActive ? t('eventMvp.speechOff') : t('eventMvp.speechOn') }}</button></div>
        <section class="loudness-panel"><label><input v-model="eventLoudness.enabled" type="checkbox" @change="saveLoudnessSettings"> {{ t('eventMvp.eventLoudness') }}</label><button @click="applyOutdoorPreset">{{ t('eventMvp.eventOutdoor') }}</button><small>{{ t('eventMvp.eventLoudnessValues', { target: eventLoudness.targetLufs, up: eventLoudness.maxPositiveGainDb, down: eventLoudness.maxNegativeGainDb, peak: eventLoudness.truePeakCeilingDbtp }) }}</small></section>
        <dl class="master-meter"><dt>{{ t('eventMvp.masterPeak') }}</dt><dd :class="{clip: clipping}">{{ master.truePeak.value.toFixed(1) }} dBTP</dd><dt>{{ t('eventMvp.masterRms') }}</dt><dd>{{ master.rms.value.toFixed(1) }} dBFS</dd><dt>{{ t('eventMvp.clipping') }}</dt><dd :class="{clip: clipping}">{{ clipping ? t('eventMvp.yes') : t('eventMvp.no') }}</dd><dt>{{ t('eventMvp.outputGain') }}</dt><dd>{{ masterGainDb.toFixed(1) }} dB</dd><dt>{{ t('eventMvp.limiterReduction') }}</dt><dd>{{ master.gainReduction.value.toFixed(1) }} dB</dd></dl>
        <div class="emergency"><button @click="stopAllCues">{{ t('eventMvp.stopAll') }}</button><button @click="fadeAll">{{ t('eventMvp.fadeAll') }}</button><button @click="nextCue">{{ t('eventMvp.next') }}</button></div>
        <div class="team-controls"><select v-model="selectedTeam" @change="saveShowBoard"><option v-for="team in teams" :key="team.id" :value="team.name">{{ team.name }}</option></select><button :disabled="!cueForTrack" @click="assign('entry')">{{ t('eventMvp.assignEntry') }}</button><button :disabled="!cueForTrack" @click="assign('exit')">{{ t('eventMvp.assignExit') }}</button></div>
        <div class="team-grid"><article v-for="team in teams" :key="team.id" class="team-card"><h3>{{ team.name }}</h3><input v-model="team.country" :placeholder="t('eventMvp.country')" @change="saveShowBoard"><input v-model="team.grade" :placeholder="t('eventMvp.grade')" @change="saveShowBoard"><button class="show-button" :disabled="!team.entryCueId" :class="{playing: isPlaying(team.entryCueId)}" @click="fire(team.entryCueId)">{{ t('eventMvp.entry') }}<small>{{ state(team.entryCueId) }}</small></button><button class="show-button exit" :disabled="!team.exitCueId" :class="{playing: isPlaying(team.exitCueId)}" @click="fire(team.exitCueId)">{{ t('eventMvp.exit') }}<small>{{ state(team.exitCueId) }}</small></button></article></div>
      </section>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import type { AudioItem, GroupItem } from '~/types/project';
import { DEFAULT_AUDIO_ITEM, DEFAULT_GROUP_ITEM } from '~/types/project';
import type { PlusTrack } from '~/types/plus';
import { drawMusicalOverlays } from '~/utils/musical';
import type { CueSuggestion, MusicalAnalysis, OverlayOptions } from '~/utils/musical';
import { useVideoOutput } from '~/composables/useVideoOutput';
import { useMasterMeter } from '~/composables/useLiveMeters';

defineEmits<{ (event: 'close'): void }>();
const { t } = useLocalization();
const { tracks, error, initialize, getTrackWaveform, requestTrackWaveform, requestAudioProxy, getAudioProxy, updateTrackTags, refresh } = usePlusLibrary();
const { currentProject, addItem, saveProject, selectedItem, openItemProperties, findItemByUuid } = useProject();
const { playCue, stopCue, stopAllCues, activeCues, masterGainDb } = useAudioEngine();
const server = useLiveplayServer();
const video = useVideoOutput();
const musical = usePlusMusical();
const musicalOverlay = ref<{ analysis: MusicalAnalysis | null; options: OverlayOptions }>({ analysis: null, options: { beats: true, downbeats: true, sections: true } });
const search = ref(''), selectedTrack = ref<PlusTrack | null>(null), waveform = ref<any>(null), waveformStatus = ref('absent');
const startMs = ref(0), endMs = ref(0), zoom = ref(1), waveformCanvas = ref<HTMLCanvasElement>();
const selectedTeam = ref('Brasil'), speechActive = ref(false), speechOriginal = new Map<string, number>();
const tagEnergy = ref(0), tagRole = ref(''), tagApproved = ref(false);
const master = useMasterMeter(() => 0);
const eventLoudness = ref({ enabled: false, targetLufs: -12, maxPositiveGainDb: 6, maxNegativeGainDb: -12, truePeakCeilingDbtp: -1 });
const clipping = computed(() => master.truePeak.value >= eventLoudness.value.truePeakCeilingDbtp);
const presets = computed(() => [
  { id: 'sports-entry', label: t('eventMvp.presetEntry') }, { id: 'sports-exit', label: t('eventMvp.presetExit') },
  { id: 'background', label: t('eventMvp.presetBackground') }, { id: 'awards', label: t('eventMvp.presetAwards') },
]);
type Team = { id: string; name: string; country?: string; grade?: string; entryCueId?: string; exitCueId?: string };
const defaultTeams = () => ['Brasil','Colombia','Francia','Portugal','Alemania','España','Argentina'].map(name => ({ id: crypto.randomUUID(), name }));
const teams = ref<Team[]>(defaultTeams());
const filteredTracks = computed(() => tracks.value.filter(track => `${track.title} ${track.artist} ${track.album}`.toLowerCase().includes(search.value.toLowerCase())));
const cueForTrack = computed<AudioItem | null>(() => {
  const id = selectedTrack.value?.id; if (!id || !currentProject.value) return null;
  const all = (items: any[]): any[] => items.flatMap(item => item.type === 'group' ? [item, ...all(item.children || [])] : [item]);
  return (all(currentProject.value.items).find(item => item.type === 'audio' && item.eventCue?.trackId === id) as AudioItem) || null;
});
const durationMs = computed(() => selectedTrack.value?.duration_ms || 0);
function duration(ms: number) { return `${Math.floor(ms / 60000)}:${Math.floor(ms / 1000 % 60).toString().padStart(2, '0')}`; }
function selectTrack(track: PlusTrack) { selectedTrack.value = track; startMs.value = 0; endMs.value = track.duration_ms; tagEnergy.value = track.tags?.energy || 0; tagRole.value = track.tags?.event_roles?.[0] || ''; tagApproved.value = !!track.tags?.school_approved; void loadWaveform(track); }
async function loadWaveform(track: PlusTrack) {
  try { if (track.format === 'mp4' && !track.media?.audio_proxy_path) { await requestAudioProxy(track.id); let proxy: any; for (let i = 0; i < 160; i++) { await new Promise(resolve => setTimeout(resolve, 250)); proxy = await getAudioProxy(track.id); if (['ready', 'failed'].includes(proxy.status)) break; } if (proxy?.status !== 'ready') { waveformStatus.value = proxy?.error || 'proxy failed'; return; } await refresh(); selectedTrack.value = tracks.value.find(value => value.id === track.id) || track; } let result = await getTrackWaveform(track.id); if (result.status !== 'ready') { await requestTrackWaveform(track.id); waveformStatus.value = 'queued'; for (let i = 0; i < 80 && result.status !== 'ready'; i++) { await new Promise(resolve => setTimeout(resolve, 250)); result = await getTrackWaveform(track.id); } } waveformStatus.value = result.status; waveform.value = result.status === 'ready' ? result : null; await nextTick(); drawWaveform(); }
  catch (e) { waveformStatus.value = String(e); }
}
function drawWaveform() { const canvas = waveformCanvas.value, channels = waveform.value?.channels || []; if (!canvas || !channels.length) return; const ctx = canvas.getContext('2d'); if (!ctx) return; ctx.clearRect(0, 0, canvas.width, canvas.height); const peaks = channels[0].peak || []; const step = Math.max(1, Math.floor(peaks.length / (canvas.width * zoom.value))); const mid = canvas.height / 2; ctx.fillStyle = '#59b6e6'; for (let x = 0; x < canvas.width; x++) { const index = Math.min(peaks.length - 1, x * step); const h = Math.max(1, Number(peaks[index] || 0) * mid); ctx.fillRect(x, mid - h, 1, h * 2); } ctx.fillStyle = '#ffbd4a'; if (durationMs.value) { ctx.fillRect(startMs.value / durationMs.value * canvas.width, 0, 2, canvas.height); ctx.fillRect(endMs.value / durationMs.value * canvas.width, 0, 2, canvas.height); } drawMusicalOverlays(ctx, canvas.width, canvas.height, durationMs.value, musicalOverlay.value.analysis, musicalOverlay.value.options); }
function onMusicalOverlay(value: { analysis: MusicalAnalysis | null; options: OverlayOptions }) { musicalOverlay.value = value; drawWaveform(); }
let suggestionPreviewTimer: ReturnType<typeof setTimeout> | undefined;
// Auditioning never edits the cue: it plays the existing cue from the suggested IN and stops at the suggested OUT.
async function previewSuggestion(suggestion: CueSuggestion) {
  const item = cueForTrack.value; if (!item) { error.value = t('musical.previewNeedsCue'); return; }
  clearTimeout(suggestionPreviewTimer);
  await prepareCueGain(item); await playCue(item); server.seekItem(item.uuid, suggestion.startMs / 1000);
  suggestionPreviewTimer = setTimeout(() => void stopCue(item.uuid), Math.min(45000, suggestion.endMs - suggestion.startMs));
}
// Applying is the only step that changes anything, and only the cue's IN/OUT and fades.
async function applySuggestion(suggestion: CueSuggestion) {
  clearTimeout(suggestionPreviewTimer);
  startMs.value = suggestion.startMs; endMs.value = suggestion.endMs;
  const item = cueForTrack.value;
  if (item) {
    item.inPoint = suggestion.startMs / 1000; item.outPoint = suggestion.endMs / 1000;
    item.playFade = suggestion.fadeInMs / 1000; item.stopFade = suggestion.fadeOutMs / 1000;
    item.eventCue = { ...(item.eventCue || {}), startMs: suggestion.startMs, endMs: suggestion.endMs, fadeInMs: suggestion.fadeInMs, fadeOutMs: suggestion.fadeOutMs } as AudioItem['eventCue'];
    await saveProject();
  }
  drawWaveform();
}
function seekWaveform(event: MouseEvent) { if (!durationMs.value) return; const rect = (event.currentTarget as HTMLCanvasElement).getBoundingClientRect(); const ms = Math.max(0, Math.min(durationMs.value, ((event.clientX - rect.left) / rect.width) * durationMs.value)); if (cueForTrack.value) server.seekItem(cueForTrack.value.uuid, ms / 1000); }
async function ensureCue() { if (!cueForTrack.value) await createCue(); else { selectedItem.value = cueForTrack.value; openItemProperties(cueForTrack.value.uuid); } }
async function createCue() { if (!selectedTrack.value || !currentProject.value) return; const track = selectedTrack.value; if (track.format === 'mp4' && !track.media?.audio_proxy_path) { error.value = 'Audio proxy is not ready'; return; } const gain = eventLoudness.value.enabled && typeof track.prepared_gain_db === 'number' ? track.prepared_gain_db : 0; const uuid = crypto.randomUUID(); const item: AudioItem = { ...DEFAULT_AUDIO_ITEM, uuid, index: [currentProject.value.items.length], displayName: track.title || track.source_name, type: 'audio', volume: Math.pow(10, gain / 20), mediaFileName: track.source_name, mediaPath: `plus/assets/${track.id}`, mediaServerPath: track.format === 'mp4' ? track.media?.audio_proxy_path : track.asset_path, waveformPath: `plus/waveforms/${track.id}.json`, duration: track.duration_ms / 1000, inPoint: startMs.value / 1000, outPoint: endMs.value / 1000, eventCue: { trackId: track.id, startMs: startMs.value, endMs: endMs.value, trackGainDb: gain, transition: { type: 'cut', durationMs: 0, }, media: { sourcePath: track.asset_path, mediaType: track.media?.media_type || 'audio', hasVideo: !!track.media?.has_video, videoTransition: 'cut' } } } as AudioItem; addItem(item); selectedItem.value = item; await saveProject(); openItemProperties(uuid); }
function applyPreset(id: string) { const item = cueForTrack.value; if (!item) return; const values: Record<string, Partial<AudioItem>> = { 'sports-entry': { playFade: .2, stopFade: 1.2, crossFade: .3, endBehavior: { action: 'nothing' } }, 'sports-exit': { playFade: .2, stopFade: 2, crossFade: 0, endBehavior: { action: 'nothing' } }, background: { volume: .7, playFade: 1, stopFade: 1, crossFade: 1, endBehavior: { action: 'loop' } }, awards: { volume: .85, playFade: 1, stopFade: 3, crossFade: 1, endBehavior: { action: 'nothing' } } }; Object.assign(item, values[id]); item.eventCue = { ...(item.eventCue || {}), preset: id as any, fadeInMs: (item.playFade || 0) * 1000, fadeOutMs: (item.stopFade || 0) * 1000 }; void saveProject(); }
function isPlaying(uuid?: string) { return !!uuid && activeCues.value.has(uuid); }
function state(uuid?: string) { return !uuid ? t('eventMvp.ready') : isPlaying(uuid) ? t('eventMvp.playing') : t('eventMvp.ready'); }
async function previewVideo() { const track = selectedTrack.value; if (track?.media?.has_video) await video.play(track.asset_path, startMs.value / 1000, { fullscreen: true }); }
const visualTransition = (cue: AudioItem) => ({ cut: 'cut', 'fade-in': 'fade-in', 'fade-out': 'fade-in', 'fade-black': 'fade-in', crossfade: 'crossfade' } as const)[cue.eventCue?.media?.videoTransition || 'cut'];
async function prepareCueGain(item: AudioItem) { if (!eventLoudness.value.enabled || activeCues.value.has(item.uuid)) return; const track = tracks.value.find(value => value.id === item.eventCue?.trackId); if (!track || typeof track.prepared_gain_db !== 'number') return; item.eventCue = { ...(item.eventCue || {}), trackGainDb: track.prepared_gain_db }; item.volume = Math.pow(10, track.prepared_gain_db / 20); await saveProject(); }
async function playSelected() { const item = cueForTrack.value; if (!item) return; await prepareCueGain(item); if (item.eventCue?.media?.hasVideo) await video.play(item.eventCue.media.sourcePath, item.inPoint, { fullscreen: true, transition: visualTransition(item), durationMs: item.eventCue?.fadeInMs || 350 }); await playCue(item); }
async function stopSelected() { const item = cueForTrack.value; if (item) await stopCue(item.uuid); await video.stop(item?.eventCue?.media?.videoTransition === 'fade-black' ? 'fade-to-black' : 'fade-out', item?.eventCue?.fadeOutMs || 350); }
async function fire(uuid?: string) { const item = uuid ? findItemByUuid(uuid) : null; if (!item) return; const cue = item as AudioItem; await prepareCueGain(cue); if (cue.eventCue?.media?.hasVideo) await video.play(cue.eventCue.media.sourcePath, cue.inPoint, { fullscreen: true, transition: visualTransition(cue), durationMs: cue.eventCue?.fadeInMs || 350 }); await playCue(cue); }
async function saveShowBoard() { if (!currentProject.value) return; currentProject.value.settings.eventShowBoard = { teams: teams.value.map(team => ({ ...team })), selectedTeamId: teams.value.find(team => team.name === selectedTeam.value)?.id, speechMode: { duckDb: -14 }, settings: { columns: 2 } }; await saveProject(); }
async function assign(kind: 'entry' | 'exit') { const team = teams.value.find(value => value.name === selectedTeam.value); if (team && cueForTrack.value) { team[kind === 'entry' ? 'entryCueId' : 'exitCueId'] = cueForTrack.value.uuid; await saveShowBoard(); } }
function makeTemplate() { if (!currentProject.value) return; const phases = ['Arrival','Delegations','Competitions','Break','Competitions II','Awards','Emergency']; for (const name of phases) { const group: GroupItem = { ...DEFAULT_GROUP_ITEM, uuid: crypto.randomUUID(), index: [currentProject.value.items.length], displayName: name, type: 'group', children: [] } as GroupItem; addItem(group); } void saveProject(); }
async function speechMode() { const duckDb = currentProject.value?.settings.eventShowBoard?.speechMode?.duckDb ?? -14; if (speechActive.value) { for (const [id, gain] of speechOriginal) await server.patchBus(id, { gainDb: gain }); speechOriginal.clear(); speechActive.value = false; return; } await server.fetchBuses(); for (const bus of server.buses) if (!bus.master && !bus.preview) { speechOriginal.set(bus.id, bus.gainDb); await server.patchBus(bus.id, { gainDb: bus.gainDb + duckDb }); } speechActive.value = true; }
function fadeAll() { stopAllCues(); }
function nextCue() { const all = currentProject.value?.items || []; const item = all.find(value => value.type === 'audio') as AudioItem | undefined; if (item) void playCue(item); }
async function saveTags() { if (!selectedTrack.value) return; await updateTrackTags(selectedTrack.value.id, { tags: { energy: tagEnergy.value, language: '', country: '', genre: '', event_roles: tagRole.value ? [tagRole.value] : [], school_approved: tagApproved.value } }); }
async function saveLoudnessSettings() { if (!currentProject.value) return; currentProject.value.settings.eventLoudness = { ...eventLoudness.value }; await saveProject(); }
async function applyOutdoorPreset() { if (!currentProject.value) return; eventLoudness.value = { enabled: true, targetLufs: -12, maxPositiveGainDb: 6, maxNegativeGainDb: -12, truePeakCeilingDbtp: -1 }; currentProject.value.settings.disableLimiter = false; await saveLoudnessSettings(); }
let videoTimer: ReturnType<typeof setInterval> | undefined;
watch(activeCues, () => { const item = cueForTrack.value; const view: any = item ? activeCues.value.get(item.uuid) : null; if (view && item?.eventCue?.media?.hasVideo) void video.sync(view.currentTime || item.inPoint); });
watch([startMs, endMs, zoom], drawWaveform); watch(waveform, drawWaveform); onMounted(() => { const saved = currentProject.value?.settings.eventLoudness; if (saved) eventLoudness.value = { ...eventLoudness.value, ...saved }; const board = currentProject.value?.settings.eventShowBoard; if (board?.teams?.length) { teams.value = board.teams.map((team, index) => ({ id: team.id || `legacy-${index}`, name: team.name, country: team.country, grade: team.grade, entryCueId: team.entryCueId, exitCueId: team.exitCueId })); const selected = teams.value.find(team => team.id === board.selectedTeamId); selectedTeam.value = selected?.name || teams.value[0]?.name || ''; } initialize(); videoTimer = setInterval(() => { const item = cueForTrack.value; const view: any = item ? activeCues.value.get(item.uuid) : null; if (view && item?.eventCue?.media?.hasVideo) void video.sync(view.currentTime || item.inPoint); }, 250); }); onBeforeUnmount(() => { if (videoTimer) clearInterval(videoTimer); clearTimeout(suggestionPreviewTimer); });
</script>

<style scoped>
.event-mvp{position:fixed;inset:18px;z-index:1300;background:var(--color-background);color:var(--color-text-primary);padding:18px;overflow:auto;border:1px solid var(--color-border);border-radius:12px}.event-header,.board-header,.toolbar,.editor-actions,.presets,.emergency,.team-controls,.wave-controls{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.event-header{justify-content:space-between}.event-header h1{margin:0}.event-header p{margin:4px 0 14px;color:var(--color-text-secondary)}.event-grid{display:grid;grid-template-columns:1fr 1.35fr 1.2fr;gap:14px;min-height:70vh}.library-column,.editor-column,.board-column{border:1px solid var(--color-border);border-radius:8px;padding:12px;min-width:0}.track-list{max-height:62vh;overflow:auto}.track-row{display:flex;flex-direction:column;align-items:flex-start;width:100%;padding:9px;border:0;border-bottom:1px solid var(--color-border);background:transparent;color:inherit;text-align:left}.track-row.selected{background:var(--color-accent);color:white}.track-row span{font-size:12px;opacity:.75}.search,input,select,button{font:inherit;padding:7px;border-radius:5px;border:1px solid var(--color-border);background:var(--color-surface);color:inherit}.search{width:100%;margin:8px 0}.waveform-wrap{position:relative;background:#111;border-radius:6px;overflow:hidden}.waveform-wrap canvas{width:100%;height:180px;display:block}.waveform-status{position:absolute;inset:8px auto auto 8px;color:#ffd}.wave-controls label,.tags label{display:flex;gap:5px;align-items:center;margin:8px 0}.wave-controls input[type=number]{width:90px}.presets{margin-top:12px}.presets span{margin-right:3px}.tags{margin-top:18px;border-top:1px solid var(--color-border);padding-top:8px}.team-grid{display:grid;grid-template-columns:repeat(2,minmax(120px,1fr));gap:8px}.team-card{border:1px solid var(--color-border);padding:8px;border-radius:6px}.team-card h3{margin:0 0 7px}.show-button{width:100%;min-height:74px;background:#1f6f50;color:white;font-size:18px}.show-button.exit{background:#63539b}.show-button.playing{outline:3px solid #ffd15c}.show-button small{display:block;font-size:11px;margin-top:5px}.emergency button{flex:1;min-height:48px}.error{color:#ff9898}.hint{color:var(--color-text-secondary)}.loudness-panel{display:grid;gap:7px;padding:10px;margin:8px 0;border:1px solid var(--color-border);border-radius:6px}.loudness-panel small{color:var(--color-text-secondary)}.master-meter{display:grid;grid-template-columns:1fr auto;gap:4px 12px;font-variant-numeric:tabular-nums}.master-meter dt{color:var(--color-text-secondary)}.master-meter dd{margin:0}.clip{color:#ff5f5f;font-weight:700}@media(max-width:1100px){.event-grid{grid-template-columns:1fr}.track-list{max-height:28vh}}
</style>
