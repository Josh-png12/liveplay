#!/usr/bin/env python3
"""LivePlay+ musical-analysis worker (provider id: ``librosa-mir``).

Runs OUTSIDE the server and outside the audio callback, speaking the same
JSON-Lines protocol v1 as the other analysis workers (one request on stdin,
one response on stdout, diagnostics on stderr).

Requests
    {"protocolVersion":1,"type":"health","requestId":"..."}
    {"protocolVersion":1,"type":"analyze","requestId":"...","trackId":"...",
     "path":"...","contentHash":"sha256:..."}

The analysis itself is delegated to librosa (ISC licence). Nothing here is a
home-grown MIR algorithm except thin post-processing: octave handling, a
tempo-segmented beat grid, key-profile correlation, a continuous energy score
and conservative energy-contrast section labelling. Anything the provider cannot
determine with evidence is returned as null/empty and its capability is
reported as false; downbeats are NOT estimated.
"""
from __future__ import annotations

import datetime
import importlib.metadata
import importlib.util
import json
import math
import os
import shutil
import subprocess
import sys
import time

# JIT cache: numba recompiles librosa's kernels on every cold process (tens of seconds) unless it can
# persist them. The install directory may be read-only, so default to a per-user writable folder.
if not os.environ.get("NUMBA_CACHE_DIR"):
    _base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".cache")
    os.environ["NUMBA_CACHE_DIR"] = os.path.join(_base, "LivePlay", "numba-cache")

# One CPU thread: the worker runs beside live playback and must not compete for cores.
for _name in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS", "NUMBA_NUM_THREADS"):
    os.environ.setdefault(_name, "1")

PROVIDER_ID = "librosa-mir"
SCHEMA_VERSION = 1
ANALYSIS_SR = 22050
HOP = 256
MAX_SECONDS = 20 * 60
MIN_SECONDS_FOR_RHYTHM = 4.0
MIN_SECONDS_FOR_STRUCTURE = 20.0

PITCHES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
# Krumhansl-Kessler key profiles.
MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]
CAMELOT_MAJOR = {"B": 1, "F#": 2, "C#": 3, "G#": 4, "D#": 5, "A#": 6, "F": 7, "C": 8, "G": 9, "D": 10, "A": 11, "E": 12}
CAMELOT_MINOR = {"G#": 1, "D#": 2, "A#": 3, "F": 4, "C": 5, "G": 6, "D": 7, "A": 8, "E": 9, "B": 10, "F#": 11, "C#": 12}

CAPABILITIES = {
    "bpm": True, "beats": True, "beatgrid": True,
    "downbeats": False,  # librosa has no downbeat tracker; we do not fake one.
    "keyDetection": True, "camelot": True, "energyAnalysis": True,
    "structureAnalysis": True,
    "sectionTypes": ["intro", "build", "drop", "break", "outro", "unknown"],
}


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def camelot(key: str | None, scale: str | None) -> str | None:
    if not key or scale not in ("major", "minor"):
        return None
    table = CAMELOT_MAJOR if scale == "major" else CAMELOT_MINOR
    number = table.get(key)
    return None if number is None else f"{number}{'B' if scale == 'major' else 'A'}"


def label_for(confidence: float | None) -> str | None:
    if confidence is None:
        return None
    return "high" if confidence >= 0.75 else "medium" if confidence >= 0.5 else "low"


def clip01(x: float) -> float:
    return float(min(1.0, max(0.0, x)))


def rnd(x: float, digits: int = 3) -> float:
    return round(float(x), digits)


def provider_version() -> str:
    try:
        return f"librosa {importlib.metadata.version('librosa')}"
    except Exception:
        return "librosa unknown"


def dependencies_available() -> tuple[bool, str]:
    for name in ("numpy", "scipy", "librosa"):
        if importlib.util.find_spec(name) is None:
            return False, f"python package '{name}' is not installed"
    return True, ""


