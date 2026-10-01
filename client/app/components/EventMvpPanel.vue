<template>
  <section class="event-mvp" role="dialog" aria-modal="true" :aria-label="t('eventMvp.title')">
    <header class="event-header"><div><h1>{{ t('eventMvp.title') }}</h1><p>{{ t('eventMvp.description') }}</p></div><button @click="$emit('close')">{{ t('eventMvp.close') }}</button></header>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <div class="event-grid">
      <section class="library-column">
        <div class="toolbar"><button @click="initialize">{{ t('eventMvp.refresh') }}</button><button :disabled="!currentProject" @click="makeTemplate">{{ t('eventMvp.template') }}</button></div>
        <input v-model="search" class="search" type="search" :placeholder="t('eventMvp.search')">
        <div class="track-list"><button v-for="track in filteredTracks" :key="track.id" class="track-row" :class="{selected: selectedTrack?.id === track.id}" @click="selectTrack(track)"><strong>{{ track.title }}</strong><span>{{ track.artist || t('eventMvp.unknownArtist') }} · {{ duration(track.duration_ms) }}</span></button></div>
        <p v-if="!currentProject" class="hint">{{ t('eventMvp.openProject') }}</p>
      </section>
      <section class="editor-column">
        <template v-if="selectedTrack">
          <h2>{{ selectedTrack.title }}</h2><p>{{ selectedTrack.artist }} · {{ selectedTrack.album }}</p>
          <div class="waveform-wrap"><canvas ref="waveformCanvas" width="900" height="180" @click="seekWaveform" /><span v-if="waveformStatus !== 'ready'" class="waveform-status">{{ waveformStatus }}</span></div>
          <div class="wave-controls"><label>IN <input v-model.number="startMs" type="number" min="0" :max="durationMs"></label><label>OUT <input v-model.number="endMs" type="number" min="0" :max="durationMs"></label><label>{{ t('eventMvp.zoom') }} <input v-model.number="zoom" type="range" min="1" max="10"></label></div>
          <div class="editor-actions"><button @click="ensureCue">{{ cueForTrack ? t('eventMvp.preview') : t('eventMvp.createCue') }}</button><button :disabled="!cueForTrack" @click="playSelected">{{ t('eventMvp.play') }}</button><button :disabled="!cueForTrack" @click="stopCue(cueForTrack!.uuid)">{{ t('eventMvp.stop') }}</button></div>
          <div class="presets"><span>{{ t('eventMvp.presets') }}</span><button v-for="preset in presets" :key="preset.id" :disabled="!cueForTrack" @click="applyPreset(preset.id)">{{ preset.label }}</button></div>
          <div class="tags"><h3>{{ t('eventMvp.tags') }}</h3><label>{{ t('eventMvp.energy') }} <input v-model.number="tagEnergy" type="number" min="0" max="5"></label><label>{{ t('eventMvp.role') }} <input v-model="tagRole"></label><label><input v-model="tagApproved" type="checkbox"> {{ t('eventMvp.approved') }}</label><button @click="saveTags">{{ t('eventMvp.saveTags') }}</button></div>
        </template>
        <p v-else class="hint">{{ t('eventMvp.selectTrack') }}</p>
      </section>
      <section class="board-column">
        <div class="board-header"><h2>{{ t('eventMvp.showBoard') }}</h2><button @click="speechMode">{{ speechActive ? t('eventMvp.speechOff') : t('eventMvp.speechOn') }}</button></div>
        <div class="emergency"><button @click="stopAllCues">{{ t('eventMvp.stopAll') }}</button><button @click="fadeAll">{{ t('eventMvp.fadeAll') }}</button><button @click="nextCue">{{ t('eventMvp.next') }}</button></div>
        <div class="team-controls"><select v-model="selectedTeam"><option v-for="team in teams" :key="team.name" :value="team.name">{{ team.name }}</option></select><button :disabled="!cueForTrack" @click="assign('entry')">{{ t('eventMvp.assignEntry') }}</button><button :disabled="!cueForTrack" @click="assign('exit')">{{ t('eventMvp.assignExit') }}</button></div>
        <div class="team-grid"><article v-for="team in teams" :key="team.name" class="team-card"><h3>{{ team.name }}</h3><button class="show-button" :disabled="!team.entryCue" :class="{playing: isPlaying(team.entryCue)}" @click="fire(team.entryCue)">{{ t('eventMvp.entry') }}<small>{{ state(team.entryCue) }}</small></button><button class="show-button exit" :disabled="!team.exitCue" :class="{playing: isPlaying(team.exitCue)}" @click="fire(team.exitCue)">{{ t('eventMvp.exit') }}<small>{{ state(team.exitCue) }}</small></button></article></div>
      </section>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, nextTick, onMounted, ref, watch } from 'vue';
import type { AudioItem, GroupItem } from '~/types/project';
import { DEFAULT_AUDIO_ITEM, DEFAULT_GROUP_ITEM } from '~/types/project';
import type { PlusTrack } from '~/types/plus';

