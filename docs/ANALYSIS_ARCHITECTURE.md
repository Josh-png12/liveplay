# LivePlay+ Phase 1.5: analysis foundation

Analysis is optional server-side data. Neither AudioEngine, ProjectState, playback commands nor `.liveplay` reference these services. No analyzers, models, music DSP or Python runtime have been added. All analysis capabilities are false.

## Implemented interfaces

`server/include/liveplay/analysis/analysis.hpp` defines:

- `AnalysisJob`: Track ID, content hash, provider ID, asset path, schema version, status and error.
- `TrackAnalysisRepository`: optional per-track lookup and atomic save. `JsonTrackAnalysisRepository` stores bounded individual documents under `plus/analysis/tracks/<track_id>.json`.
- `AnalysisProvider`: provider identity, capabilities and execution contract. No concrete production provider is installed.
- `AnalysisScheduler`: bounded queue, durable jobs, deduplication by Track/provider/version 1, retry of failed jobs, recovery of running jobs as pending, worker stop and error isolation.

Jobs are persisted separately in `plus/analysis/jobs.json` (256 jobs maximum, 8 MiB read bound). No analysis is enqueued automatically on import. An empty scheduler performs no processing. Requests for unavailable providers end as failed jobs with explicit errors. Failed records have null analytical values. A stopped scheduler rejects new jobs; Track access and playback remain available.

The scheduler currently dispatches its provider interface on a separate C++ thread. This is orchestration scaffolding, not a production MIR execution path. The next provider must be an external process adapter with timeouts, bounded output, cancellation and process-tree cleanup before any heavy algorithm is enabled. A synchronous provider that never returns would block scheduler shutdown; no such provider ships.

```mermaid
flowchart LR
  HTTP[Plus HTTP] --> Library[LibraryService]
  HTTP --> Scheduler[AnalysisScheduler]
  Scheduler --> Jobs[Durable jobs]
  Scheduler --> Adapter[AnalysisProvider process adapter: future]
  Adapter --> Worker[Independent Analysis Worker: future]
  Worker --> Adapter
  Scheduler --> Results[TrackAnalysisRepository]
  Engine[Existing AudioEngine]
```

## Versioned nullable schema

```json
{
  "track_id": "track_<sha256>",
  "analysis_version": 1,
  "status": "pending",
  "providers": {},
  "basic": {"duration_ms": null, "peak_dbfs": null, "rms_dbfs": null},
  "loudness": {"integrated_lufs": null, "true_peak_dbtp": null, "lra": null},
  "rhythm": {"bpm": null, "confidence": null, "beats": []},
  "tonality": {"key": null, "scale": null, "camelot": null, "confidence": null},
  "structure": {"sections": []}
}
```

Analytical sections and fields can be omitted/null. Track ID and version are required. Present sections must be objects or null; supported statuses are pending/running/completed/failed. Documents are bounded at 8 MiB. A completed record includes content hash and provider provenance. Consumers must distinguish absent/null data from a measured zero.

This initial schema validates identity, version, section envelopes and size. Algorithm-specific units, confidence ranges, finite values and array element constraints must be validated by the next provider adapter. Version 1 does not encode multiple model/configuration cache generations: add algorithm/model version and parameters to the cache key before installing analyzers. Unknown schema versions fail explicitly.

## JSON/IPC protocol

`worker_request()` and `worker_response()` define and validate the future process boundary. Use newline-delimited UTF-8 JSON on stdin/stdout with a maximum 8 MiB envelope; logs go to stderr. Example request:

```json
{
  "protocol_version": 1,
  "operation": "analyze",
  "job": {
    "id": "job_track_<sha256>_provider",
    "track_id": "track_<sha256>",
    "content_hash": "sha256:<hash>",
    "provider": "provider",
    "asset_path": "C:/.../plus/assets/<hash>.wav",
    "analysis_version": 1,
    "status": "pending",
    "error": ""
  }
}
```

Successful response:

```json
{
  "protocol_version": 1,
  "job_id": "job_track_<sha256>_provider",
  "content_hash": "sha256:<hash>",
  "analysis": {"track_id": "track_<sha256>", "analysis_version": 1, "status": "completed", "providers": {}}
}
```

The validator rejects another job ID/hash/Track, unsupported versions, oversized documents and non-completed results. The scheduler checks current asset SHA-256 before and after execution and validates the provider result identity before commit. An external adapter must enforce timeouts/output limits and translate worker failures into job errors; it must not trust executable paths from HTTP input.

Windows deployment: hidden worker process, Unicode paths, bounded concurrency, CPU baseline, no WSL requirement. Heavy workers can later be Python for Essentia/MIR, but runtimes, packages, weights, licenses and reproducible installation must be reviewed first. No Internet or LLM dependency is permitted in audio rendering. Provider failure must never request transport changes.

## HTTP and capabilities

- `GET /api/plus/analysis/jobs`: durable job list.
- `POST /api/plus/analysis/jobs`: `{track_id, provider}`; 202. Unavailable providers fail asynchronously.
- `GET /api/plus/analysis/tracks/<id>`: optional record; 404 when absent.
- `GET /api/plus/capabilities`: `analysis: {basic:false,loudness:false,rhythm:false,tonality:false,structure:false,semantic:false,stems:false}`. `analysisAvailable:false` supplies an explicit aggregate flag.

These routes reuse operator authentication and exclude filesystem access by API tokens. Analysis initialization failure does not disable library or legacy playback. No WebSocket frames or project fields were added.

## Validation and deferred work

Native tests exercise the empty/stopped scheduler, nullable/failed records, running-job recovery, independent Track access and protocol correlation rejection. HTTP E2E submits an unavailable provider and verifies project/cue snapshots remain unchanged. This validates data/control isolation, not an uninterrupted-audio latency guarantee under CPU/IO contention.

Before a real provider: external process adapter, timeout/cancel protocol, versioned cache keys, granular schema validators, concurrency/resource budgets, job history/compaction, all-or-nothing result ownership across multiple providers, malformed-store backups and live audio contention measurements. No AutoDJ, beatmatching, stems or AI is part of this phase.