def decode(path: str):
    """Decode to mono float32 at ANALYSIS_SR. FFmpeg first (any container), then soundfile."""
    import numpy as np

    configured = os.environ.get("LIVEPLAY_FFMPEG")
    ffmpeg = configured if configured and os.path.exists(configured) else shutil.which(configured or "ffmpeg")
    if ffmpeg:
        command = [ffmpeg, "-v", "error", "-nostdin", "-i", path, "-t", str(MAX_SECONDS), "-vn", "-ac", "1",
                   "-ar", str(ANALYSIS_SR), "-f", "f32le", "-"]
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=flags)
        if completed.returncode == 0 and completed.stdout:
            return np.frombuffer(completed.stdout, dtype="<f4").astype(np.float32, copy=True)
        log("ffmpeg decode failed: " + completed.stderr.decode("utf-8", "replace")[:300])
    import librosa

    y, _ = librosa.load(path, sr=ANALYSIS_SR, mono=True, duration=MAX_SECONDS)
    return y


def fit_beatgrid(beats_ms, bpm: float | None, confidence: float | None) -> dict | None:
    """Piecewise-constant tempo grid so tempo changes never need a model change."""
    import numpy as np

    if beats_ms is None or len(beats_ms) < 3 or not bpm:
        return None
    beats = np.asarray(beats_ms, dtype=float)
    intervals = np.diff(beats)
    median = float(np.median(intervals))
    segments, start = [], 0
    for i in range(1, len(intervals) + 1):
        window = intervals[start:i]
        if i == len(intervals) or (len(window) >= 4 and abs(intervals[i] - np.median(window)) > 0.08 * np.median(window)
                                   and i + 3 < len(intervals)
                                   and all(abs(intervals[j] - intervals[i]) < 0.05 * intervals[i] for j in range(i, min(i + 3, len(intervals))))):
            seg_beats = beats[start:i + 1]
            if len(seg_beats) >= 2:
                seg_interval = float(np.median(np.diff(seg_beats)))
                segments.append({"startMs": rnd(seg_beats[0], 1), "endMs": rnd(seg_beats[-1], 1),
                                 "bpm": rnd(60000.0 / seg_interval, 2), "beatCount": int(len(seg_beats))})
            start = i
    if not segments:
        return None
    constant = len(segments) == 1
    interval = 60000.0 / bpm
    # In a constant grid the first beat is the phase-robust median of (beat - n*interval).
    n = np.arange(len(beats))
    first = float(np.median(beats - n * interval)) if constant else float(beats[0])
    return {"firstBeatMs": rnd(max(first, 0.0), 1), "intervalMs": rnd(interval, 3), "tempo": rnd(bpm, 2),
            "confidence": rnd(confidence or 0.0, 3), "constantTempo": constant, "segments": segments,
            "medianIntervalMs": rnd(median, 3)}


def attack_envelope(y, sr: int):
    """Short-time amplitude envelope derivative (ms-scale), used to place beats on the audio."""
    import numpy as np

    smooth = max(1, int(sr * 0.003))
    env = np.convolve(np.abs(y), np.ones(smooth) / smooth, mode="same")
    return np.diff(env, prepend=env[0])


