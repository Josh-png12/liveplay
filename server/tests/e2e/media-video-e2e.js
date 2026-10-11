// Disposable MP4 pipeline smoke: H.264/AAC import -> probe -> PCM proxy -> waveform.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const ffmpeg = require('@ffmpeg-installer/ffmpeg').path;
const ffprobe = require('@ffprobe-installer/ffprobe').path;
const exe = path.resolve(process.argv[2] || 'server/build/liveplay-server.exe');
const root = fs.mkdtempSync(path.join(path.resolve('server/build'), 'media-e2e-'));
const inputs = path.join(root, 'inputs'); fs.mkdirSync(inputs);
const serverExe = path.join(root, path.basename(exe)); fs.copyFileSync(exe, serverExe);
for (const name of fs.readdirSync(path.dirname(exe))) if (name.endsWith('.dll')) fs.copyFileSync(path.join(path.dirname(exe), name), path.join(root, name));
const media = path.join(inputs, 'fixture-h264-aac.mp4');
const audioOnly = path.join(inputs, 'fixture-aac-only.mp4');
const videoOnly = path.join(inputs, 'fixture-h264-only.mp4');
const corrupt = path.join(inputs, 'fixture-corrupt.mp4');
const made = spawnSync(ffmpeg, ['-y','-f','lavfi','-i','color=c=blue:s=320x180:r=25:d=2','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=2','-c:v','libx264','-pix_fmt','yuv420p','-c:a','aac','-shortest',media], { windowsHide: true });
if (made.status !== 0) throw new Error(`fixture generation failed: ${made.stderr}`);
if (spawnSync(ffmpeg, ['-y','-f','lavfi','-i','sine=frequency=660:sample_rate=48000:duration=1','-c:a','aac',audioOnly], { windowsHide: true }).status !== 0) throw new Error('audio-only fixture failed');
if (spawnSync(ffmpeg, ['-y','-f','lavfi','-i','color=c=red:s=160x90:r=25:d=1','-c:v','libx264','-pix_fmt','yuv420p','-an',videoOnly], { windowsHide: true }).status !== 0) throw new Error('video-only fixture failed');
fs.writeFileSync(corrupt, 'not an mp4');
let proc; const port = 4587; const sleep = ms => new Promise(r => setTimeout(r, ms));
async function api(route, options = {}) { const r = await fetch(`http://127.0.0.1:${port}${route}`, { ...options, headers: { 'content-type':'application/json', ...(options.headers || {}) } }); return { status:r.status, body: await r.json() }; }
async function main() {
  proc = spawn(serverExe, ['--bind','127.0.0.1','--port',String(port),'--fs-root',inputs], { cwd:root, env:{...process.env,LIVEPLAY_PLUS_DATA_DIR:path.join(root,'plus')}, windowsHide:true });
  for(let i=0;i<100;i++){try{if((await api('/api/health')).status===200)break;}catch{}await sleep(100);}
  const batch=await api('/api/plus/imports',{method:'POST',body:JSON.stringify({files:[media]})});assert.equal(batch.status,202);
  let done;for(let i=0;i<200;i++){done=await api(`/api/plus/imports/${batch.body.id}`);if(['completed','failed'].includes(done.body.status))break;await sleep(50);}assert.equal(done.body.status,'completed');assert.equal(done.body.files[0].status,'imported');
  const id=done.body.files[0].track_id; let track=(await api(`/api/plus/library/tracks/${id}`)).body;assert.equal(track.media.media_type,'video');assert.equal(track.media.has_video,true);assert.equal(track.media.has_audio,true);assert.equal(track.media.video_codec,'h264');assert.equal(track.media.audio_codec,'aac');assert.equal(track.media.width,320);
  await api(`/api/plus/library/tracks/${id}/audio-proxy`,{method:'POST',body:'{}'});let proxy;for(let i=0;i<200;i++){proxy=(await api(`/api/plus/library/tracks/${id}/audio-proxy`)).body;if(['ready','failed'].includes(proxy.status))break;await sleep(50);}if(proxy.status !== 'ready') console.error(proxy);assert.equal(proxy.status,'ready');assert.ok(fs.existsSync(proxy.path));
  track=(await api(`/api/plus/library/tracks/${id}`)).body;assert.ok(track.media.audio_proxy_path);
  await api(`/api/plus/library/tracks/${id}/waveform`,{method:'POST',body:'{}'});let wave;for(let i=0;i<200;i++){wave=(await api(`/api/plus/library/tracks/${id}/waveform`)).body;if(['ready','failed'].includes(wave.status))break;await sleep(50);}assert.equal(wave.status,'ready');assert.ok(wave.channels[0].peak.length > 0);
  for (const [file, expected] of [[audioOnly, 'imported'], [videoOnly, 'imported'], [corrupt, 'failed']]) { const extra = await api('/api/plus/imports',{method:'POST',body:JSON.stringify({files:[file]})}); let settled; for(let i=0;i<200;i++){settled=await api(`/api/plus/imports/${extra.body.id}`);if(['completed','failed'].includes(settled.body.status))break;await sleep(50);} assert.equal(settled.body.files[0].status, expected); if (expected === 'imported') { const extraTrack = settled.body.files[0].track_id; await api(`/api/plus/library/tracks/${extraTrack}/audio-proxy`,{method:'POST',body:'{}'}); let extraProxy; for(let i=0;i<200;i++){extraProxy=(await api(`/api/plus/library/tracks/${extraTrack}/audio-proxy`)).body;if(['ready','failed'].includes(extraProxy.status))break;await sleep(50);} assert.equal(extraProxy.status,'ready'); if (file === videoOnly) { const videoTrack=(await api(`/api/plus/library/tracks/${extraTrack}`)).body; assert.equal(videoTrack.media.has_audio,false); } }
  }
  console.log('Media/video MP4 pipeline passed');
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(()=>{if(proc&&proc.exitCode===null)proc.kill();});
