// Plays a continuous cue while the real Plus pipeline imports 1,000 unique WAVs.
// Pass criterion: no engine underrun/discontinuity and every render block stays
// inside its deadline. Uses a disposable server/store under server/build.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const exe = path.resolve(process.argv[2] || 'server/build/liveplay-server.exe');
const root = fs.mkdtempSync(path.join(path.resolve('server/build'), 'audio-isolation-'));
const inputs = path.join(root, 'inputs'); fs.mkdirSync(inputs);
const serverExe = path.join(root, path.basename(exe)); fs.copyFileSync(exe, serverExe);
for (const name of fs.readdirSync(path.dirname(exe))) if (name.endsWith('.dll')) fs.copyFileSync(path.join(path.dirname(exe), name), path.join(root, name));
const port = 4586; let proc, log = '';
const sleep = ms => new Promise(r => setTimeout(r, ms));
function wav(frames, amplitude, seed = 0) {
  const bytes = Buffer.alloc(44 + frames * 4); bytes.write('RIFF'); bytes.writeUInt32LE(bytes.length - 8, 4); bytes.write('WAVEfmt ', 8);
  bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(1, 20); bytes.writeUInt16LE(2, 22); bytes.writeUInt32LE(48000, 24); bytes.writeUInt32LE(192000, 28);
  bytes.writeUInt16LE(4, 32); bytes.writeUInt16LE(16, 34); bytes.write('data', 36); bytes.writeUInt32LE(bytes.length - 44, 40);
  for (let i=0; i<frames; i++) { const sample = Math.round(Math.sin(2*Math.PI*440*i/48000)*amplitude) + (i === seed % frames ? seed % 97 : 0); bytes.writeInt16LE(sample,44+i*4); bytes.writeInt16LE(sample,46+i*4); }
  return bytes;
}
async function api(route, body, method) {
  const response = await fetch(`http://127.0.0.1:${port}${route}`, {method:method || (body === undefined?'GET':'POST'), headers:body===undefined?{}:{'content-type':'application/json'}, body:body===undefined?undefined:JSON.stringify(body), signal:AbortSignal.timeout(15000)});
  return {status:response.status, body:await response.json()};
}
async function main() {
  const playing = path.join(inputs,'continuous.wav'); fs.writeFileSync(playing,wav(48000*30,1200));
  const files=[]; for(let i=0;i<1000;i++){const p=path.join(inputs,`load-${String(i).padStart(4,'0')}.wav`);fs.writeFileSync(p,wav(4800,700+i,i));files.push(p);}
  proc=spawn(serverExe,['--bind','127.0.0.1','--port',String(port),'--fs-root',inputs],{cwd:root,env:{...process.env,LIVEPLAY_PLUS_DATA_DIR:path.join(root,'plus')},windowsHide:true,stdio:['ignore','pipe','pipe']});
  proc.stdout.on('data',d=>log+=d);proc.stderr.on('data',d=>log+=d);
  for(let i=0;i<150;i++){try{if((await api('/api/health')).status===200)break;}catch{} if(proc.exitCode!==null)throw new Error(log);await sleep(100);}
  const item={uuid:'isolation-loop',type:'audio',displayName:'Isolation loop',mediaServerPath:playing,volume:0.1,endBehavior:'loop'};
  assert.equal((await api('/api/project/document',{name:'audio-isolation',items:[item]},'PUT')).status,200);
  for(let i=0;i<200;i++){if((await api('/api/project/progress')).body.loading===false)break;await sleep(50);}
  assert.equal((await api('/api/project/items/isolation-loop/play',{})).status,200); await sleep(1000);
  const baseline=await api('/api/engine/stats?reset=1'); assert(baseline.body.deviceCount>0,'an active output device is required for the acoustic test');
  const before=baseline.body; const submitted=await api('/api/plus/imports',{files,requestId:'audio-isolation-1000'}); assert.equal(submitted.status,202);
  let batch; const queue=[]; const started=Date.now();
  while(Date.now()-started<180000){await sleep(100); const stats=(await api('/api/engine/stats')).body;queue.push(stats.queuedMs);batch=(await api(`/api/plus/imports/${submitted.body.id}`)).body;if(['completed','failed'].includes(batch.status))break;}
  assert.equal(batch?.status,'completed'); assert.equal(batch.files.filter(f=>f.status==='imported').length,1000);
  await sleep(750); const after=(await api('/api/engine/stats')).body;
  const result={tracks:1000,duration_ms:Date.now()-started,underruns:after.underruns,dropouts:after.underruns,glitches:after.discontinuities,callback_overruns:after.renderBlockUsMax>after.blockBudgetUs?1:0,render_block_us_max:after.renderBlockUsMax,render_block_us_avg:after.renderBlockUsAvg,block_budget_us:after.blockBudgetUs,min_queued_ms:Math.min(...queue),cpu_ms:after.processCpuMs-before.processCpuMs,working_set_bytes:after.processWorkingSetBytes,peak_working_set_bytes:after.processPeakWorkingSetBytes,device_count:after.deviceCount};
  fs.writeFileSync(path.resolve('docs/AUDIO_ISOLATION_RESULTS.json'),JSON.stringify(result,null,2)+'\n');
  assert.equal(result.underruns,0); assert.equal(result.glitches,0); assert.equal(result.callback_overruns,0); assert(after.blocksRendered>0);
  console.log('PASS 0 audible dropouts during 1,000-track library import'); console.log(JSON.stringify(result,null,2));
}
main().catch(e=>{console.error(e.stack||e);console.error(log);process.exitCode=1;}).finally(async()=>{if(proc&&proc.exitCode===null){const exited=new Promise(r=>proc.once('exit',r));proc.kill();await exited;} fs.rmSync(root,{recursive:true,force:true});});