def refine_beats(y, sr: int, beats_ms, interval_ms: float):
    """Snap each tracker beat to the strongest attack nearby and recover beats the
    tracker skipped at the head/tail of a steady passage. Beats stay the tracker's
    own wherever the audio offers no clear attack."""
    import numpy as np

    if len(beats_ms) < 4:
        return list(beats_ms)
    rise = attack_envelope(y, sr)
    back, fwd = int(sr * min(0.07, interval_ms / 2000.0)), int(sr * min(0.04, interval_ms / 4000.0))

    def snap(ms: float):
        centre = int(ms * sr / 1000.0)
        lo, hi = max(0, centre - back), min(len(rise), centre + fwd)
        if hi - lo < 8:
            return None, 0.0
        window = rise[lo:hi]
        k = int(np.argmax(window))
        return (lo + k) * 1000.0 / sr, float(window[k])

    snapped = [snap(b) for b in beats_ms]
    strengths = np.array([s for _, s in snapped])
    floor = 0.2 * float(np.median(strengths)) if np.median(strengths) > 0 else 0.0
    refined = [t if t is not None and s > floor else b for (t, s), b in zip(snapped, beats_ms)]
    reference = float(np.median(strengths))
    interval = float(np.median(np.diff(refined)))

    def extend(start_ms: float, step: float, limit):
        found, t = [], start_ms + step
        while limit(t):
            ms, strength = snap(t)
            if ms is None or strength < 0.35 * reference or abs(ms - t) > 0.2 * abs(step):
                break
            found.append(ms); t = ms + step
        return found

    head = extend(refined[0], -interval, lambda t: t >= 0.0)
    tail = extend(refined[-1], interval, lambda t: t <= len(y) * 1000.0 / sr - 50.0)
    return head[::-1] + refined + tail


def analyze_rhythm(y, sr: int, onset_env, duration_s: float) -> dict:
    import librosa
    import numpy as np

    empty = {"bpm": None, "bpmConfidence": None, "bpmConfidenceLabel": None, "bpmAmbiguous": False,
             "bpmAlternatives": [], "beatsMs": [], "beatGrid": None}
    if duration_s < MIN_SECONDS_FOR_RHYTHM or float(np.max(onset_env)) <= 1e-6:
        return empty
    tempo, frames = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr, hop_length=HOP, start_bpm=120.0, tightness=100)
    tempo = float(np.atleast_1d(tempo)[0])
    if len(frames) < 4 or tempo <= 0:
        return empty
    beats_ms = librosa.frames_to_time(frames, sr=sr, hop_length=HOP) * 1000.0
    intervals = np.diff(beats_ms)
    median = float(np.median(intervals))
    # Prefer the measured median interval to the tracker's coarse tempo bins.
    bpm = 60000.0 / median if median > 0 else tempo
    if abs(bpm - tempo) / tempo > 0.05:
        bpm = tempo
    # Octave folding into a musically common range, keeping the beats consistent.
    folded = False
    while bpm < 55.0:
        bpm *= 2.0; folded = True
    while bpm > 200.0:
        bpm /= 2.0; folded = True

    beats_ms = np.asarray(refine_beats(y, sr, list(beats_ms), 60000.0 / bpm))
    intervals = np.diff(beats_ms)
    fine = 60000.0 / float(np.median(intervals))
    if abs(fine - bpm) / bpm < 0.04:  # the audio-aligned beats measure tempo better than coarse frames
        bpm = fine
    env = onset_env - np.mean(onset_env)
    ac = librosa.autocorrelate(env, max_size=int(sr * 4.5 / HOP))
    ac = ac / (ac[0] + 1e-12)

    def ac_at(bpm_value: float) -> float:
        lag = (60.0 / bpm_value) * sr / HOP
        lo, hi = int(math.floor(lag)) - 1, int(math.ceil(lag)) + 1
        if lo < 1 or hi >= len(ac):
            return 0.0
        return float(np.max(ac[lo:hi + 1]))

    own = max(ac_at(bpm), 1e-6)
    alternatives = []
    if bpm * 2 <= 260 and ac_at(bpm * 2) / own >= 0.6:
        alternatives.append({"bpm": rnd(bpm * 2, 2), "relation": "double", "support": rnd(ac_at(bpm * 2) / own, 3)})
    if bpm / 2 >= 40 and ac_at(bpm / 2) / own >= 1.25:
        alternatives.append({"bpm": rnd(bpm / 2, 2), "relation": "half", "support": rnd(ac_at(bpm / 2) / own, 3)})
    if folded:
        alternatives.append({"bpm": rnd(tempo, 2), "relation": "tracker-octave", "support": None})

    cv = float(np.std(intervals) / max(np.mean(intervals), 1e-9))
    regularity = clip01(1.0 - cv * 6.0)
    salience = clip01(own * 2.0)
    strength = float(np.mean(onset_env[np.clip(frames, 0, len(onset_env) - 1)]) / (np.mean(onset_env) + 1e-9))
    pulse = clip01((strength - 1.0) / 2.0)
    confidence = clip01(0.4 * regularity + 0.3 * salience + 0.3 * pulse)
    ambiguous = any(a["relation"] in ("double", "half") for a in alternatives)
    if ambiguous:
        confidence = min(confidence, 0.55)
    grid = fit_beatgrid(beats_ms, bpm, confidence)
    return {"bpm": rnd(bpm, 2), "bpmConfidence": rnd(confidence, 3), "bpmConfidenceLabel": label_for(confidence),
            "bpmAmbiguous": ambiguous, "bpmAlternatives": alternatives,
            "beatsMs": [rnd(b, 1) for b in beats_ms], "beatGrid": grid}


