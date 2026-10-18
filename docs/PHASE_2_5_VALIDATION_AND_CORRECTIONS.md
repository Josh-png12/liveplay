# LivePlay+ Phase 2.5 — Real Music Validation & Musical Corrections

## Corrections

The detected analysis is never rewritten. `corrections` (stored beside it in `analysis/musical/<track>.json`) holds the operator's changes and the server derives `effective` values from detected + corrections:

| Correction | Stored as | Effect |
|---|---|---|
| BPM override / ×2 / ÷2 | `bpm: {mode:"custom"\|"factor", value}` | `effective.bpm`, confidence 1.0, ambiguity cleared, grid rebuilt at the new tempo |
| First beat | `firstBeatMs` (absolute) | phase of the corrected grid |
| Grid offset (left/right) | `gridOffsetMs` (±60 s, nudged server-side) | shifts every beat |
| Key / scale | `key`, `scale` (independent) | `effective.key/scale`; **Camelot recomputed** by the server |

* `effective` = `{bpm, bpmSource: detected\|factor\|custom, bpmConfidence, beatGrid, gridSource: detected\|corrected, key, scale, camelot, keySource, corrected}`. A corrected grid is a constant-tempo grid; an uncorrected one keeps the detected (possibly multi-segment) grid.
* Consumers use `effective` (`consumer_view()` for Smart Cue; `effectiveBpm/Key/Camelot` in list summaries; the waveform overlays use the effective beats). The table marks corrected rows with ✎.
* Re-analysis of the **same audio** keeps corrections; changed audio (different `sourceHash`) drops them with the stale data.
* API: `POST /api/plus/analysis/tracks/<id>/musical/corrections` with `{set:{bpm|firstBeatMs|gridOffsetMs|key|scale}, reset:"all"|"bpm"|"grid"|"key"|[..], nudgeGridMs}`. Invalid values (BPM outside 20–400, a factor without a detected BPM, unknown key/scale, grid correction with no tempo available) are rejected with 400.
* Not implemented (as requested): warp markers, multi-segment grid editing.

## Editor (Musical Insights)

BPM: detected vs effective, alternatives (click to adopt), Use ×2, Use ÷2, custom BPM, ±0.1 fine adjustment, Reset. Key: detected key, corrected key, major/minor, recalculated Camelot, Reset. Beat grid: Set first beat (from the last waveform click), nudge ◀/▶ by 1/5/10/50 ms, Reset to detected.

## Waveform timing

Cause: the editor drew peaks with `x * step` while IN/OUT markers used `time/duration*width`, so any zoom > 1 desynchronised them. Now `utils/waveformView.ts` owns the mapping (`makeView`, `timeMsToCanvasX`, `canvasXToTimeMs`, `bucketRangeForColumn`, `zoomAround`, `scrollToCenter`). Peaks (max over the buckets each pixel column covers), IN/OUT, playhead, beats, downbeats, sections, suggestion highlight and click-to-seek all use it; a horizontal scroll slider appears when zoom > 1 (zoom 1–64 supported by the transform, 1–10 offered by the slider). Tests: `client/tests/waveform-view-tests.js`.

## Real Music Validation (Library → Diagnostics)

Pick a folder inside an allowed media folder (the same jail as `/api/fs`), optionally recursive (max 2000 files). Files are analysed in place by a dedicated worker process (no import, no copy; paused while playback is protected). For each file the table shows detected BPM/confidence/alternatives, key/confidence/Camelot, energy, sections, processing time and errors, plus the Smart Cue proposals; the operator edits `expectedBpm`, `expectedKey` ("F minor", "Fm", "C#m", "8A"), notes and a Smart Cue rating (Good/Acceptable/Bad). Runs persist under `plus/validation/` (last 20) and export as JSON or CSV (`filename, duration, detectedBpm, bpmConfidence, alternatives, detectedKey, keyConfidence, camelot, energy, sections, analysisDurationMs, errors, expectedBpm, expectedKey, notes, smartCueRating, smartCueSuggestions`; CSV cells that could be read as formulas are neutralised).

Metrics (null when there is nothing to measure — never invented): exact BPM accuracy (±1 BPM), octave-equivalent accuracy (also accepting ×2 / ÷2), median BPM error (raw and octave-adjusted), exact key accuracy (a missed detection counts as wrong), Camelot-compatible accuracy (same code, ±1 on the ring, or relative major/minor), failures (listed), average processing time (also excluding the cold first file) and Smart Cue rating counts.

API: `POST/GET /api/plus/validation/runs`, `GET …/runs/<id>`, `POST …/runs/<id>/cancel`, `POST …/runs/<id>/entries/<n>`, `GET …/runs/<id>/export?format=json|csv[&wrap=1]`. API tokens are excluded like the rest of the analysis routes.

**Status of real results:** no real music was available on the development machine, so no real accuracy figures exist. The tool is functional and tested end to end on synthetic fixtures only.

## Cold-start profile (this machine, Windows 11, Python 3.12.10, librosa 0.11.0, 120 s synthetic track, median of 3)

`python tools/mir/profile_cold_start.py --repeats 3 --seconds 120`

