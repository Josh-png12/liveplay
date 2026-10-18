const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const exe = path.resolve(process.argv[2] || 'server/build/liveplay-server.exe');
const root = fs.mkdtempSync(path.join(path.dirname(exe), 'security-mode-e2e-'));
const localExe = path.join(root, path.basename(exe)); fs.copyFileSync(exe, localExe);
for (const name of fs.readdirSync(path.dirname(exe))) if (name.endsWith('.dll')) fs.copyFileSync(path.join(path.dirname(exe), name), path.join(root, name));
const wait = ms => new Promise(r => setTimeout(r, ms));
async function main() {
  const denied = spawn(localExe, ['--port','4593','--bind','0.0.0.0','--require-auth','--fs-root',root], {cwd:root,windowsHide:true,env:{...process.env,LIVEPLAY_MACHINE_DIR:path.join(root,'empty-machine'),LIVEPLAY_PLUS_DATA_DIR:path.join(root,'plus')}});
  const deniedCode = await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(new Error('LAN gate did not stop server')),10000);denied.once('exit',code=>{clearTimeout(timer);resolve(code);});});
  assert.equal(deniedCode, 2, 'Event LAN without an admin must fail closed');
  let output=''; const local = spawn(localExe, ['--port','4594','--fs-root',root], {cwd:root,windowsHide:true,env:{...process.env,LIVEPLAY_MACHINE_DIR:path.join(root,'local-machine'),LIVEPLAY_PLUS_DATA_DIR:path.join(root,'plus2')}});
  local.stdout.on('data',d=>output+=d); local.stderr.on('data',d=>output+=d);
  for(let i=0;i<80&&!output.includes('127.0.0.1:4594');i++) await wait(100);
  assert.match(output,/127\.0\.0\.1:4594/,'default bind must be loopback'); local.kill();
  console.log('Phase 1.9 security modes passed');
}
main().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>fs.rmSync(root,{recursive:true,force:true}));