def analyze_key(y, sr: int) -> dict:
    import librosa
    import numpy as np

    empty = {"key": None, "scale": None, "keyConfidence": None, "keyConfidenceLabel": None, "camelot": None}
    if len(y) < sr * 3 or float(np.sqrt(np.mean(y ** 2))) < 1e-4:
        return empty
    tuning = float(librosa.estimate_tuning(y=y, sr=sr))
    chroma = librosa.feature.chroma_cqt(y=y, sr=sr, hop_length=512, tuning=tuning)
    profile = np.sum(chroma, axis=1)
    if float(np.max(profile)) <= 1e-6 or float(np.std(profile)) < 1e-6:
        return empty
    # Tonal clarity: noise and unpitched clicks give a nearly flat chroma profile, so
    # their best profile correlation is meaningless and must not become a "key".
    clarity = float(np.std(profile) / (np.mean(profile) + 1e-9))
    if clarity < 0.08:
        return {**empty, "tonalClarity": rnd(clarity, 3)}
    scores = []
    for tonic in range(12):
        for scale, ref in (("major", MAJOR), ("minor", MINOR)):
            r = float(np.corrcoef(profile, np.roll(ref, tonic))[0, 1])
            scores.append((r, PITCHES[tonic], scale))
    scores.sort(key=lambda s: s[0], reverse=True)
    best, second = scores[0], scores[1]
    # The relative major/minor is a legitimate near-tie in tonal music, so the
    # margin is measured against the best competitor outside that pair.
    competitor = next((s for s in scores[1:] if not (s[1] != best[1] and s[2] != best[2]
                       and abs(PITCHES.index(s[1]) - PITCHES.index(best[1])) in (3, 9))), second)
    margin = best[0] - competitor[0]
    confidence = clip01(clip01(margin / 0.12) * 0.5 + clip01((best[0] - 0.3) / 0.5) * 0.5)
    confidence *= clip01((clarity - 0.08) / 0.17)
    if confidence < 0.15:
        return {**empty, "tonalClarity": rnd(clarity, 3), "keyRejected": "no clear tonal centre"}
    return {"key": best[1], "scale": best[2], "keyConfidence": rnd(confidence, 3),
            "keyConfidenceLabel": label_for(confidence), "camelot": camelot(best[1], best[2]),
            "tonalClarity": rnd(clarity, 3),
            "keyAlternative": {"key": competitor[1], "scale": competitor[2], "correlation": rnd(competitor[0], 3)}}