| Stage | Time |
|---|---|
| Python interpreter start | 0.23 s |
| `import numpy` | 0.64 s |
| `import scipy.signal` | 4.56 s (dominant import cost; includes antivirus scanning) |
| `import librosa` (lazy; real work happens at first use) | 0.36 s |
| Worker startup (spawn → `health`) | 0.65 s |
| First analysis, **empty** numba cache (cold JIT) | **95.0 s** (onset 60.0 s, rhythm 28.4 s) |
| First analysis, populated numba cache, new process | **23.0 s** (onset 11.3 s, rhythm 6.5 s, key 2.0 s) |
| Warm analysis, same process | **4.9 s** (decode 0.35, onset 0.57, rhythm 1.46, key 1.67, energy 0.20, structure 0.34) |
| Explicit `warmup` request (populated cache) | 17.9 s |
| First analysis after warmup | **4.6 s** |

Safe optimisations applied (nothing touches `AudioEngine`):
1. **Persistent numba cache** in `%LOCALAPPDATA%\LivePlay\numba-cache` (the install directory may be read-only): the first-ever analysis drops from ≈95 s to ≈23 s on later cold processes.
2. **Persistent worker** (already in Phase 2): jobs after the first cost ≈5 s instead of ≈23 s.
3. **Controlled warm-up**: `POST /api/plus/analysis/musical/warmup`, called when the Library opens, runs a 24 s synthetic analysis in the worker (skipped while playback is protected, idle-released after 5 minutes). It moves ≈18 s of cold start off the first real track (23.0 s → 4.6 s measured in the same sequence). The very first run on a machine still pays the cold JIT once.

The worker also reports per-stage `timingsMs` in every result (shown in validation exports).

## Windows packaging study (for "user installs nothing")

| Option | Size added | Pros | Cons |
|---|---|---|---|
| **Managed venv** (`setup-mir.ps1`) | ≈416 MB measured (+ system Python) | Simple, upgradable, what works today | Requires Python on the machine → fails the goal |
| **Embeddable Python + pinned wheels** | Python itself ≈12 MB (embeddable zip); site-packages ≈370 MB (llvmlite 117, scipy 115, sklearn 45, numpy 34+21, numba 30) → ≈400 MB unpacked, ≈150–200 MB compressed (estimate) | No user Python; offline; reproducible with hashed lockfile; keeps the numba cache working; same worker script | Larger download; we own security updates; must build the bundle in CI |
| **PyInstaller / standalone worker** | similar size, one folder | Single executable | numba/llvmlite need hooks and often break; one-file mode re-extracts hundreds of MB on every start (worse cold start); antivirus false positives |
| **Optional MIR component** | (delivery model, not a runtime) | Keeps the main installer small; users who never analyse never download it; matches the "degradable capability" design | Needs a download/installer step and a version-compatibility check |

**Recommendation: ship the MIR engine as an *optional component* built as an embeddable-Python + pinned-wheels bundle** (hash-locked `requirements`, built in CI, installed under `%LOCALAPPDATA%\LivePlay\components\mir\`, discovered through the existing `LIVEPLAY_MIR_PYTHON`/`LIVEPLAY_MIR_WORKER` mechanism, installable from Settings with a size warning). Avoid PyInstaller for this stack. If the bundle becomes unacceptable (size, update burden), the next step is replacing librosa's few used pieces with a native C++ worker, not repackaging Python. No migration was done in this phase.

## Downbeats (not implemented; capability stays `false`)

Candidates whose licence permits general/commercial distribution — **verify the exact licence of code *and* weights before adopting**:

* **Beat This!** (CPJKU, ISMIR 2024): reported as MIT for code and published checkpoints, and it avoids madmom's DBN post-processing (peak picking). Community ONNX/Core ML ports exist. This is the most promising candidate; the licence claims above come from web search results (third-party pages), not a reviewed LICENSE file.
* **Essentia**: algorithms are AGPL-3.0 (this repository is AGPL-3.0-only, but a closed commercial distribution would need Essentia's commercial licence); its pretrained models are CC BY-NC-SA (excluded).
* **BeatNet**: licence of code/weights not confirmed in this study — treat as excluded until verified.
* **madmom**: code BSD, but the DBN trackers are patented/non-commercial and the models are CC BY-NC-SA — **excluded**.

No NonCommercial model is used or referenced by the code.

## Tests

`liveplay-musical-tests` (CTest `musical`): + corrections (×2, ÷2, custom, grid phase/nudge, key/scale override, Camelot recalculation, validation errors, store persistence/re-analysis/dropping on changed audio, consumer view and Smart Cue on the corrected grid), key-label parsing, Camelot compatibility, metrics (including null-without-expectations and octave/key cases), runner (sorting, failure isolation, expectations → metrics, JSON/CSV export, formula neutralisation, persistence, cancellation, recursion, missing provider). `tests/mir/test_worker.py`: + per-stage timings and `warmup`. `server/tests/e2e/musical-analysis-e2e.js`: + corrections API, warm-up, validation runner over HTTP. Client: `waveform-view-tests.js` (1×/2×/5×, scroll, 3-hour tracks, coverage, shared overlay/IN-OUT mapping), `musical-tests.js` (+ Camelot preview, effective beats).
