# LivePlay+ Media & Video Foundation

## Scope

Phase 1.8 adds one reliable audiovisual format: MP4 with H.264 video and AAC audio. `Track.media` is optional and backward-compatible; existing audio records retain their IDs and behave as audio-only media. The source MP4 stays immutable in the library asset store.

## Pipeline

`ImportService` hashes/copies first, then runs FFprobe outside the audio engine to read streams, duration, codecs, resolution, framerate and default audio facts. A video track with audio can request a regenerable cache entry through `POST /api/plus/library/tracks/<id>/audio-proxy`. FFmpeg emits 48 kHz stereo PCM s16le WAV to `plus/cache/audio-proxies`; the key is `source SHA-256 + pcm-s16le + 48000 + 2`.

The cue stores the original media source only as optional `eventCue.media` metadata, while `mediaServerPath` points to the proxy. Thus buses, PFL, gain, fades, stop-all, fade-all, Speech Mode and sports presets continue through the existing `AudioEngine`. Proxy/cache loss is recoverable by requesting it again; a project never requires cache files to open.

Waveform uses the completed proxy when present. Basic analysis can target the same proxy through the existing external analysis protocol. Performance Mode makes proxy and waveform work wait while audio is active.

## Video output and A/V clock

Electron owns a separate mute-only video-output window on a selected display. It never creates an HTML audio path. The Event MVP obtains position from the server-owned active cue (AudioEngine) and sends that value to video as the master clock. The monitor measures video position every 500 ms. Drift above 80 ms triggers a video seek; audio is never changed. The UI retains `avDriftMs`, `maxDriftMs` and `corrections` for diagnostics.

V1 video intent supports cut, fade-in, fade-out, fade-to-black and visual crossfade metadata. The initial secondary window implements cut/seek/black idle reliably; visual alpha transitions are reserved for the next display renderer pass.

## Errors and deployment

MP4 without audio receives a duration-matched silent PCM proxy so it can still use one AudioEngine cue as the master clock for video. Corrupt files, unsupported codecs, missing FFprobe/FFmpeg, lost proxy/source and closed output windows return recoverable errors; none stop legacy playback. Bundle or configure `LIVEPLAY_FFMPEG` and `LIVEPLAY_FFPROBE` for production. This repository validates with the installed Windows tools; packaging must add the approved FFmpeg/FFprobe binaries and their license notices.

## Windows smoke test

1. Ensure `ffmpeg.exe` and `ffprobe.exe` are on `PATH`, or set `LIVEPLAY_FFMPEG` and `LIVEPLAY_FFPROBE` to absolute paths.
2. Start LivePlay locally, open **Event MVP**, then import an H.264/AAC `.mp4` from Music Library.
3. Select the 🎬 record. Wait for the proxy/waveform status to become ready.
4. Set IN/OUT, create the cue, then use Preview Video or Play. Audio is rendered by LivePlay; the secondary monitor is mute-only.
5. Stop or Fade All and reopen the project. The cue metadata persists; if the proxy is missing, request it again.
# Video Presentation & Hardware RC

The secondary video output is visual-only. `AudioEngine` remains the single
audio authority; the renderer only sends the current audio position to a muted
Electron video window. Visual cues support cut, fade-in, fade-out, fade to
black and crossfade independently from the cue's audio fade.

The output can be assigned to a display in **Settings → Event Hardware Test**.
The desktop preference is saved under Electron user data and a project may also
store its optional `settings.videoOutput` reference. If a saved display or an
idle image is absent, LivePlay+ opens on the primary display and uses black.
`Identify Displays` creates a short, numbered overlay on each display.

Only `cache/audio-proxies` is clearable through `DELETE /api/plus/media-cache`.
It contains regenerated PCM proxies; it never deletes a project source or a
required project asset. The endpoint rejects clearing while a proxy job runs.

## FFmpeg distribution

Desktop resolution is **bundled binary → `LIVEPLAY_FFMPEG` / `LIVEPLAY_FFPROBE`
→ PATH**. Electron passes the resolved executable to the local C++ server, so
the release does not require a globally installed FFmpeg. No binary is fetched
at runtime. Missing tools leave the application usable and make only probe,
proxy, waveform and transcode-dependent operations unavailable.

The bundled executable comes from the pinned `@ffmpeg-installer/ffmpeg` and
`@ffprobe-installer/ffprobe` npm packages. Release packaging must retain the
package licence notices and record the exact upstream build/configuration in
the release manifest. FFmpeg's licensing depends on that upstream build
(LGPL or GPL); legal review of the shipped build is a release gate.