defineEmits<{ (event: 'close'): void }>();
const { t } = useLocalization();
const { tracks, error, initialize, getTrackWaveform, requestTrackWaveform, updateTrackTags } = usePlusLibrary();
const { currentProject, addItem, saveProject, selectedItem, openItemProperties, findItemByUuid } = useProject();
const { playCue, stopCue, stopAllCues, activeCues } = useAudioEngine();
const server = useLiveplayServer();
const search = ref(''), selectedTrack = ref<PlusTrack | null>(null), waveform = ref<any>(null), waveformStatus = ref('absent');
const startMs = ref(0), endMs = ref(0), zoom = ref(1), waveformCanvas = ref<HTMLCanvasElement>();
const selectedTeam = ref('Brasil'), speechActive = ref(false), speechOriginal = new Map<string, number>();
const tagEnergy = ref(0), tagRole = ref(''), tagApproved = ref(false);
const presets = computed(() => [
  { id: 'sports-entry', label: t('eventMvp.presetEntry') }, { id: 'sports-exit', label: t('eventMvp.presetExit') },
  { id: 'background', label: t('eventMvp.presetBackground') }, { id: 'awards', label: t('eventMvp.presetAwards') },
]);
type Team = { name: string; entryCue?: string; exitCue?: string };
const teams = ref<Team[]>(['Brasil','Colombia','Francia','Portugal','Alemania','España','Argentina'].map(name => ({ name })));
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
  try { let result = await getTrackWaveform(track.id); if (result.status !== 'ready') { await requestTrackWaveform(track.id); waveformStatus.value = 'queued'; for (let i = 0; i < 80 && result.status !== 'ready'; i++) { await new Promise(resolve => setTimeout(resolve, 250)); result = await getTrackWaveform(track.id); } } waveformStatus.value = result.status; waveform.value = result.status === 'ready' ? result : null; await nextTick(); drawWaveform(); }
  catch (e) { waveformStatus.value = String(e); }
}
function drawWaveform() { const canvas = waveformCanvas.value, channels = waveform.value?.channels || []; if (!canvas || !channels.length) return; const ctx = canvas.getContext('2d'); if (!ctx) return; ctx.clearRect(0, 0, canvas.width, canvas.height); const peaks = channels[0].peak || []; const step = Math.max(1, Math.floor(peaks.length / (canvas.width * zoom.value))); const mid = canvas.height / 2; ctx.fillStyle = '#59b6e6'; for (let x = 0; x < canvas.width; x++) { const index = Math.min(peaks.length - 1, x * step); const h = Math.max(1, Number(peaks[index] || 0) * mid); ctx.fillRect(x, mid - h, 1, h * 2); } ctx.fillStyle = '#ffbd4a'; if (durationMs.value) { ctx.fillRect(startMs.value / durationMs.value * canvas.width, 0, 2, canvas.height); ctx.fillRect(endMs.value / durationMs.value * canvas.width, 0, 2, canvas.height); } }
function seekWaveform(event: MouseEvent) { if (!durationMs.value) return; const rect = (event.currentTarget as HTMLCanvasElement).getBoundingClientRect(); const ms = Math.max(0, Math.min(durationMs.value, ((event.clientX - rect.left) / rect.width) * durationMs.value)); if (cueForTrack.value) server.seekItem(cueForTrack.value.uuid, ms / 1000); }
async function ensureCue() { if (!cueForTrack.value) await createCue(); else { selectedItem.value = cueForTrack.value; openItemProperties(cueForTrack.value.uuid); } }
async function createCue() { if (!selectedTrack.value || !currentProject.value) return; const track = selectedTrack.value; const uuid = crypto.randomUUID(); const item: AudioItem = { ...DEFAULT_AUDIO_ITEM, uuid, index: [currentProject.value.items.length], displayName: track.title || track.source_name, type: 'audio', mediaFileName: track.source_name, mediaPath: `plus/assets/${track.id}`, mediaServerPath: track.asset_path, waveformPath: `plus/waveforms/${track.id}.json`, duration: track.duration_ms / 1000, inPoint: startMs.value / 1000, outPoint: endMs.value / 1000, eventCue: { trackId: track.id, startMs: startMs.value, endMs: endMs.value, trackGainDb: 0, transition: { type: 'cut', durationMs: 0 } } } as AudioItem; addItem(item); selectedItem.value = item; await saveProject(); openItemProperties(uuid); }
function applyPreset(id: string) { const item = cueForTrack.value; if (!item) return; const values: Record<string, Partial<AudioItem>> = { 'sports-entry': { playFade: .2, stopFade: 1.2, crossFade: .3, endBehavior: { action: 'nothing' } }, 'sports-exit': { playFade: .2, stopFade: 2, crossFade: 0, endBehavior: { action: 'nothing' } }, background: { volume: .7, playFade: 1, stopFade: 1, crossFade: 1, endBehavior: { action: 'loop' } }, awards: { volume: .85, playFade: 1, stopFade: 3, crossFade: 1, endBehavior: { action: 'nothing' } } }; Object.assign(item, values[id]); item.eventCue = { ...(item.eventCue || {}), preset: id as any, fadeInMs: (item.playFade || 0) * 1000, fadeOutMs: (item.stopFade || 0) * 1000 }; void saveProject(); }
function isPlaying(uuid?: string) { return !!uuid && activeCues.value.has(uuid); }
function state(uuid?: string) { return !uuid ? t('eventMvp.ready') : isPlaying(uuid) ? t('eventMvp.playing') : t('eventMvp.ready'); }
function playSelected() { if (cueForTrack.value) void playCue(cueForTrack.value); }
function fire(uuid?: string) { const item = uuid ? findItemByUuid(uuid) : null; if (item) void playCue(item as AudioItem | GroupItem); }
function assign(kind: 'entry' | 'exit') { const team = teams.value.find(value => value.name === selectedTeam.value); if (team && cueForTrack.value) team[`${kind}Cue` as 'entryCue' | 'exitCue'] = cueForTrack.value.uuid; }
function makeTemplate() { if (!currentProject.value) return; const phases = ['Arrival','Delegations','Competitions','Break','Competitions II','Awards','Emergency']; for (const name of phases) { const group: GroupItem = { ...DEFAULT_GROUP_ITEM, uuid: crypto.randomUUID(), index: [currentProject.value.items.length], displayName: name, type: 'group', children: [] } as GroupItem; addItem(group); } void saveProject(); }
async function speechMode() { if (speechActive.value) { for (const [id, gain] of speechOriginal) await server.patchBus(id, { gainDb: gain }); speechOriginal.clear(); speechActive.value = false; return; } await server.fetchBuses(); for (const bus of server.buses) if (!bus.master && !bus.preview) { speechOriginal.set(bus.id, bus.gainDb); await server.patchBus(bus.id, { gainDb: bus.gainDb - 14 }); } speechActive.value = true; }
function fadeAll() { stopAllCues(); }
function nextCue() { const all = currentProject.value?.items || []; const item = all.find(value => value.type === 'audio') as AudioItem | undefined; if (item) void playCue(item); }
async function saveTags() { if (!selectedTrack.value) return; await updateTrackTags(selectedTrack.value.id, { tags: { energy: tagEnergy.value, language: '', country: '', genre: '', event_roles: tagRole.value ? [tagRole.value] : [], school_approved: tagApproved.value } }); }
watch([startMs, endMs, zoom], drawWaveform); watch(waveform, drawWaveform); onMounted(initialize);
</script>

