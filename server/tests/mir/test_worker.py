"""Phase 2 worker tests (analysis quality on synthetic fixtures + JSON-Lines protocol).

Run with the MIR virtualenv:  tools/mir/.venv/Scripts/python.exe -I tests/mir/test_worker.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER_DIR = os.path.join(HERE, "..", "..", "tools", "mir")
sys.path.insert(0, HERE)
sys.path.insert(0, WORKER_DIR)

import numpy as np  # noqa: E402

import fixtures  # noqa: E402
import liveplay_mir_worker as worker  # noqa: E402

failures = 0


def check(condition: bool, text: str) -> None:
    global failures
    print(("PASS " if condition else "FAIL ") + text, flush=True)
    if not condition:
        failures += 1


def beat_error_ms(detected, truth):
    detected = np.asarray(detected)
    return np.array([np.min(np.abs(detected - t)) for t in truth])


def main() -> int:
    out = tempfile.mkdtemp(prefix="mirfx-")
    manifest = fixtures.generate(out)
    result = {name: worker.analyze(os.path.join(out, name + ".wav"), "sha256:" + "0" * 64) for name in manifest}

    # ---- BPM tolerance and beat alignment on steady clicks
    for name in ("click120", "click90"):
        r, truth = result[name], manifest[name]
        check(r["bpm"] is not None and abs(r["bpm"] - truth["bpm"]) < 0.8, f"{name}: BPM {r['bpm']} within 0.8 of {truth['bpm']}")
        check(r["bpmConfidenceLabel"] == "high" and not r["bpmAmbiguous"], f"{name}: high confidence, not ambiguous")
        err = beat_error_ms(r["beatsMs"], truth["beatsMs"])
        check(len(r["beatsMs"]) >= len(truth["beatsMs"]) - 2 and np.median(err) < 10 and np.percentile(err, 90) < 20,
              f"{name}: {len(r['beatsMs'])}/{len(truth['beatsMs'])} beats, median error {np.median(err):.1f} ms")
        grid = r["beatGrid"]
        check(grid and grid["constantTempo"] and abs(grid["tempo"] - truth["bpm"]) < 0.8 and abs(grid["firstBeatMs"] - truth["beatsMs"][0]) < 15,
              f"{name}: constant beat grid starts on the first click")
        check(r["downbeatsMs"] == [] and r["downbeatsAvailable"] is False, f"{name}: downbeats are not invented")
        check(r["key"] is None and r["camelot"] is None, f"{name}: unpitched clicks have no key")

    # ---- double-time ambiguity must be reported, not hidden
    d = result["doubletime120"]
    check(d["bpmAmbiguous"] and any(a["relation"] == "double" for a in d["bpmAlternatives"]) and d["bpmConfidenceLabel"] != "high",
          f"doubletime120: ambiguity reported (alternatives {d['bpmAlternatives']}, confidence {d['bpmConfidence']})")
    check(abs(d["bpm"] - 120) < 1.0 or abs(d["bpm"] - 240) < 2.0, "doubletime120: BPM is one of the plausible octaves")

    # ---- silence, noise, short
    s = result["silence"]
    check(s["bpm"] is None and s["key"] is None and s["beatsMs"] == [] and s["sections"] == [] and s["energy"] == 0.0 and any("no signal" in w for w in s["warnings"]),
          "silence: everything null, energy 0, warned")
    n = result["noise"]
    check(n["key"] is None, "noise: no key reported")
    check(n["bpm"] is None or n["bpmConfidenceLabel"] == "low", f"noise: tempo (if any) is low confidence ({n['bpm']}, {n['bpmConfidence']})")
    sh = result["short"]
    check(sh["bpm"] is None and sh["beatsMs"] == [] and any("too short" in w for w in sh["warnings"]), "short track: no rhythm analysis, warned")
    check(0.0 <= sh["energy"] <= 1.0, "short track: energy still measured")

    # ---- key / Camelot
    for name, truth in (("major_c", manifest["major_c"]), ("minor_f", manifest["minor_f"])):
        r = result[name]
        check((r["key"], r["scale"], r["camelot"]) == (truth["key"], truth["scale"], truth["camelot"]) and r["keyConfidence"] >= 0.7,
              f"{name}: {r['key']} {r['scale']} -> {r['camelot']} (confidence {r['keyConfidence']})")
    table = {("C", "major"): "8B", ("F", "minor"): "4A", ("A", "minor"): "8A", ("C#", "minor"): "12A", ("B", "major"): "1B", ("G#", "major"): "4B"}
    check(all(worker.camelot(k, sc) == v for (k, sc), v in table.items()) and worker.camelot("C", "dorian") is None and worker.camelot(None, "major") is None,
          "worker Camelot table agrees with the server's")
    for tonic in (0, 2, 7, 9):  # transposition keeps the detected key consistent
        for minor in (False, True):
            r = worker.analyze_key(fixtures.scale_track(tonic, minor).astype(np.float32)[::2], 22050)  # 44.1k -> 22.05k
            expected = (worker.PITCHES[tonic], "minor" if minor else "major")
            check((r["key"], r["scale"]) == expected, f"transposed scale {expected}: detected {(r['key'], r['scale'])}")

    # ---- energy
    v = result["variable_energy"]
    curve = v["energyCurve"]["values"]
    check(0.0 < v["energy"] < 1.0 and all(0.0 <= x <= 1.0 for x in curve), "variable energy: scalar and curve in 0..1")
    quiet, loud = np.mean(curve[:8]), np.mean(curve[14:26])
    check(loud > quiet + 0.15, f"variable energy: loud middle ({loud:.2f}) vs quiet intro ({quiet:.2f})")
    check(result["click120"]["energy"] < v["energy"] + 0.5 and result["noise"]["energy"] > result["click90"]["energy"],
          "energy is not just peak amplitude: noise outranks sparse clicks")

    # ---- structure: schema, ordering, bounds, confidence (categories are only checked where evidence is unambiguous)
    sec = v["sections"]
    check(len(sec) >= 2 and sec[0]["startMs"] == 0, "structure: sections start at 0")
    check(all(a["endMs"] <= b["startMs"] + 1 and a["startMs"] < a["endMs"] for a, b in zip(sec, sec[1:])), "structure: ordered, non-overlapping")
    check(sec[-1]["endMs"] <= v["durationMs"] + 1 and all(0.0 <= x["confidence"] <= 1.0 for x in sec), "structure: within duration, confidence 0..1")
    check(all(x["type"] in ("intro", "build", "drop", "break", "outro", "unknown") for x in sec), "structure: only supported labels")
    check(sec[0]["type"] == "intro" and sec[-1]["type"] == "outro" and any(x["type"] == "drop" for x in sec), f"structure: quiet-loud-quiet is {[x['type'] for x in sec]}")
    check(result["noise"]["sections"] == [] or all(x["type"] == "unknown" for x in result["noise"]["sections"]), "structure: no invented labels on noise")
    check(result["short"]["sections"] == [] and result["silence"]["sections"] == [], "structure: nothing for short/silent tracks")

    # ---- every result is JSON-serialisable and bounded
    blob = json.dumps(result["click120"])
    check(len(blob) < 200_000, f"document size is bounded ({len(blob)} bytes)")

    # ---- JSON-Lines protocol, including keep-alive and error paths
    py = sys.executable
    script = os.path.join(WORKER_DIR, "liveplay_mir_worker.py")
    def run(lines, **kw):
        return subprocess.run([py, script], input="".join(json.dumps(x) + "\n" if isinstance(x, dict) else x for x in lines),
                              capture_output=True, text=True, timeout=300, **kw)
    health = run([{"protocolVersion": 1, "type": "health", "requestId": "h1"}])
    reply = json.loads(health.stdout.splitlines()[0])
    check(health.returncode == 0 and reply["status"] == "ok" and reply["requestId"] == "h1" and reply["capabilities"]["downbeats"] is False and reply["capabilities"]["bpm"] is True,
          "protocol: health reports honest capabilities")
    bad = run([{"protocolVersion": 1, "type": "analyze", "requestId": "a1", "path": os.path.join(out, "missing.wav"), "contentHash": "sha256:" + "0" * 64}])
    reply = json.loads(bad.stdout.splitlines()[0])
    check(bad.returncode == 0 and reply["status"] == "error" and reply["requestId"] == "a1" and reply["error"], "protocol: unreadable file -> error reply, clean exit")
    check(run(["not json\n"]).returncode == 3 and run([{"protocolVersion": 2, "type": "health", "requestId": "x"}]).returncode == 4 and run([{"protocolVersion": 1, "type": "weird", "requestId": "x"}]).returncode == 5,
          "protocol: malformed / wrong-version / unknown-type requests exit non-zero")
    two = run([{"protocolVersion": 1, "type": "health", "requestId": "k1", "keepAlive": True}, {"protocolVersion": 1, "type": "health", "requestId": "k2"}])
    ids = [json.loads(line)["requestId"] for line in two.stdout.splitlines()]
    check(ids == ["k1", "k2"], "protocol: keepAlive serves several requests on one process")
    one = run([{"protocolVersion": 1, "type": "health", "requestId": "o1"}, {"protocolVersion": 1, "type": "health", "requestId": "o2"}])
    check([json.loads(line)["requestId"] for line in one.stdout.splitlines()] == ["o1"], "protocol: without keepAlive the process answers once and exits")
    ok = run([{"protocolVersion": 1, "type": "analyze", "requestId": "a2", "path": os.path.join(out, "click120.wav"), "contentHash": "sha256:" + "ab" * 32}])
    reply = json.loads(ok.stdout.splitlines()[0])
    check(reply["status"] == "ok" and reply["data"]["musical"]["sourceHash"] == "sha256:" + "ab" * 32 and abs(reply["data"]["musical"]["bpm"] - 120) < 0.8,
          "protocol: analyze echoes the source hash used for cache invalidation")

    print(f"mir worker failures: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
