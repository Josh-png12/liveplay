# LivePlay+ Phase 2 — Smart Music Engine

Musical analysis is an **optional, degradable, background** capability. It never runs in the audio callback, never starts automatically on import, and LivePlay starts and plays normally when it is absent. Nothing in this phase is event-specific.

## MIR provider decision

| Candidate | Licence | Windows distribution | Maintenance | Verdict |
|---|---|---|---|---|
| **librosa 0.11** | ISC | Pure-Python wheels (numpy/scipy/numba) install with `pip` | Active | **Chosen** |
| Essentia | AGPL-3.0 (commercial otherwise) | No official Windows wheels; native build needs a heavy toolchain | Active | Rejected for now; best candidate for a later native/`EssentiaProvider` |
| aubio | GPL-3.0 | No wheels for current Python on Windows | Effectively unmaintained (last release 2019) | Rejected |
| madmom | BSD code, **CC BY-NC-SA models** | Build problems on current Python | Unmaintained | Not used; the only realistic *downbeat* source, so a future `DownbeatProvider` candidate pending licence review |

This table comes from prior knowledge of those projects and was not re-verified online during this phase; re-check licences before shipping a bundled Python.

librosa has no downbeat tracker, so **downbeats are not provided and the capability is `false`**. The provider is reached only through `MusicalAnalysisProvider`, so Essentia, a native analyser or a cloud service can replace it without touching UI, library or engine.

## Architecture

```
Library UI ──HTTP──► control_server routes ──► AnalysisScheduler (single worker, Performance Mode aware)
                                                  │ sink                    │ execute
                                                  ▼                         ▼
                                     MusicalAnalysisStore        ExternalMusicalProvider
                              (analysis/musical/<id>.json +     (ExternalAnalysisProvider, below-normal
                               derived index.json)               priority, persistent JSON-Lines process)
                                                                            │
                                                          tools/mir/liveplay_mir_worker.py (librosa)
```

* `AnalysisScheduler` was extended, not duplicated: per-provider **result sinks** (musical results never overwrite the loudness record), `force` re-run, `cancel()`, idle `release()`, 4096-job bound with pruning of finished jobs, `integrity_path` so a decoded proxy can be analysed while the original's SHA-256 is still verified before and after.
* `ExternalAnalysisProvider` gained arguments, below-normal process priority and an optional **persistent process** (Windows). The scientific Python stack costs roughly 13 s to load on the development machine; a persistent worker pays it once per queue. Crash, timeout, cancel or malformed output discard the process and the next job starts a fresh one. The worker is released when the queue empties.
* Pause/priority: the existing `protect_audio` predicate (Performance Mode + active playback) holds queued jobs; a job running when playback starts is not interrupted but runs at below-normal priority, single-threaded (`OMP/MKL/NUMBA` threads = 1).
* The server validates every result (`validate_musical`): finite values, ranges, strictly increasing beats, ordered non-overlapping sections, source hash present. **Camelot is always recomputed server-side**; the provider's value is ignored.

## Data model (`MusicalAnalysis`, schema 1)

Stored per track in `analysis/musical/<track_id>.json`, separate from manual tags and from the basic/loudness record. A small derived `index.json` backs list views and is rebuilt if it disagrees with the directory. Fields: `provider, providerVersion, analyzedAt, sourceHash, durationMs, bpm, bpmConfidence(+Label), bpmAmbiguous, bpmAlternatives[], beatsMs[], beatGrid{firstBeatMs, intervalMs, tempo, confidence, constantTempo, segments[]}, downbeatsMs[]/downbeatsAvailable, key, scale, keyConfidence, camelot, energy(0–1), energyLevel(1–5), energyConfidence, energyCurve{hopMs, values[]}, sections[{type,startMs,endMs,confidence,energy}], warnings[], capabilities, corrections{}`.

* The beat grid is piecewise (`segments`) so tempo changes need no model change; `corrections` is a reserved empty slot for a future manual BPM/grid/key editor (not implemented).
* Manual `tags.energy` (1–5) is untouched; analysed energy is a separate continuous value.
* **Stale**: valid while `sourceHash` equals the library's `content_hash`. A mismatch (or an explicit flag) reports `stale`; data is kept and still readable until re-analysis replaces it.

## What the worker computes

* **BPM/beats**: librosa beat tracker (start 120 BPM), beats then snapped to the strongest attack in the audio (sub-3 ms on clicks) and extended over skipped head/tail beats; tempo from the audio-aligned beats. Octave handling: folded into 55–200 BPM, and **double/half alternatives are reported** (`bpmAmbiguous`, `bpmAlternatives`) with confidence capped at 0.55 (“low/medium”). Confidence = regularity + autocorrelation salience + beat pulse strength.
* **Key**: chroma-CQT with tuning estimate, Krumhansl–Kessler profile correlation, rejected (`null`) when the chroma is flat (noise/clicks) or confidence < 0.15.
* **Energy**: 0.55 loudness + 0.30 onset activity + 0.15 spectral brightness per window; scalar = mean over non-silent windows; curve capped at ≤1800 points.
* **Structure**: agglomerative segmentation of MFCC+chroma, boundaries snapped to beats, then **conservative energy-contrast labels** (`intro, build, drop, break, outro`, else `unknown`; label confidence <0.35 → `unknown`). It never emits `verse/chorus/bridge`: those cannot be derived reliably from energy alone.

