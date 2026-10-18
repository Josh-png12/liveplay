"""Cold-start profile of the MIR worker. Measures, separately:

  interpreter     python -c pass
  imports         numpy, scipy.signal, librosa (each in a fresh process)
  worker startup  spawn -> reply to `health` (no scientific imports are needed for it)
  first analysis  first `analyze` in a fresh worker, with an EMPTY numba cache (cold JIT) and with a POPULATED cache
  warm analysis   the next `analyze` in the same worker
  warmup          the explicit `warmup` request, and the first analysis after it

Usage: python -I profile_cold_start.py [--repeats 3] [--seconds 120] [--json out.json]
Run it with the MIR virtualenv's interpreter. Times are wall-clock milliseconds on this machine; the OS file
cache is warm after the first run, and antivirus scanning (typically the dominant cost on Windows) is included.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tests", "mir"))
WORKER = os.path.join(HERE, "liveplay_mir_worker.py")


def timed(cmd, env=None, input_text=None):
    start = time.perf_counter()
    done = subprocess.run(cmd, input=input_text, capture_output=True, text=True, env=env, timeout=900)
    return (time.perf_counter() - start) * 1000, done


class Worker:
    def __init__(self, env):
        self.start = time.perf_counter()
        self.proc = subprocess.Popen([sys.executable, WORKER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env)
        self.n = 0

    def ask(self, **fields):
        self.n += 1
        begin = time.perf_counter()
        self.proc.stdin.write(json.dumps({"protocolVersion": 1, "requestId": f"r{self.n}", "keepAlive": True, **fields}) + "\n")
        self.proc.stdin.flush()
        reply = json.loads(self.proc.stdout.readline())
        return (time.perf_counter() - begin) * 1000, reply

    def close(self):
        self.proc.stdin.close(); self.proc.wait(timeout=30)


def analyze_request(path):
    return dict(type="analyze", path=path, contentHash="sha256:" + "0" * 64)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=120.0)
    parser.add_argument("--json")
    args = parser.parse_args()
    import fixtures

    work = tempfile.mkdtemp(prefix="mir-profile-")
    samples, _ = fixtures.click_track(124, args.seconds)
    track = os.path.join(work, "profile.wav")
    fixtures.write_wav(track, samples)
    report = {"trackSeconds": args.seconds, "python": sys.version.split()[0]}

    def repeat(fn):
        values = [fn() for _ in range(args.repeats)]
        return {"medianMs": round(statistics.median(values), 1), "samplesMs": [round(v, 1) for v in values]}

    report["interpreter"] = repeat(lambda: timed([sys.executable, "-c", "pass"])[0])
    for module in ("numpy", "scipy.signal", "librosa"):
        report[f"import {module}"] = repeat(lambda m=module: timed([sys.executable, "-c", f"import {m}"])[0])

    # --- environments: persistent numba cache (the worker default) versus an empty one
    populated = dict(os.environ, NUMBA_CACHE_DIR=os.path.join(work, "cache-populated"))
    empty_factory = lambda: dict(os.environ, NUMBA_CACHE_DIR=tempfile.mkdtemp(prefix="numba-empty-"))

    def startup(env):
        w = Worker(env)
        ms, reply = w.ask(type="health")
        total = (time.perf_counter() - w.start) * 1000
        w.close()
        assert reply["status"] == "ok", reply
        return total

    report["worker startup (spawn -> health)"] = repeat(lambda: startup(populated))

    # Fill the persistent cache once (also the "cold JIT" measurement: the cache starts empty).
    w = Worker(populated); first_cold, reply = w.ask(**analyze_request(track)); assert reply["status"] == "ok", reply
    cold_timings = reply["data"]["musical"]["timingsMs"]; w.close()
    report["first analysis, EMPTY numba cache (cold JIT)"] = {"medianMs": round(first_cold, 1), "stagesMs": cold_timings}

    def first_and_second(env, pre=None):
        w = Worker(env)
        if pre:
            pre(w)
        first, r1 = w.ask(**analyze_request(track))
        second, r2 = w.ask(**analyze_request(track))
        w.close()
        assert r1["status"] == r2["status"] == "ok"
        return first, second, r1["data"]["musical"]["timingsMs"], r2["data"]["musical"]["timingsMs"]

    runs = [first_and_second(populated) for _ in range(args.repeats)]
    report["first analysis, populated cache (cold process)"] = {"medianMs": round(statistics.median(r[0] for r in runs), 1), "samplesMs": [round(r[0], 1) for r in runs], "stagesMs": runs[0][2]}
    report["warm analysis (same process)"] = {"medianMs": round(statistics.median(r[1] for r in runs), 1), "samplesMs": [round(r[1], 1) for r in runs], "stagesMs": runs[0][3]}

    warm_runs = []
    for _ in range(args.repeats):
        w = Worker(populated)
        warm_ms, reply = w.ask(type="warmup"); assert reply["status"] == "ok", reply
        first, _r = w.ask(**analyze_request(track)); w.close()
        warm_runs.append((warm_ms, first))
    report["explicit warmup request (populated cache)"] = {"medianMs": round(statistics.median(r[0] for r in warm_runs), 1)}
    report["first analysis after warmup"] = {"medianMs": round(statistics.median(r[1] for r in warm_runs), 1), "samplesMs": [round(r[1], 1) for r in warm_runs]}

    for name, value in report.items():
        if isinstance(value, dict):
            print(f"{name:52s} {value['medianMs']:>10.1f} ms" + (f"   {value['samplesMs']}" if 'samplesMs' in value else ""))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=1)


if __name__ == "__main__":
    main()