def analyze_energy(y, sr: int, onset_env, duration_s: float) -> dict:
    import librosa
    import numpy as np

    hop_s = max(1.0, math.ceil(duration_s / 1800.0))
    hop = int(hop_s * sr)
    n = max(1, int(math.ceil(len(y) / hop)))
    rms_db, centroid_n, flux = [], [], []
    env_per = max(1, int(round(hop_s * sr / HOP)))
    for i in range(n):
        chunk = y[i * hop:(i + 1) * hop]
        if len(chunk) < 64:
            continue
        rms = float(np.sqrt(np.mean(chunk ** 2)))
        rms_db.append(20.0 * math.log10(max(rms, 1e-6)))
        c = librosa.feature.spectral_centroid(y=chunk, sr=sr, n_fft=1024, hop_length=1024)
        centroid_n.append(clip01((math.log2(max(float(np.mean(c)), 100.0)) - math.log2(500.0)) / (math.log2(5000.0) - math.log2(500.0))))
        seg = onset_env[i * env_per:(i + 1) * env_per]
        flux.append(float(np.mean(seg)) if len(seg) else 0.0)
    if not rms_db:
        return {"energy": None, "energyConfidence": None, "energyCurve": None}
    rms_db = np.asarray(rms_db)
    if float(np.max(rms_db)) < -70.0:
        return {"energy": 0.0, "energyConfidence": rnd(0.9, 3), "energyCurve": {"hopMs": int(hop_s * 1000), "values": [0.0] * len(rms_db)}}
    loud = np.clip((rms_db + 40.0) / 32.0, 0.0, 1.0)  # -40 dBFS..-8 dBFS
    flux = np.asarray(flux)
    flux_n = np.clip(flux / (np.percentile(flux, 95) + 1e-9), 0.0, 1.0) if np.max(flux) > 0 else np.zeros_like(flux)
    centroid = np.asarray(centroid_n)
    # Loudness dominates; onset activity and brightness separate dense/bright
    # material from sparse/dark material of equal level.
    curve = np.clip(0.55 * loud + 0.30 * flux_n + 0.15 * centroid, 0.0, 1.0)
    active = curve[rms_db > -60.0]
    energy = float(np.mean(active)) if len(active) else 0.0
    confidence = clip01(0.3 + 0.7 * min(1.0, duration_s / 30.0))
    return {"energy": rnd(energy, 3), "energyConfidence": rnd(confidence, 3),
            "energyCurve": {"hopMs": int(hop_s * 1000), "values": [rnd(v, 3) for v in curve]}}


