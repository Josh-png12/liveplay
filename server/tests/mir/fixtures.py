"""Deterministic synthetic audio fixtures for the Phase 2 musical-analysis tests.

Everything is generated from a fixed seed: no third-party audio, no licensing.
Each generator returns (samples, truth) where truth documents what the test may
assert. Usage:  python fixtures.py <output_dir>
"""
from __future__ import annotations

import json
import os
import struct
import sys
import wave

import numpy as np

SR = 44100


def write_wav(path: str, samples: np.ndarray, sr: int = SR) -> None:
    data = np.clip(samples, -1.0, 1.0)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(struct.pack("<%dh" % len(data), *(data * 32767).astype(np.int16)))


def kick(n: int, f0: float = 90.0) -> np.ndarray:
    t = np.arange(n) / SR
    return np.sin(2 * np.pi * (f0 * np.exp(-t * 18.0) + 40.0) * t * 1.0) * np.exp(-t * 22.0)


def click(n: int = 600) -> np.ndarray:
    t = np.arange(n) / SR
    return np.sin(2 * np.pi * 1800.0 * t) * np.exp(-t * 600.0)


def hat(n: int, rng: np.random.Generator) -> np.ndarray:
    return rng.standard_normal(n) * np.exp(-np.arange(n) / SR * 160.0) * 0.5


def place(buf: np.ndarray, sample: np.ndarray, start_s: float, gain: float = 1.0) -> None:
    i = int(round(start_s * SR))
    j = min(len(buf), i + len(sample))
    if i < len(buf):
        buf[i:j] += sample[: j - i] * gain


def click_track(bpm: float, seconds: float = 30.0, offset: float = 0.25):
    buf = np.zeros(int(seconds * SR))
    beats, t, c = [], offset, click()
    while t < seconds - 0.1:
        place(buf, c, t, 0.9); beats.append(t * 1000.0); t += 60.0 / bpm
    return buf, {"bpm": bpm, "beatsMs": beats}


def double_time(bpm: float = 120.0, seconds: float = 30.0):
    """Kick on every beat plus equal-weight off-beat hats: 120 reads as 240 too."""
    rng = np.random.default_rng(7)
    buf = np.zeros(int(seconds * SR)); t = 0.25; k = kick(int(0.25 * SR)); beats = []
    while t < seconds - 0.3:
        place(buf, k, t, 0.9); place(buf, hat(2000, rng), t + 30.0 / bpm, 0.9); beats.append(t * 1000.0); t += 60.0 / bpm
    return buf, {"bpm": bpm, "ambiguous": True, "beatsMs": beats}


def tonal(notes: list[float], seconds: float = 20.0):
    t = np.arange(int(seconds * SR)) / SR
    buf = np.zeros_like(t)
    for f in notes:
        for h, a in ((1, 1.0), (2, 0.5), (3, 0.25)):
            buf += a * np.sin(2 * np.pi * f * h * t)
    buf *= 0.12 / max(len(notes), 1) * 4
    return buf * np.minimum(1.0, t / 0.05)


def midi(n: int) -> float:
    return 440.0 * 2.0 ** ((n - 69) / 12.0)


def scale_track(tonic_pc: int, minor: bool, seconds: float = 24.0):
    """Cycle the scale with tonic triads emphasised, so the key is unambiguous."""
    steps = [0, 2, 3, 5, 7, 8, 10] if minor else [0, 2, 4, 5, 7, 9, 11]
    base = 60 + tonic_pc
    buf = np.zeros(int(seconds * SR)); t, i = 0.0, 0
    chord = [0, 3 if minor else 4, 7]
    while t < seconds - 0.5:
        seg = int(0.5 * SR)
        tt = np.arange(seg) / SR
        env = np.exp(-tt * 2.5) * np.minimum(1.0, tt / 0.01)
        degree = steps[i % 7]
        notes = [midi(base + degree)] + ([midi(base - 12 + c) for c in chord] if i % 2 == 0 else [])
        for f in notes:
            for h, a in ((1, 1.0), (2, 0.4), (3, 0.2)):
                place(buf, np.sin(2 * np.pi * f * h * tt) * env * a * 0.12, t)
        t += 0.5; i += 1
    return buf


def variable_energy(seconds: float = 40.0):
    """Quiet sparse intro, loud dense middle, quiet outro, on a steady 120 BPM pulse."""
    rng = np.random.default_rng(11)
    buf = np.zeros(int(seconds * SR)); k = kick(int(0.25 * SR)); t = 0.25
    while t < seconds - 0.3:
        level = 0.15 if t < 10 or t > 30 else 0.9
        place(buf, k, t, level)
        if 10 <= t <= 30:
            place(buf, hat(2000, rng), t + 0.25, 0.6)
            place(buf, scale_track(0, False, 0.5)[: int(0.5 * SR)], t, 0.8)
        t += 0.5
    return buf


def generate(out: str) -> dict:
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(1)
    manifest = {}

    def save(name: str, samples: np.ndarray, truth: dict) -> None:
        write_wav(os.path.join(out, name + ".wav"), samples)
        manifest[name] = truth

    for bpm in (120, 90):
        s, truth = click_track(bpm); save(f"click{bpm}", s, truth)
    s, truth = double_time(); save("doubletime120", s, truth)
    save("silence", np.zeros(SR * 12), {"silent": True})
    save("noise", rng.standard_normal(SR * 20) * 0.2, {"noise": True})
    save("major_c", scale_track(0, False), {"key": "C", "scale": "major", "camelot": "8B"})
    save("minor_f", scale_track(5, True), {"key": "F", "scale": "minor", "camelot": "4A"})
    save("short", click_track(120, 2.0)[0], {"short": True})
    save("variable_energy", variable_energy(), {"bpm": 120.0, "variable": True})
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f)
    return manifest


if __name__ == "__main__":
    generate(sys.argv[1] if len(sys.argv) > 1 else "fixtures")