<style scoped>
.event-mvp{position:fixed;inset:18px;z-index:1300;background:var(--color-background);color:var(--color-text-primary);padding:18px;overflow:auto;border:1px solid var(--color-border);border-radius:12px}.event-header,.board-header,.toolbar,.editor-actions,.presets,.emergency,.team-controls,.wave-controls{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.event-header{justify-content:space-between}.event-header h1{margin:0}.event-header p{margin:4px 0 14px;color:var(--color-text-secondary)}.event-grid{display:grid;grid-template-columns:1fr 1.35fr 1.2fr;gap:14px;min-height:70vh}.library-column,.editor-column,.board-column{border:1px solid var(--color-border);border-radius:8px;padding:12px;min-width:0}.track-list{max-height:62vh;overflow:auto}.track-row{display:flex;flex-direction:column;align-items:flex-start;width:100%;padding:9px;border:0;border-bottom:1px solid var(--color-border);background:transparent;color:inherit;text-align:left}.track-row.selected{background:var(--color-accent);color:white}.track-row span{font-size:12px;opacity:.75}.search,input,select,button{font:inherit;padding:7px;border-radius:5px;border:1px solid var(--color-border);background:var(--color-surface);color:inherit}.search{width:100%;margin:8px 0}.waveform-wrap{position:relative;background:#111;border-radius:6px;overflow:hidden}.waveform-wrap canvas{width:100%;height:180px;display:block}.waveform-status{position:absolute;inset:8px auto auto 8px;color:#ffd}.wave-controls label,.tags label{display:flex;gap:5px;align-items:center;margin:8px 0}.wave-controls input[type=number]{width:90px}.presets{margin-top:12px}.presets span{margin-right:3px}.tags{margin-top:18px;border-top:1px solid var(--color-border);padding-top:8px}.team-grid{display:grid;grid-template-columns:repeat(2,minmax(120px,1fr));gap:8px}.team-card{border:1px solid var(--color-border);padding:8px;border-radius:6px}.team-card h3{margin:0 0 7px}.show-button{width:100%;min-height:74px;background:#1f6f50;color:white;font-size:18px}.show-button.exit{background:#63539b}.show-button.playing{outline:3px solid #ffd15c}.show-button small{display:block;font-size:11px;margin-top:5px}.emergency button{flex:1;min-height:48px}.error{color:#ff9898}.hint{color:var(--color-text-secondary)}@media(max-width:1100px){.event-grid{grid-template-columns:1fr}.track-list{max-height:28vh}}
</style>