def analyze_structure(y, sr: int, duration_s: float, beats_ms, energy: dict) -> list:
    import librosa
    import numpy as np

    curve = (energy.get("energyCurve") or {})
    values = np.asarray(curve.get("values") or [])
    if duration_s < MIN_SECONDS_FOR_STRUCTURE or len(values) < 8 or float(np.max(values)) <= 0.02:
        return []
    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13, hop_length=2048)
    chroma = librosa.feature.chroma_stft(y=y, sr=sr, hop_length=2048)
    features = librosa.util.normalize(np.vstack([librosa.util.normalize(mfcc, axis=1), chroma]), axis=0)
    features = librosa.util.sync(features, np.arange(0, features.shape[1], 4), aggregate=np.median)
    k = int(min(10, max(3, round(duration_s / 30.0))))
    if features.shape[1] < k + 1:
        return []
    bounds = librosa.segment.agglomerative(features, k)
    frame_times = np.arange(0, mfcc.shape[1], 4) * 2048.0 / sr * 1000.0
    cut_ms = sorted({0.0} | {float(frame_times[min(b, len(frame_times) - 1)]) for b in bounds})
    duration_ms = duration_s * 1000.0
    if beats_ms:
        beats = np.asarray(beats_ms)
        cut_ms = sorted({0.0 if c <= 0.0 else float(beats[int(np.argmin(np.abs(beats - c)))]) for c in cut_ms})
    cut_ms = [c for c in cut_ms if c < duration_ms - 1000.0]
    edges = cut_ms + [duration_ms]
    merged = [edges[0]]
    for e in edges[1:]:
        if e - merged[-1] >= 6000.0 or e == duration_ms:
            merged.append(e)
        elif e == duration_ms:
            merged[-1] = e
    if len(merged) < 3:
        return []
    hop_ms = float(curve["hopMs"])
    seg_energy = []
    for a, b in zip(merged[:-1], merged[1:]):
        lo, hi = int(a // hop_ms), max(int(a // hop_ms) + 1, int(b // hop_ms))
        seg_energy.append(float(np.mean(values[lo:hi])))
    mean = float(np.mean(values)) or 1e-9
    sections = []
    for i, (a, b) in enumerate(zip(merged[:-1], merged[1:])):
        e = seg_energy[i]
        ratio = e / mean
        kind, conf = "unknown", 0.0
        prev_e = seg_energy[i - 1] if i else None
        next_e = seg_energy[i + 1] if i + 1 < len(seg_energy) else None
        if i == 0 and ratio < 0.85:
            kind, conf = "intro", clip01((0.85 - ratio) / 0.35 + 0.35)
        elif i == len(seg_energy) - 1 and ratio < 0.85:
            kind, conf = "outro", clip01((0.85 - ratio) / 0.35 + 0.35)
        elif 0 < i < len(seg_energy) - 1 and ratio < 0.7 and prev_e is not None and prev_e > e * 1.25:
            kind, conf = "break", clip01((0.7 - ratio) / 0.4 + 0.3)
        elif 0 < i < len(seg_energy) - 1 and next_e is not None and next_e > e * 1.25 and ratio < 1.0:
            kind, conf = "build", clip01((next_e / max(e, 1e-6) - 1.25) / 0.6 + 0.3)
        elif i > 0 and prev_e is not None and ratio > 1.1 and e > prev_e * 1.2:
            kind, conf = "drop", clip01((ratio - 1.1) / 0.4 + 0.3)
        if conf < 0.35:
            kind = "unknown"
        sections.append({"type": kind, "startMs": rnd(a, 1), "endMs": rnd(b, 1), "confidence": rnd(conf, 3),
                         "energy": rnd(e, 3)})
    return sections


def analyze(path: str, content_hash: str) -> dict:
    timings: dict = {}
    begin = time.perf_counter()
    import librosa  # noqa: F401  (timed: this is most of a cold start)
    import numpy  # noqa: F401
    timings["importMs"] = (time.perf_counter() - begin) * 1000
    mark = time.perf_counter()
    y = decode(path)
    timings["decodeMs"] = (time.perf_counter() - mark) * 1000
    result = analyze_signal(y, content_hash, timings)
    timings["totalMs"] = (time.perf_counter() - begin) * 1000
    result["timingsMs"] = {k: rnd(v, 1) for k, v in timings.items()}
    return result


def analyze_signal(y, content_hash: str, timings: dict | None = None) -> dict:
    import librosa
    import numpy as np

    timings = timings if timings is not None else {}

    def stage(name, fn):
        mark = time.perf_counter()
        value = fn()
        timings[name] = (time.perf_counter() - mark) * 1000
        return value

    sr = ANALYSIS_SR
    duration_s = len(y) / sr
    warnings = []
    if duration_s >= MAX_SECONDS - 1:
        warnings.append(f"analysis limited to the first {MAX_SECONDS // 60} minutes")
    silent = len(y) == 0 or float(np.max(np.abs(y))) < 1e-4
    result = {
        "schemaVersion": SCHEMA_VERSION, "provider": PROVIDER_ID, "providerVersion": provider_version(),
        "analyzedAt": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "sourceHash": content_hash, "durationMs": rnd(duration_s * 1000, 1), "status": "completed",
        "stale": False, "warnings": warnings, "capabilities": CAPABILITIES,
        "downbeatsMs": [], "downbeatsAvailable": False,
    }
    if silent:
        warnings.append("no signal: audio is silent or empty")
        result.update({"bpm": None, "bpmConfidence": None, "bpmConfidenceLabel": None, "bpmAmbiguous": False,
                       "bpmAlternatives": [], "beatsMs": [], "beatGrid": None, "key": None, "scale": None,
                       "keyConfidence": None, "camelot": None, "energy": 0.0 if len(y) else None,
                       "energyConfidence": 0.9 if len(y) else None, "energyCurve": None, "sections": []})
        return result
    onset_env = stage("onsetMs", lambda: librosa.onset.onset_strength(y=y, sr=sr, hop_length=HOP))
    rhythm = stage("rhythmMs", lambda: analyze_rhythm(y, sr, onset_env, duration_s))
    if duration_s < MIN_SECONDS_FOR_RHYTHM:
        warnings.append("track too short for reliable rhythm analysis")
    elif rhythm["bpm"] is None:
        warnings.append("no rhythmic pulse found")
    key = stage("keyMs", lambda: analyze_key(y, sr))
    energy = stage("energyMs", lambda: analyze_energy(y, sr, onset_env, duration_s))
    sections = stage("structureMs", lambda: analyze_structure(y, sr, duration_s, rhythm["beatsMs"], energy))
    result.update(rhythm); result.update(key); result.update(energy); result["sections"] = sections
    return result


def warmup() -> dict:
    """Runs a short synthetic analysis so numba compiles/loads every kernel before the first real track."""
    import numpy as np

    begin = time.perf_counter()
    rng = np.random.default_rng(0)
    seconds = 24
    y = (rng.standard_normal(ANALYSIS_SR * seconds) * 0.02).astype("float32")
    for beat in range(0, seconds * 2):
        i = int(beat * 0.5 * ANALYSIS_SR)
        y[i:i + 300] += np.sin(np.linspace(0, 80, 300)).astype("float32") * 0.8
    analyze_signal(y, "sha256:" + "0" * 64, {})
    return {"warmupMs": rnd((time.perf_counter() - begin) * 1000, 1)}


def respond(request_id: str, **payload) -> None:
    print(json.dumps({"protocolVersion": 1, "requestId": request_id, **payload}, separators=(",", ":")), flush=True)


def handle(request: dict) -> int | None:
    """Answer one request. Returns an exit code only for protocol violations."""
    request_id = str(request.get("requestId", ""))
    if request.get("protocolVersion") != 1 or not request_id:
        return 4
    kind = request.get("type")
    if kind == "health":
        ok, reason = dependencies_available()
        if ok:
            respond(request_id, type="health", status="ok", provider=PROVIDER_ID, providerVersion=provider_version(),
                    capabilities=CAPABILITIES)
        else:
            respond(request_id, type="health", status="error", error=reason)
        return None
    if kind == "warmup":
        try:
            ok, reason = dependencies_available()
            if not ok:
                raise RuntimeError(reason)
            respond(request_id, type="warmup", status="ok", **warmup())
        except Exception as exc:
            respond(request_id, type="warmup", status="error", error=str(exc)[:500])
        return None
    if kind != "analyze":
        return 5
    try:
        ok, reason = dependencies_available()
        if not ok:
            raise RuntimeError(reason)
        data = analyze(str(request["path"]), str(request.get("contentHash", "")))
        respond(request_id, type="result", status="ok", data={"musical": data})
    except Exception as exc:  # report, never crash with a traceback on stdout
        log(f"analysis failed: {exc!r}")
        respond(request_id, type="result", status="error", error=str(exc)[:500])
    return None


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "--file":  # manual debugging aid
        print(json.dumps(analyze(sys.argv[2], "sha256:manual"), indent=1))
        return 0
    # One request per line. The server may keep this process alive between
    # jobs (the scientific stack costs seconds to load) and closes stdin to end it.
    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            request = json.loads(line)
        except Exception:
            return 3
        code = handle(request)
        if code is not None:
            return code
        if request.get("keepAlive") is not True:
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