## API (all under operator authentication; API tokens are excluded like the rest of `/api/plus/analysis`)

| Route | Purpose |
|---|---|
| `GET /api/plus/capabilities` → `musicalAnalysis{available, provider, providerVersion, unavailableReason, bpm, beats, beatgrid, downbeats, keyDetection, camelot, energyAnalysis, structureAnalysis, smartCueV1, sectionTypes, cueIntents[]}` | Reflects the installed provider only |
| `GET /api/plus/analysis/tracks/<id>/musical` | `{status: absent\|queued\|running\|ready\|stale\|failed, state, job, analysis}` |
| `POST /api/plus/analysis/tracks/<id>/musical` `{force?}` | Queue one track (202) |
| `POST /api/plus/analysis/musical` `{mode: selected\|missing\|stale, track_ids?, force?}` | Analyze Selected / Missing / Outdated |
| `GET /api/plus/analysis/musical/summaries` | Compact rows + queue progress for tables |
| `POST /api/plus/analysis/cancel` `{track_ids? \| all:true, provider?}` | Cancel pending and running jobs (empty scope is rejected) |
| `GET /api/plus/analysis/tracks/<id>/cue-suggestions?intents=a,b` | Smart Cue V1 proposals (`autoApply:false`) |

The existing `/api/plus/analysis/jobs` and `/tracks/<id>` routes are unchanged.

## Smart Cue V1

`CueSuggestionProvider` (`RuleBasedCueSuggestionProvider`) consumes the energy curve, sections, beats/downbeats, BPM (only if its confidence ≥ 0.3) and, as a fallback, the waveform RMS envelope. Intents: `strong-start`, `section-start`, `high-energy`, `steady-background`, `clean-outro`; custom intents register with `register_intent()`. A suggestion states its inputs (`basis`), a confidence and a reason; intents without evidence are returned under `unavailable`, never invented. Positions snap to the beat grid (downbeat grid when available). The UI offers **Preview** (plays the existing cue from the suggested IN and stops at the OUT; nothing is edited), **Apply** (sets IN/OUT/fades on the editor and the cue) and **Reject**.

## UI

Library: BPM (with confidence dot and “≈” when ambiguous), Key, Camelot, Energy, Analysis status columns; Analyze Selected / Reanalyze / Analyze Missing / Analyze Outdated / Cancel Analysis; per-track details (confidence, provider, date, beat grid, beats, downbeats, sections). The Show Control track editor shows the same panel plus waveform overlays (beats, downbeats, sections — each toggle drawn on the existing canvas as single paths with thinning to the canvas width, no per-marker DOM) and the suggestions list.

## Packaging and dependencies

* Optional: Python 3.10–3.13 + `tools/mir/requirements.txt` (`numpy`, `scipy`, `librosa==0.11.0` → numba, scikit-learn, soundfile…). On the development machine the virtualenv is about **416 MB**; the worker script is ~25 KB.
* `server/tools/mir/setup-mir.ps1` creates the environment (default `%LOCALAPPDATA%\LivePlay\mir-env`); the Electron launcher exports `LIVEPLAY_MIR_PYTHON` when that environment exists. The installer ships only the worker script, `requirements.txt` and `setup-mir.ps1`; **no Python runtime is bundled** in this phase.
* Discovery: `LIVEPLAY_MIR_WORKER` / `LIVEPLAY_MIR_PYTHON`, else `<cwd>/mir`, `tools/mir` (development), `.venv` beside the script, then `python` on `PATH` (the Microsoft Store stub is skipped). `LIVEPLAY_PLUS_MUSICAL_ANALYSIS=0` disables it; `LIVEPLAY_MIR_TIMEOUT_S` overrides the 240 s per-job timeout. FFmpeg (`LIVEPLAY_FFMPEG`, as for the other workers) is used for decoding, with a `soundfile` fallback.

## Tests

* `liveplay-musical-tests` (CTest `musical`): Camelot (all 24 keys, enharmonics, circle-of-fifths and relative-key properties), validation, store/stale/index rebuild, cue suggestions, scheduler cancel/crash/force/paused/missing-provider/asset-changed.
* `tests/mir/test_worker.py` (CTest `mir_worker`, only when the virtualenv exists): 120/90 BPM with beat alignment, double-time ambiguity, silence, noise, short track, major/minor and transposed keys, variable energy, structure schema, JSON-Lines protocol including keep-alive.
* `server/tests/e2e/musical-analysis-e2e.js`: real server + worker over HTTP — capabilities, analysis, stale, persistence, batch modes, cancellation, worker killed mid-job, missing provider.
* `client/tests/musical-tests.js`: formatting, status derivation, marker thinning, bounded overlay drawing.

## Known limits

See the Phase 2 delivery report: quality has been measured on synthetic fixtures only; no real-music accuracy figures exist yet.
