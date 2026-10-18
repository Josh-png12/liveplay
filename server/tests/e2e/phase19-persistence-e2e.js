// Phase 1.9 save -> stop -> restart -> reopen reliability check.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const exe = path.resolve(process.argv[2] || 'server/build/liveplay-server.exe');
const root = fs.mkdtempSync(path.join(path.dirname(exe), 'phase19-e2e-'));
const serverExe = path.join(root, path.basename(exe)); fs.copyFileSync(exe, serverExe);
for (const name of fs.readdirSync(path.dirname(exe))) if (name.endsWith('.dll')) fs.copyFileSync(path.join(path.dirname(exe), name), path.join(root, name));
const port = 4592, projectPath = path.join(root, 'event.liveplay'); let proc;
const wait = ms => new Promise(r => setTimeout(r, ms));
async function api(route, options={}) { const r = await fetch(`http://127.0.0.1:${port}${route}`, { ...options, headers:{'content-type':'application/json',...(options.headers||{})} }); return { status:r.status, body:await r.json() }; }
async function start() { proc = spawn(serverExe, ['--port', String(port), '--bind', '127.0.0.1', '--fs-root', root], { cwd:root, windowsHide:true, env:{...process.env,LIVEPLAY_PLUS_DATA_DIR:path.join(root,'plus'),LIVEPLAY_MACHINE_DIR:path.join(root,'machine')} }); for(let i=0;i<100;i++){try{if((await api('/api/health')).status===200)return;}catch{} await wait(100);} throw new Error('server did not start'); }
async function stop(){ if(proc&&proc.exitCode===null)proc.kill(); await new Promise(r=>proc.once('exit',r)); }
async function main(){
  await start();
  const settings = { eventShowBoard:{ teams:[{id:'team-1',name:'Falcons',country:'CO',grade:'10',entryCueId:'audio-1',exitCueId:'video-1'}],selectedTeamId:'team-1',speechMode:{duckDb:-14},settings:{columns:2}}, eventLoudness:{enabled:true,targetLufs:-12,maxPositiveGainDb:6,maxNegativeGainDb:-12,truePeakCeilingDbtp:-1}, videoOutput:{displayId:42,idleKind:'logo',idleSource:'assets/logo.png'}, disableLimiter:false };
  const document = { name:'Phase 1.9 Event',version:'2.5.0',busSchema:2,items:[{uuid:'audio-1',type:'audio',displayName:'Entry',mediaServerPath:'audio.wav',inPoint:1,outPoint:9,volume:0.8,eventCue:{trackGainDb:-1.9}},{uuid:'video-1',type:'audio',displayName:'Exit Video',mediaServerPath:'video.mp4',inPoint:2,outPoint:8,volume:0.7,eventCue:{media:{hasVideo:true,sourcePath:'video.mp4'}}}],cartItems:[],cartOnlyItems:[],settings};
  assert.equal((await api('/api/project/load',{method:'POST',body:JSON.stringify({document})})).status,200);
  assert.equal((await api('/api/project/save',{method:'POST',body:JSON.stringify({path:projectPath,document})})).status,200);
  await stop(); await start();
  assert.equal((await api('/api/project/load',{method:'POST',body:JSON.stringify({path:projectPath})})).status,200);
  const loaded = await api('/api/project/document'); assert.equal(loaded.status,200);
  assert.deepEqual(loaded.body.settings.eventShowBoard, settings.eventShowBoard);
  assert.deepEqual(loaded.body.settings.eventLoudness, settings.eventLoudness);
  assert.deepEqual(loaded.body.settings.videoOutput, settings.videoOutput);
  assert.equal(loaded.body.items[0].eventCue.trackGainDb,-1.9);
  console.log('Phase 1.9 save/restart persistence passed');
}
main().catch(e=>{console.error(e);process.exitCode=1;}).finally(async()=>{if(proc&&proc.exitCode===null)proc.kill(); fs.rmSync(root,{recursive:true,force:true});});
