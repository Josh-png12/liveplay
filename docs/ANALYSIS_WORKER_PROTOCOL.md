# LivePlay+ analysis worker protocol v1

The analysis boundary is local and offline. LivePlay Server launches the executable configured by `LIVEPLAY_ANALYSIS_WORKER`; it never accepts an executable path from HTTP and audio rendering never waits for this process. The server sends one UTF-8 JSON object followed by `\n` to stdin and reads one JSON object from stdout. stderr is diagnostic only. Each request creates a fresh worker process, so a crash cannot poison a later job.

All messages have `protocolVersion: 1` and an opaque `requestId`. The server rejects a different version, ID, message type, non-object response, malformed JSON, more than 8 MiB on stdout, or a result that does not validate against the nullable analysis schema.

```json
{"protocolVersion":1,"type":"health","requestId":"health-..."}
{"protocolVersion":1,"type":"analyze","requestId":"req-...","trackId":"track_...","path":"C:/.../assets/hash.wav","contentHash":"sha256:..."}
```

The health response is `{"protocolVersion":1,"type":"healthy","requestId":"health-..."}`. An analysis result is `{"protocolVersion":1,"type":"result","requestId":"req-...","status":"ok","data":{}}`; `data` is converted to the independent nullable `TrackAnalysis` document. A worker may return `status:"error"` with a bounded error string.

The default timeout is 30 seconds. Cancellation, timeout, invalid output and unexpected exit terminate the process and mark only the analysis job failed; Track metadata, assets, project playback and AudioEngine continue. The scheduler waits during active playback when Performance Mode is enabled. A future Python worker must retain this contract, add provider/model/configuration provenance to its result/cache key, and pass licensing, Windows packaging and resource-budget review before it advertises any capability.

`liveplay-analysis-worker-mock` is a test fixture. It supports health, an empty successful result, malformed output, deliberate crash and timeout paths. It does not analyze audio.

## `ffmpeg-basic` provider

The Event MVP adds an optional `liveplay-ffmpeg-analysis-worker` target using the same JSON Lines envelope. It reports only nullable duration, peak/RMS, integrated loudness and true peak fields plus bounded `recommended_gain_db`. The server discovers it beside the server executable or through `LIVEPLAY_FFMPEG_ANALYSIS_WORKER`; FFmpeg is selected with `LIVEPLAY_FFMPEG` or `PATH`. Missing FFmpeg, malformed output, timeout and worker exit fail only that analysis job. Installers must package the worker and a license-compliant FFmpeg build before enabling `LIVEPLAY_PLUS_BASIC_ANALYSIS`.
