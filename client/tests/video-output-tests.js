// Fast regression guard for the Electron boundary. BrowserWindow itself needs
// real displays, so keep this test deterministic and verify the contract the
// renderer depends on without instantiating Electron.
const assert = require('assert');
const fs = require('fs');
const source = fs.readFileSync(require.resolve('../electron/main.js'), 'utf8');
for (const token of ['crossfade', 'fade-to-black', 'video-output:identify', 'video-output:status', 'LIVEPLAY_FFMPEG', 'LIVEPLAY_FFPROBE', 'pathToFileURL']) assert(source.includes(token), `missing video output contract: ${token}`);
assert(source.indexOf('[bundledFfmpeg') < source.indexOf('process.env.LIVEPLAY_FFMPEG') && source.indexOf('process.env.LIVEPLAY_FFMPEG') < source.indexOf("await pathTool('ffmpeg')"), 'media tool resolution must remain bundled → environment → PATH');
console.log('PASS Video output IPC, transitions, display identification and media-tool resolution contract');
